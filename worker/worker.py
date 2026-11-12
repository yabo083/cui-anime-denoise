#!/usr/bin/env python3
"""GPT anime-denoise worker sidecar, protocol v2.

Speaks one compact JSON object per line on stdin/stdout; stderr carries the log.

    app -> worker : {"cmd":"init","runtime":null,"device":"auto","models_dir":null}
                    {"cmd":"denoise","id":N,"path":...,"down_pct":60,"l0":0.001,"out":...}
                    {"cmd":"composite","id":N,"mask_b64":...,"out":...}
                    {"cmd":"cancel","id":N}
                    {"cmd":"models","action":"status"|"download"}
                    {"cmd":"shutdown"}
    worker -> app : {"type":"boot","stage":...,"value":0..1}        during init / model download
                    {"type":"ready","device":...,"torch":...,"gpu":...,"model_ready":...,
                     "models_dir":...,"error":null}
                    {"type":"progress","id":N,"stage":...,"value":0..1}
                    {"type":"done","id":N,"out":...,"w":...,"h":...,"ms":...,
                     "sr_cached":bool,"preview_b64":...}
                    {"type":"composited","id":N,"out":...,"preview_b64":...}
                    {"type":"cancelled","id":N}
                    {"type":"models","ready":bool,"path":...,"bytes":...,"model_ready":bool,
                     "error":null|str}
                    {"type":"error","id":N|null,"message":...}
                    {"type":"bye"}

Threads: the main thread reads the first line (always `init`) and does every import there, then starts the
stdin reader and a single job thread. On Windows a thread blocked inside stdin while another thread loads
numpy/torch's DLLs deadlocks the module loader, so nothing touches stdin until the imports are done.
The reader thread answers cancel / composite / models status itself (they must not wait behind a denoise);
denoise and model download run one at a time on the job thread. `cancel` is answered immediately: one band of
the upscaler can hold the GPU for seconds, so the job is marked, its stop() unwinds it, and whatever it
produced afterwards is discarded instead of being reported a second time.
"""
import argparse
import base64
import contextlib
import hashlib
import io
import json
import math
import os
import queue
import sys
import threading
import time
import traceback
import urllib.request
import zipfile
from collections import OrderedDict
from pathlib import Path

PREVIEW_MAX = 1600          # longest preview edge, px
PREVIEW_COMPRESS = 1        # zlib level for the preview and the saved PNG: speed over size
SR_CACHE_SIZE = 3           # super-resolution results kept, keyed by (source sha1, down_pct)
RESULT_CACHE_SIZE = 2       # finished jobs kept for composite, keyed by job id
L0_BETA_MAX = 1e5           # denoise.l0_smooth's default beta_max
L0_KAPPA = 2.0              # denoise.l0_smooth's default kappa: beta doubles every iteration
MODEL_KEY = "esrgan"        # denoise.MODELS / denoise.SHA256 entry this worker uses

_out_lock = threading.Lock()
_log_path = None
# The protocol stream, pinned at startup: denoise.fetch() prints to stdout, so it is run under
# redirect_stdout(stderr) and send() must not follow that redirect.
_stdout = sys.stdout


def log(msg):
    line = str(msg).rstrip()
    try:
        print(line, file=sys.stderr, flush=True)
    except (OSError, ValueError):
        pass
    if _log_path is not None:
        try:
            with open(_log_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except OSError:
            pass


def send(obj):
    """One JSON line, UTF-8, flushed immediately."""
    line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    with _out_lock:
        try:
            _stdout.write(line + "\n")
            _stdout.flush()
        except (OSError, ValueError):  # app went away; nothing left to tell it
            pass


def error(message, jid=None):
    """A protocol error: the job id it belongs to, or null for a worker-level problem."""
    send({"type": "error", "id": jid, "message": message})


def quiet(fn, *args, **kw):
    """Run an upstream helper with its stdout pointed at stderr: denoise.fetch prints "downloading ..."."""
    with contextlib.redirect_stdout(sys.stderr):
        return fn(*args, **kw)


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def make_preview(img, alpha):
    """PNG base64 of an image, longest edge <= PREVIEW_MAX, alpha kept when the source had it."""
    from PIL import Image

    im = img if img.mode == "RGB" else img.convert("RGB")
    if alpha is not None:
        im = im.copy()
        im.putalpha(alpha)
    if max(im.size) > PREVIEW_MAX:
        im = im.copy()
        im.thumbnail((PREVIEW_MAX, PREVIEW_MAX), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "PNG", compress_level=PREVIEW_COMPRESS)
    return base64.b64encode(buf.getvalue()).decode("ascii")


class Progress:
    """Per-job progress line. Monotone, coalesced to 1% steps; `force` sends every one (L0 iterations)."""

    def __init__(self, jid):
        self.jid = jid
        self.stage = None
        self.value = 0.0

    def emit(self, stage, value, force=False):
        value = min(1.0, max(0.0, float(value)))
        if not force and stage == self.stage and value < self.value + 0.01 and value < 1.0:
            return
        value = max(value, self.value)  # never go backwards, even across a stage change
        self.stage, self.value = stage, value
        send({"type": "progress", "id": self.jid, "stage": stage, "value": round(value, 3)})


class Worker:
    def __init__(self, args):
        self.args = args
        self.mod = None                 # the denoise module, imported on init
        self.np = None
        self.torch = None
        self.device = None
        self.models = {}                # key -> loaded model
        self.root = None                # folder holding denoise.py
        self.models_dir = None
        self.model_ready = False        # weights loaded in memory
        self.model_error = None         # why model_ready is false
        self.verified = {}              # str(path) -> (size, mtime_ns) whose sha256 already matched
        # job bookkeeping: `pending` holds queued jobs so a cancel can reach them before they start
        self.pending = {}
        self.current = None
        self.job_lock = threading.Lock()
        self.cache_lock = threading.Lock()
        self.sr_cache = OrderedDict()   # (sha1, down_pct) -> full-res RGB uint8 array
        self.results = OrderedDict()    # job id -> (rgb, alpha, icc, source path)
        self.jobs = queue.Queue()       # work for the job thread: init / denoise / model download
        self.closing = False
        self.closed = threading.Event()

    # -- setup ---------------------------------------------------------------

    def boot(self, stage, value):
        send({"type": "boot", "stage": stage, "value": round(min(1.0, max(0.0, value)), 3)})

    def resolve_root(self, cmd):
        """Folder holding denoise.py: --denoise-root, ANIME_DENOISE_ROOT, init runtime, --venv, sys.prefix."""
        tried = []
        for cand in (self.args.denoise_root, os.environ.get("ANIME_DENOISE_ROOT"), cmd.get("runtime"),
                     cmd.get("venv_dir"), self.args.venv, sys.prefix):
            if not cand:
                continue
            base = Path(str(cand))
            for folder in (base, base.parent):  # a venv lives one level below the project root
                tried.append(str(folder))
                if (folder / "denoise.py").is_file():
                    return folder
        log(f"denoise.py not found; tried: {', '.join(tried) or 'nothing'}")
        error("cannot find denoise.py; set --denoise-root or ANIME_DENOISE_ROOT "
              f"(tried: {', '.join(tried) or 'nothing'})")
        return None

    def init(self, cmd):
        self.boot("resolve runtime", 0.02)
        root = self.resolve_root(cmd)
        if root is None:
            return
        self.root = root
        if str(root) not in sys.path:
            sys.path.insert(0, str(root))

        self.boot("import torch", 0.10)
        try:
            import denoise
            import numpy as np
            import torch
            import spandrel  # noqa: F401  denoise.load_model imports it; load it here, on this thread
            from PIL import Image, ImageCms, ImageOps  # noqa: F401  previews and composite use these
        except BaseException as err:  # a broken runtime, not a job failure: the app cannot proceed
            log(traceback.format_exc())
            error(f"import failed from {root}: {err}")
            return
        self.mod, self.np, self.torch = denoise, np, torch

        self.boot("probe device", 0.45)
        device = str(cmd.get("device") or "auto")
        if device == "auto":
            try:
                device = denoise.default_device()
            except Exception as err:
                log(traceback.format_exc())
                error(f"device probe failed: {err}")
                return
        self.device = device

        self.models_dir = Path(self.args.models or cmd.get("models_dir") or (root / "models")).expanduser()

        self.boot("check model", 0.55)
        ok, why = self.verify_model()
        if ok:
            self.boot("load model", 0.65)
            try:
                self.models = {MODEL_KEY: quiet(denoise.load_model, MODEL_KEY, self.models_dir, device)}
                self.model_ready = True
                self.model_error = None
            except BaseException as err:  # bad file, no spandrel, no VRAM: app may re-download
                log(traceback.format_exc())
                self.model_error = f"model load failed ({self.model_path()}): {err}"
        else:
            self.model_error = why
        self.boot("ready" if self.model_ready else "model missing", 0.98)

        if device.startswith("cuda"):
            try:
                gpu = self.torch.cuda.get_device_name(0)
            except Exception:
                gpu = "CUDA"
        elif device.startswith("mps"):
            gpu = "Apple MPS"
        else:
            gpu = "CPU"
        ready = {"type": "ready", "device": device, "torch": self.torch.__version__, "gpu": gpu,
                 "model_ready": self.model_ready, "models_dir": str(self.models_dir),
                 "error": self.model_error}
        note = getattr(denoise, "GPU_NOTE", "")
        if note:
            ready["note"] = note
        send(ready)
        log(f"ready: device={device} gpu={gpu} models={self.models_dir} model_ready={self.model_ready}")

    # -- model management ----------------------------------------------------

    def model_path(self):
        name = self.mod.MODELS[MODEL_KEY][0]
        return self.models_dir / name

    def verify_model(self):
        """(ok, why): the weights are on disk and match denoise.SHA256 (hashed once per file version)."""
        path = self.model_path()
        try:
            st = path.stat()
        except OSError:
            return False, f"model not downloaded: {path}"
        stamp = (st.st_size, st.st_mtime_ns)
        if self.verified.get(str(path)) == stamp:
            return True, None
        h = hashlib.sha256()
        try:
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h.update(chunk)
        except OSError as err:
            return False, f"cannot read {path}: {err}"
        want = self.mod.SHA256.get(path.name)
        if want and h.hexdigest() != want:
            return False, f"model file is corrupt (sha256 mismatch): {path}"
        self.verified[str(path)] = stamp
        return True, None

    def models_status(self):
        if self.mod is None:
            error("worker not initialized; send init first")
            return
        ok, why = self.verify_model()
        path = self.model_path()
        try:
            size = path.stat().st_size
        except OSError:
            size = 0
        send({"type": "models", "ready": ok, "path": str(path), "bytes": size,
              "model_ready": self.model_ready, "error": None if ok else why})

    def models_download(self):
        if self.mod is None:
            error("worker not initialized; send init first")
            return
        try:
            path = self.download_model()
        except (Exception, SystemExit) as err:  # denoise.fetch raises SystemExit on a checksum miss
            log(traceback.format_exc())
            error(f"model download failed: {err}")
            return
        if not self.model_ready and self.device is not None:
            self.boot("load model", 0.95)
            try:
                self.models = {MODEL_KEY: quiet(self.mod.load_model, MODEL_KEY, self.models_dir,
                                                self.device)}
                self.model_ready = True
                self.model_error = None
            except BaseException as err:
                log(traceback.format_exc())
                self.model_error = f"model load failed ({path}): {err}"
        self.models_status()

    def download_model(self):
        """Fetch the weights, reporting byte progress; verifies against denoise.SHA256 like denoise.fetch."""
        name, url = self.mod.MODELS[MODEL_KEY]
        path = self.models_dir / name
        ok, why = self.verify_model()
        if ok:
            self.boot("model present", 1.0)
            return path
        log(f"downloading {url} -> {path} ({why})")
        self.models_dir.mkdir(parents=True, exist_ok=True)
        self.boot("download model", 0.0)
        try:
            data = self.download_stream(url, name)
            if url.endswith(".zip"):
                with zipfile.ZipFile(io.BytesIO(data)) as z:
                    data = z.read(next(n for n in z.namelist() if n.endswith(name)))
            got = hashlib.sha256(data).hexdigest()
            want = self.mod.SHA256.get(name)
            if want and got != want:
                raise ValueError(f"checksum mismatch for {name}: got {got[:12]}..., expected {want[:12]}...")
            tmp = path.with_name(path.name + ".part")
            tmp.write_bytes(data)
            os.replace(tmp, path)
        except Exception as err:
            # denoise.fetch does the same job in one blocking read and its own checksum check
            log(f"streamed download failed ({err}); falling back to denoise.fetch")
            quiet(self.mod.fetch, MODEL_KEY, self.models_dir)
        self.boot("download model", 0.9)
        self.verified.pop(str(path), None)
        ok, why = self.verify_model()
        if not ok:
            raise RuntimeError(why)
        return path

    def download_stream(self, url, name):
        """Same bytes as denoise.fetch, read in chunks so the app can show progress."""
        req = urllib.request.Request(url, headers={"User-Agent": "gpt-anime-denoise"})
        buf = io.BytesIO()
        with urllib.request.urlopen(req) as resp:
            total = int(resp.headers.get("Content-Length") or 0)
            while True:
                chunk = resp.read(1 << 16)
                if not chunk:
                    break
                buf.write(chunk)
                got = buf.tell()
                self.boot("download model", 0.9 * got / total if total else min(0.85, got / 2e7))
        log(f"downloaded {buf.tell()} bytes for {name}")
        return buf.getvalue()

    # -- jobs ----------------------------------------------------------------

    def handle(self, kind, job, cmd):
        if kind == "init":
            self.init(cmd)
        elif kind == "denoise":
            self.denoise(job, cmd)
        elif kind == "models":
            self.models_download()
        elif kind == "shutdown":
            self.shutdown()

    def reply(self, job, obj):
        """Send a job's terminal line at most once: a cancel answered by the reader thread must not be
        followed by a second `cancelled`, and a job that finished just before the cancel keeps its `done`."""
        with self.job_lock:
            if job["replied"]:
                return False
            job["replied"] = True
        send(obj)
        return True

    def cancel(self, jid):
        # Answered here, on the reader thread: a band of the upscaler can hold the GPU for seconds, and the
        # app must not wait for it. The job's stop() sees the event and unwinds on its own; anything it
        # produces afterwards is discarded.
        with self.job_lock:
            current = self.current
            queued = self.pending.pop(jid, None)  # a job still waiting in the queue is dropped here
        job = current if (current is not None and current["id"] == jid) else queued
        if job is None:
            send({"type": "error", "id": jid, "message": f"no such job: {jid}"})
            return
        self.drop_result(jid)
        job["cancel"].set()
        if self.reply(job, {"type": "cancelled", "id": jid}):
            log(f"cancel {jid}: {'signalled' if job is current else 'queued'}")

    def drop_result(self, jid):
        with self.cache_lock:
            self.results.pop(jid, None)

    def denoise(self, job, cmd):
        jid = job["id"]

        def fail(msg):
            self.reply(job, {"type": "error", "id": jid, "message": msg})

        if job["cancel"].is_set():
            self.reply(job, {"type": "cancelled", "id": jid})
            return
        if self.mod is None:
            fail("worker not initialized; send init first")
            return
        if not self.model_ready:
            fail(self.model_error or "model not ready; send models download")
            return
        try:
            src_path = Path(str(cmd["path"]))
            down_pct = float(cmd.get("down_pct", self.mod.DOWN * 100))
            l0 = float(cmd.get("l0", self.mod.L0))
        except (KeyError, TypeError, ValueError) as err:
            fail(f"bad denoise command: {err}")
            return
        out = str(cmd.get("out") or "")
        if not out:
            fail("denoise needs an out path")
            return
        out = Path(out)
        if not 0.0 < down_pct <= 100.0:
            fail(f"down_pct must be in (0, 100], got {down_pct}")
            return
        if not l0 >= 0.0:
            fail(f"l0 must be 0 or more, got {l0}")
            return
        if not src_path.is_file():
            fail(f"no such image: {src_path}")
            return

        with self.job_lock:
            self.pending.pop(jid, None)
            self.current = job
        t0 = time.perf_counter()
        try:
            self.run_denoise(job, jid, src_path, out, down_pct, l0, t0)
        except BaseException as err:  # includes denoise.Cancelled, checked below
            if isinstance(err, getattr(self.mod, "Cancelled", ())):
                self.drop_result(jid)
                self.reply(job, {"type": "cancelled", "id": jid})
            else:
                log(traceback.format_exc())
                fail(f"{type(err).__name__}: {err}")
        finally:
            with self.job_lock:
                self.current = None

    def run_denoise(self, job, jid, src_path, out, down_pct, l0, t0):
        from PIL import Image

        mod, np, device, cancel = self.mod, self.np, self.device, job["cancel"]
        prog = Progress(jid)
        prog.emit("decode", 0.02)

        # Super-resolution cache: same file bytes and same downscale -> same result. The key hashes the file,
        # so a re-encoded or edited source misses; the value is a plain uint8 array (a third of a PIL copy).
        key = (sha1_file(src_path), round(down_pct))
        with self.cache_lock:
            rgb = self.sr_cache.get(key)
            if rgb is not None:
                self.sr_cache.move_to_end(key)
        sr_cached = rgb is not None

        # The alpha and colour profile the PNG needs still come from the source (and prove its size).
        src, alpha, icc = mod.open_image(str(src_path))
        prog.emit("decode", 0.10)

        if rgb is None:
            # upscale() checks `stop` once per memory-sized band; that is the only progress hook it has.
            bands = [0]

            def sr_stop():
                if cancel.is_set():
                    return True
                bands[0] += 1
                prog.emit("upscale", 0.12 + 0.53 * (1.0 - 0.72 ** bands[0]))
                return False

            old_down = mod.DOWN
            mod.DOWN = down_pct / 100.0
            try:
                sr = mod.sr_at(self.models[MODEL_KEY], src, device, stop=sr_stop)
            finally:
                mod.DOWN = old_down
            rgb = np.asarray(sr, np.uint8)
            del sr
            with self.cache_lock:
                self.sr_cache[key] = rgb
                self.sr_cache.move_to_end(key)
                while len(self.sr_cache) > SR_CACHE_SIZE:
                    self.sr_cache.popitem(last=False)
        prog.emit("upscale", 0.68)

        if l0 > 0:
            # l0_smooth runs one stop() per beta iteration and beta doubles from 2*lam up to beta_max, so the
            # count is known up front and every iteration gets its own line.
            total = max(1, int(math.ceil(math.log(L0_BETA_MAX / (2.0 * l0)) / math.log(L0_KAPPA))))
            step = 0.25 / total
            seen = [0]

            def l0_stop():
                if cancel.is_set():
                    return True
                seen[0] += 1
                prog.emit("smooth", 0.70 + step * seen[0], force=True)
                return False

            arr = mod.to_arr(Image.fromarray(rgb))
            smooth = mod.l0_smooth(arr, l0, device=device, stop=l0_stop)
            del arr
            img = mod.to_img(smooth)
            del smooth
            final = np.asarray(img, np.uint8)
            prog.emit("smooth", 0.95)
        else:
            img = Image.fromarray(rgb)
            final = rgb

        if cancel.is_set():
            raise mod.Cancelled()
        w, h = img.size
        prog.emit("encode", 0.97)
        out.parent.mkdir(parents=True, exist_ok=True)
        mod.save_png(img, str(out), alpha, icc, compress_level=PREVIEW_COMPRESS)

        preview = None
        try:
            preview = make_preview(img, alpha)
        except Exception:  # a preview is a nicety; never fail the job over it
            log(traceback.format_exc())

        # composite re-reads the source, so all it needs here is the result and the source's extras
        with self.cache_lock:
            self.results[jid] = (final, alpha, icc, str(src_path))
            self.results.move_to_end(jid)
            while len(self.results) > RESULT_CACHE_SIZE:
                self.results.popitem(last=False)

        ms = int((time.perf_counter() - t0) * 1000)
        prog.emit("encode", 1.0)
        done = {"type": "done", "id": jid, "out": str(out), "w": w, "h": h, "ms": ms,
                "sr_cached": sr_cached}
        if preview is not None:
            done["preview_b64"] = preview
        if not self.reply(job, done):  # the cancel was answered while this saved: leave nothing behind
            self.drop_result(jid)
            try:
                out.unlink()
            except OSError:
                pass
            log(f"job {jid} finished after a cancel; output discarded")
            return
        log(f"done {jid}: {out} {w}x{h} {ms} ms sr_cached={sr_cached}")

    # -- composite (reader thread: must answer while a denoise is running) ----

    def composite(self, cmd):
        jid = cmd.get("id")
        if not isinstance(jid, int) or isinstance(jid, bool):
            error("composite needs an integer id")
            return
        if self.mod is None:
            send({"type": "error", "id": jid, "message": "worker not initialized; send init first"})
            return
        with self.cache_lock:
            entry = self.results.get(jid)
            if entry is not None:
                self.results.move_to_end(jid)
        if entry is None:
            send({"type": "error", "id": jid,
                  "message": f"no cached result for job {jid}; run denoise again"})
            return
        out = str(cmd.get("out") or "")
        if not out:
            send({"type": "error", "id": jid, "message": "composite needs an out path"})
            return
        rgb, alpha, icc, src_path = entry
        h, w = rgb.shape[0], rgb.shape[1]
        try:
            mask = decode_mask(cmd.get("mask_b64"))
        except Exception as err:
            send({"type": "error", "id": jid, "message": f"bad mask: {err}"})
            return
        if mask.size != (w, h):
            send({"type": "error", "id": jid,
                  "message": f"mask is {mask.size[0]}x{mask.size[1]}, result is {w}x{h}"})
            return
        try:
            orig, _alpha, _icc = self.mod.open_image(src_path)
        except Exception as err:
            self.drop_result(jid)
            send({"type": "error", "id": jid, "message": f"cannot re-read {src_path}: {err}"})
            return
        np = self.np
        den = np.asarray(rgb, np.int32)
        src = np.asarray(orig, np.int32)
        if src.shape != den.shape:
            self.drop_result(jid)
            send({"type": "error", "id": jid,
                  "message": f"source is {src.shape[1]}x{src.shape[0]} now, result is {w}x{h}"})
            return
        send({"type": "progress", "id": jid, "stage": "composite", "value": 0.5})
        m = np.asarray(mask, np.int32)[:, :, None]
        # out = den + (orig - den) * m / 255, rounded to the nearest level
        mix = (den * 255 + (src - den) * m + 127) // 255
        from PIL import Image

        img = Image.fromarray(np.clip(mix, 0, 255).astype(np.uint8))
        preview = None
        try:
            path = Path(out)
            path.parent.mkdir(parents=True, exist_ok=True)
            self.mod.save_png(img, str(path), alpha, icc, compress_level=PREVIEW_COMPRESS)
            preview = make_preview(img, alpha)
        except Exception as err:
            log(traceback.format_exc())
            send({"type": "error", "id": jid, "message": f"composite failed: {err}"})
            return
        reply = {"type": "composited", "id": jid, "out": out}
        if preview is not None:
            reply["preview_b64"] = preview
        send(reply)
        log(f"composited {jid}: {out}")

    # -- lifecycle -----------------------------------------------------------

    def shutdown(self):
        if self.closing:
            return  # a second shutdown (or EOF after one) must not send a second bye
        self.closing = True
        with self.job_lock:
            if self.current is not None:
                self.current["cancel"].set()
        send({"type": "bye"})
        self.closed.set()

    def wait_idle(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            with self.job_lock:
                if self.current is None:
                    return True
            time.sleep(0.02)
        return False


def decode_mask(text):
    """8-bit greyscale PIL image from a base64 PNG mask."""
    from PIL import Image

    if not text or not isinstance(text, str):
        raise ValueError("mask_b64 is missing")
    try:
        raw = base64.b64decode(text, validate=False)
    except Exception as err:
        raise ValueError(f"mask_b64 is not base64: {err}")
    im = Image.open(io.BytesIO(raw))
    im.load()
    return im.convert("L")


def handle_line(worker, line):
    """Parses one JSONL command. cancel / composite / models status are answered right here, so they can
    reach a denoise that is already running; returns the (kind, job, cmd) for the job thread, or None."""
    line = line.strip()
    if not line:
        return None
    try:
        cmd = json.loads(line)
    except ValueError as err:
        error(f"bad json: {err}")
        return None
    if not isinstance(cmd, dict):
        error("command must be a JSON object")
        return None
    name = cmd.get("cmd")
    if name == "cancel":
        try:
            worker.cancel(int(cmd.get("id")))
        except (TypeError, ValueError):
            error("cancel needs an integer id")
        return None
    if name == "composite":
        worker.composite(cmd)
        return None
    if name == "models":
        action = cmd.get("action")
        if action == "status":
            worker.models_status()
            return None
        if action == "download":
            return ("models", None, cmd)
        error(f"models needs action status or download, got {action!r}")
        return None
    if name == "denoise":
        jid = cmd.get("id")
        if not isinstance(jid, int) or isinstance(jid, bool):
            error("denoise needs an integer id")
            return None
        job = {"id": jid, "cancel": threading.Event(), "replied": False}
        with worker.job_lock:
            worker.pending[jid] = job
        return ("denoise", job, cmd)
    if name == "init":
        return ("init", None, cmd)
    if name == "shutdown":
        worker.shutdown()
        return None
    if isinstance(name, str):
        error(f"unknown cmd: {name}")
        return None
    error("command needs a cmd field")
    return None


def reader(worker):
    try:
        while True:
            line = sys.stdin.readline()  # explicit readline, not iteration: no read-ahead in the way
            if not line:
                break
            item = handle_line(worker, line)
            if item is not None:
                worker.jobs.put(item)
    except (OSError, ValueError):
        pass
    finally:
        worker.shutdown()  # app went away: stop the running job and let the process end


def job_loop(worker):
    while True:
        item = worker.jobs.get()
        if item is None:
            return
        kind, job, cmd = item
        try:
            worker.handle(kind, job, cmd)
        except BaseException as err:
            log(traceback.format_exc())
            msg = {"type": "error", "id": job["id"] if job else None,
                   "message": f"{type(err).__name__}: {err}"}
            if job is None:
                send(msg)
            else:
                worker.reply(job, msg)


def main(argv):
    global _log_path, _stdout
    ap = argparse.ArgumentParser(description="anime-denoise worker sidecar (JSONL on stdin/stdout)")
    ap.add_argument("--denoise-root", default=None, help="folder holding denoise.py")
    ap.add_argument("--venv", default=None, help="python environment or project folder (fallback)")
    ap.add_argument("--models", default=None, help="folder holding the ESRGAN weights")
    ap.add_argument("--log", default=None, help="also append the worker log to this file")
    args = ap.parse_args(argv)
    if args.log:
        _log_path = str(Path(args.log).expanduser())

    for stream, kw in ((sys.stdout, {"line_buffering": True, "write_through": True}),
                       (sys.stdin, {})):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace", **kw)
        except (AttributeError, ValueError):
            pass
    _stdout = sys.stdout

    worker = Worker(args)
    # The app opens with `init`: run it here, while no thread is blocked in stdin (see the module docstring).
    first = sys.stdin.readline()
    if first:
        item = handle_line(worker, first)
        if item is not None:
            worker.handle(*item)

    if not worker.closing:
        threading.Thread(target=reader, args=(worker,), name="stdin", daemon=True).start()
        threading.Thread(target=job_loop, args=(worker,), name="jobs", daemon=True).start()
        worker.closed.wait()
        worker.wait_idle(2.0)  # let a cancelled job unwind before the process goes away

    with _out_lock:  # let the last line land before the process exits
        try:
            sys.stdout.flush()
        except (OSError, ValueError):
            pass
    os._exit(0)


if __name__ == "__main__":
    main(sys.argv[1:])
