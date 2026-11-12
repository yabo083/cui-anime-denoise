#!/usr/bin/env python3
"""GPT anime-denoise worker sidecar.

Speaks one compact JSON object per line on stdin/stdout; stderr is free-form log output.

    app -> worker : {"cmd":"init","venv_dir":..., "device":"auto", "models_dir":null}
                    {"cmd":"denoise","id":N,"path":...,"scale":0.6,"l0":0.001,"out_dir":...,"preview_b64":true}
                    {"cmd":"cancel","id":N}
                    {"cmd":"shutdown"}
    worker -> app : {"type":"ready"|"progress"|"done"|"error"|"cancelled"|"bye", ...}

The first line (always `init`) is read on the main thread and the stdin reader thread starts only after
the imports: on Windows a thread blocked inside stdin while numpy/torch map their DLLs deadlocks the
loader, so nothing here blocks on stdin until every module and the model are loaded.
"""
import base64
import io
import json
import math
import os
import queue
import sys
import threading
import time
import traceback
from pathlib import Path

PREVIEW_MAX = 1280          # longest preview edge, px
L0_BETA_MAX = 1e5           # denoise.l0_smooth's default beta_max, used to predict its iteration count
DEFAULT_MODELS = Path.home() / ".cache" / "gpt-anime-denoise"

_out_lock = threading.Lock()


def log(msg):
    try:
        print(msg, file=sys.stderr, flush=True)
    except Exception:
        pass


def send(obj):
    """One JSON line, UTF-8, flushed immediately."""
    line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    with _out_lock:
        try:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()
        except (OSError, ValueError):  # app went away; nothing left to tell it
            pass


class Worker:
    def __init__(self):
        self.mod = None                 # the denoise module, imported on init
        self.torch = None
        self.device = None
        self.models = {}
        self.models_dir = None
        # job bookkeeping: `pending` holds queued jobs so a cancel can reach them before they start
        self.pending = {}
        self.current = None
        self.lock = threading.Lock()
        self.commands = queue.Queue()
        self.sr_cache = None            # (key, image) of the last super-resolution result
        self.closing = False

    # -- job control ---------------------------------------------------------

    def cancel(self, jid):
        with self.lock:
            current = self.current
            queued = self.pending.get(jid)
        if current is not None and current["id"] == jid:
            current["cancel"].set()
            log(f"cancel {jid}: signalled")
        elif queued is not None:
            queued["cancel"].set()
            log(f"cancel {jid}: queued job")
        else:
            send({"type": "error", "id": jid, "message": f"no such job: {jid}"})

    def handle(self, kind, job, cmd):
        if kind == "init":
            self.init(cmd)
        elif kind == "denoise":
            self.denoise(job, cmd)
        elif kind == "shutdown":
            self.shutdown()

    # -- commands ------------------------------------------------------------

    def init(self, cmd):
        venv_dir = cmd.get("venv_dir")
        if venv_dir:
            venv_dir = str(venv_dir)
            if venv_dir not in sys.path:
                sys.path.insert(0, venv_dir)
        try:
            import denoise
            import torch
            import spandrel  # denoise.load_model imports it; pull it in here, off the reader thread
            from PIL import Image, ImageCms, ImageOps  # noqa: F401  (make_preview uses these)
        except Exception as err:
            log(traceback.format_exc())
            send({"type": "error", "message": f"import failed: {err}"})
            return
        self.mod, self.torch = denoise, torch

        device = str(cmd.get("device") or "auto")
        if device == "auto":
            try:
                device = denoise.default_device()
            except Exception as err:
                send({"type": "error", "message": f"device probe failed: {err}"})
                return
        models_dir = Path(cmd["models_dir"]) if cmd.get("models_dir") else DEFAULT_MODELS

        try:
            models = {"esrgan": denoise.load_model("esrgan", models_dir, device)}
        except BaseException as err:  # bad download, checksum miss, no spandrel: app may retry
            log(traceback.format_exc())
            send({"type": "error", "message": f"model load failed ({models_dir}): {err}"})
            return

        self.device, self.models, self.models_dir = device, models, models_dir
        self.sr_cache = None
        if device.startswith("cuda"):
            gpu = torch.cuda.get_device_name(0)
        elif device.startswith("mps"):
            gpu = "Apple MPS"
        else:
            gpu = "CPU"
        ready = {"type": "ready", "device": device, "torch": torch.__version__, "gpu": gpu}
        note = getattr(denoise, "GPU_NOTE", "")
        if note:
            ready["note"] = note
        send(ready)

    def denoise(self, job, cmd):
        jid = job["id"]
        if job["cancel"].is_set():
            send({"type": "cancelled", "id": jid})
            return
        if self.mod is None or "esrgan" not in self.models:
            send({"type": "error", "id": jid, "message": "worker not initialized; send init first"})
            return
        try:
            path = str(cmd["path"])
            scale = float(cmd.get("scale", self.mod.DOWN))
            l0 = float(cmd.get("l0", self.mod.L0))
        except (KeyError, TypeError, ValueError) as err:
            send({"type": "error", "id": jid, "message": f"bad denoise command: {err}"})
            return
        if not 0.0 < scale <= 1.0:
            send({"type": "error", "id": jid, "message": f"scale must be in (0, 1], got {scale}"})
            return
        if not l0 >= 0.0:
            send({"type": "error", "id": jid, "message": f"l0 must be 0 or more, got {l0}"})
            return
        out_dir = Path(cmd.get("out_dir") or Path(path).parent)

        with self.lock:
            self.pending.pop(jid, None)
            self.current = job
        t0 = time.perf_counter()
        try:
            self._denoise(job, cmd, jid, path, scale, l0, out_dir, t0)
        except BaseException as err:  # includes denoise.Cancelled, checked below
            if isinstance(err, getattr(self.mod, "Cancelled", ())):
                send({"type": "cancelled", "id": jid})
            else:
                log(traceback.format_exc())
                send({"type": "error", "id": jid, "message": f"{type(err).__name__}: {err}"})
        finally:
            with self.lock:
                self.current = None

    def _denoise(self, job, cmd, jid, path, scale, l0, out_dir, t0):
        mod, device, cancel = self.mod, self.device, job["cancel"]
        last = [0.0]

        def emit(value):
            value = min(1.0, max(0.0, value))
            if value <= last[0] and value < 1.0:
                return
            if value < last[0] + 0.01 and value < 1.0:
                return
            last[0] = value
            send({"type": "progress", "id": jid, "value": round(value, 3)})

        emit(0.02)
        src, alpha, icc = mod.open_image(path)
        w, h = src.size
        emit(0.05)

        # Downscale -> ESRGAN x4 -> back to the input size. sr_at reads the module-global DOWN at call time
        # and hands the stop callback to upscale, which checks it before every band.
        key = (path, os.stat(path).st_mtime_ns, w, h, scale)
        sr = None
        if self.sr_cache is not None and self.sr_cache[0] == key:
            sr = self.sr_cache[1]
            emit(0.70)
        else:
            def sr_stop():
                if cancel.is_set():
                    return True
                emit(min(0.66, last[0] + 0.14))
                return False

            emit(0.10)
            old_down = mod.DOWN
            mod.DOWN = scale
            try:
                sr = mod.sr_at(self.models["esrgan"], src, device, stop=sr_stop)
            finally:
                mod.DOWN = old_down
            self.sr_cache = (key, sr)
            emit(0.70)

        # L0 gradient minimisation: one stop() call per beta iteration, and beta doubles until beta_max,
        # so the total is known up front and the bar steps once per iteration.
        if l0 > 0:
            total = max(1, int(math.ceil(math.log(L0_BETA_MAX / (2.0 * l0)) / math.log(2.0))))
            step = 0.25 / total
            seen = [0]

            def l0_stop():
                if cancel.is_set():
                    return True
                seen[0] += 1
                emit(0.70 + step * seen[0])
                return False

            img = mod.to_img(mod.l0_smooth(mod.to_arr(sr), l0, device=device, stop=l0_stop))
        else:
            img = sr
        emit(0.95)

        if cancel.is_set():
            raise mod.Cancelled()

        out_dir.mkdir(parents=True, exist_ok=True)
        out_path = out_dir / (Path(path).stem + "_cui_denoise.png")
        mod.save_png(img, str(out_path), alpha, icc)
        emit(0.98)

        preview = None
        if cmd.get("preview_b64", True):
            try:
                preview = make_preview(img, alpha)
            except Exception:  # a preview is a nicety; never fail the job over it
                log(traceback.format_exc())
        src_preview = None
        try:
            src_preview = make_preview(src, alpha)
        except Exception:
            log(traceback.format_exc())
        emit(1.0)

        done = {"type": "done", "id": jid, "out": str(out_path), "w": w, "h": h,
                "ms": int((time.perf_counter() - t0) * 1000)}
        if preview is not None:
            done["preview_b64"] = preview
        if src_preview is not None:
            done["src_preview_b64"] = src_preview
        send(done)

    def shutdown(self):
        self.closing = True
        with self.lock:
            if self.current is not None:
                self.current["cancel"].set()
        send({"type": "bye"})


def make_preview(img, alpha):
    """PNG base64 of the result, longest edge <= PREVIEW_MAX, alpha kept when the source had it."""
    from PIL import Image

    im = img if img.mode == "RGB" else img.convert("RGB")
    if alpha is not None:
        im = im.copy()
        im.putalpha(alpha)
    if max(im.size) > PREVIEW_MAX:
        im = im.copy()
        im.thumbnail((PREVIEW_MAX, PREVIEW_MAX), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


def handle_line(worker, line):
    """Parses one JSONL command. `cancel` is acted on here so it can reach a job already running;
    returns the (kind, job, cmd) the main thread should run, or None."""
    line = line.strip()
    if not line:
        return None
    try:
        cmd = json.loads(line)
    except ValueError as err:
        send({"type": "error", "message": f"bad json: {err}"})
        return None
    if not isinstance(cmd, dict):
        send({"type": "error", "message": "command must be a JSON object"})
        return None
    name = cmd.get("cmd")
    if name == "cancel":
        try:
            worker.cancel(int(cmd.get("id")))
        except (TypeError, ValueError):
            send({"type": "error", "message": "cancel needs an integer id"})
        return None
    if name == "denoise":
        jid = cmd.get("id")
        if not isinstance(jid, int):
            send({"type": "error", "message": "denoise needs an integer id"})
            return None
        job = {"id": jid, "cancel": threading.Event()}
        with worker.lock:
            worker.pending[jid] = job
        return ("denoise", job, cmd)
    if isinstance(name, str):
        return (name, None, cmd)
    send({"type": "error", "message": "command needs a cmd field"})
    return None


def reader(worker):
    try:
        while True:
            line = sys.stdin.readline()  # explicit readline, not iteration: no read-ahead in the way
            if not line:
                break
            item = handle_line(worker, line)
            if item is not None:
                worker.commands.put(item)
    except (OSError, ValueError):
        pass
    finally:
        worker.commands.put(None)  # stdin closed: stop once the current job is done


def main():
    for stream, kw in ((sys.stdout, {"line_buffering": True, "write_through": True}),
                       (sys.stdin, {})):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace", **kw)
        except (AttributeError, ValueError):
            pass

    worker = Worker()
    # The app opens with `init`: run it here, while no thread is blocked in stdin (see the module docstring).
    first = sys.stdin.readline()
    if first:
        item = handle_line(worker, first)
        if item is not None:
            worker.handle(*item)

    if not worker.closing:
        threading.Thread(target=reader, args=(worker,), name="stdin", daemon=True).start()
        while True:
            item = worker.commands.get()
            if item is None:
                break
            kind, job, cmd = item
            if kind in ("init", "denoise", "shutdown"):
                worker.handle(kind, job, cmd)
                if kind == "shutdown":
                    break
            else:
                out = {"type": "error", "message": f"unknown cmd: {kind}"}
                if isinstance(cmd.get("id"), int):
                    out["id"] = cmd["id"]
                send(out)

    with _out_lock:  # let the last line land before the process exits
        try:
            sys.stdout.flush()
        except (OSError, ValueError):
            pass
    os._exit(0)


if __name__ == "__main__":
    main()
