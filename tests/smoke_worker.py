#!/usr/bin/env python3
"""End-to-end drive of worker/worker.py, protocol v2.

    E:/AI/gpt-anime-denoise/.venv/Scripts/python.exe tests/smoke_worker.py

Walks the whole protocol: init -> models status -> denoise (cold) -> denoise (same file, SR cache hit) ->
composite with a hand-made mask -> cancel a long job -> shutdown.
Runs the worker with its own venv python and writes everything under outputs/ (gitignored), so the
upstream project at E:\\AI\\gpt-anime-denoise is only read.
"""
import base64
import io
import json
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
PY = Path(r"E:\AI\gpt-anime-denoise\.venv\Scripts\python.exe")
DENOISE_ROOT = Path(r"E:\AI\gpt-anime-denoise")
MODELS = Path.home() / ".cache" / "gpt-anime-denoise"
WORKER = ROOT / "worker" / "worker.py"
SRC = DENOISE_ROOT / "examples" / "furina_before_after.webp"
OUT = ROOT / "outputs" / "smoke_v2"
LOG = OUT / "worker.log"
STAGES = {"decode", "upscale", "smooth", "encode", "composite"}


class Proc:
    def __init__(self):
        self.err = open(OUT / "stderr.txt", "wb")
        self.p = subprocess.Popen([str(PY), "-X", "utf8", str(WORKER), "--denoise-root", str(DENOISE_ROOT),
                                   "--models", str(MODELS), "--log", str(LOG)],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err,
                                  text=True, encoding="utf-8", errors="replace", bufsize=1,
                                  cwd=str(ROOT))
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            line = self.p.stdout.readline()
            if not line:
                break
            line = line.strip()
            if line:
                self.lines.put(line)
        self.lines.put(None)

    def send(self, obj):
        self.p.stdin.write(json.dumps(obj) + "\n")
        self.p.stdin.flush()

    def next(self, timeout, label):
        try:
            line = self.lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f"timed out after {timeout}s waiting for {label}")
        if line is None:
            raise AssertionError(f"worker closed stdout while waiting for {label} (rc={self.p.poll()})")
        ev = json.loads(line)
        shown = line if len(line) < 200 else line[:190] + "..." + line[-8:]
        print(f"  <- {shown}")
        return ev

    def until(self, types, timeout, label):
        end = time.time() + timeout
        while True:
            ev = self.next(max(0.1, end - time.time()), label)
            if ev.get("type") in types:
                return ev

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        rc = self.p.wait(timeout=20)
        self.err.close()
        return rc


def init(p, timeout=240):
    t0 = time.time()
    p.send({"cmd": "init", "runtime": str(DENOISE_ROOT), "device": "auto", "models_dir": str(MODELS)})
    boots = []
    while True:
        ev = p.next(timeout, "ready")
        if ev["type"] == "boot":
            boots.append((ev["stage"], ev["value"]))
            continue
        assert ev["type"] == "ready", f"init failed: {ev}"
        values = [v for _s, v in boots]
        assert values == sorted(values), f"boot progress went backwards: {boots}"
        assert all(0.0 <= v <= 1.0 for v in values), boots
        print(f"  boot: {boots}")
        print(f"  ready after {time.time() - t0:.1f}s: {ev}")
        return ev


def denoise(p, jid, path, down_pct, l0, out, timeout=300):
    """Sends a denoise and returns (done event, [(stage, value)...], wall seconds)."""
    t0 = time.time()
    p.send({"cmd": "denoise", "id": jid, "path": str(path), "down_pct": down_pct, "l0": l0,
            "out": str(out)})
    steps = []
    while True:
        ev = p.next(timeout, f"done for job {jid}")
        if ev.get("type") == "progress":
            assert ev["id"] == jid, ev
            assert ev["stage"] in STAGES, ev
            steps.append((ev["stage"], ev["value"]))
            continue
        break
    wall = time.time() - t0
    assert ev["type"] == "done", f"job {jid} failed: {ev}"
    return ev, steps, wall


def check_steps(steps):
    values = [v for _s, v in steps]
    assert values == sorted(values), f"progress went backwards: {steps}"
    assert all(0.0 <= v <= 1.0 for v in values), steps
    assert values and values[-1] >= 0.95, steps
    return values


def check_preview(done, w, h):
    raw = base64.b64decode(done["preview_b64"])
    im = Image.open(io.BytesIO(raw))
    im.load()
    assert im.format == "PNG", im.format
    assert max(im.size) == 1600, im.size
    assert abs(im.size[0] / im.size[1] - w / h) < 0.01, (im.size, w, h)
    assert len(raw) > 1000, len(raw)
    return im.size, im.mode, len(raw)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    if LOG.exists():
        LOG.unlink()
    for stale in OUT.glob("*.png"):
        stale.unlink()
    ok = True

    print(f"[1] worker {WORKER} with {PY}")
    print(f"    source {SRC} ({SRC.stat().st_size} bytes), models {MODELS}")
    p = Proc()
    try:
        ready = init(p)
        assert ready["device"] == "cuda", ready
        assert ready["model_ready"] is True, ready
        assert ready["error"] is None, ready
        assert ready["models_dir"] == str(MODELS), ready

        p.send({"cmd": "models", "action": "status"})
        ev = p.until({"models", "error"}, 60, "models status")
        assert ev["type"] == "models", ev
        assert ev["ready"] is True, ev
        weight = MODELS / "RealESRGAN_x4plus_anime_6B.pth"
        assert Path(ev["path"]) == weight, ev
        assert ev["bytes"] == weight.stat().st_size, ev
        print(f"  models: {ev['bytes']} bytes, model_ready={ev['model_ready']}")

        print("[2] denoise cold (down_pct=60, l0=0.001)")
        out1 = OUT / "a_denoise.png"
        done, steps, wall = denoise(p, 1, SRC, 60, 0.001, out1)
        check_steps(steps)
        assert done["sr_cached"] is False, done
        assert Path(done["out"]) == out1 and out1.exists(), done["out"]
        assert (done["w"], done["h"]) == (2096, 1586), done
        assert isinstance(done["ms"], int) and done["ms"] > 0, done
        size, mode, nbytes = check_preview(done, done["w"], done["h"])
        print(f"  done: {out1.name} {done['w']}x{done['h']} ms={done['ms']} wall={wall:.2f}s")
        print(f"  stages: {sorted({s for s, _v in steps})} ({len(steps)} progress lines)")
        print(f"  preview: {size} {mode} {nbytes} bytes")

        print("[3] denoise again, same file and params -> SR cache hit")
        out2 = OUT / "a_denoise_2.png"
        done2, steps2, wall2 = denoise(p, 2, SRC, 60, 0.001, out2)
        check_steps(steps2)
        assert done2["sr_cached"] is True, done2
        assert done2["ms"] < done["ms"] * 0.5, (done2["ms"], done["ms"])
        assert (done2["w"], done2["h"]) == (done["w"], done["h"]), done2
        a = np.asarray(Image.open(out1).convert("RGB"), np.int16)
        b = np.asarray(Image.open(out2).convert("RGB"), np.int16)
        diff = int(np.abs(a - b).max())
        assert diff == 0, f"cached result differs by {diff} levels"
        print(f"  done: ms={done2['ms']} wall={wall2:.2f}s (cold {done['ms']} ms, wall {wall:.2f}s),"
              f" identical output (max diff {diff})")

        print("[3b] same file, cached SR, l0=0 -> the whole job is decode + encode")
        done3, steps3, wall3 = denoise(p, 3, SRC, 60, 0, OUT / "a_denoise_nol0.png")
        check_steps(steps3)
        assert done3["sr_cached"] is True, done3
        assert done3["ms"] < 1000, done3
        print(f"  done: ms={done3['ms']} wall={wall3:.2f}s")

        print("[4] composite: left half protected (mask 255) = original, right half = denoised")
        w, h = done2["w"], done2["h"]
        mask = Image.new("L", (w, h), 0)
        mask.paste(255, (0, 0, w // 2, h))
        buf = io.BytesIO()
        mask.save(buf, "PNG")
        mask_b64 = base64.b64encode(buf.getvalue()).decode("ascii")

        p.send({"cmd": "composite", "id": 9999, "mask_b64": mask_b64, "out": str(OUT / "nope.png")})
        bad = p.until({"composited", "error"}, 60, "composite for a stale id")
        assert bad["type"] == "error" and bad["id"] == 9999, bad
        print(f"  stale id rejected: {bad['message']}")

        p.send({"cmd": "composite", "id": 9999, "mask_b64": "!!!notbase64", "out": str(OUT / "n.png")})
        bad = p.until({"composited", "error"}, 60, "composite with a bad mask")
        assert bad["type"] == "error", bad

        out3 = OUT / "a_denoise_protected.png"
        t0 = time.time()
        p.send({"cmd": "composite", "id": 2, "mask_b64": mask_b64, "out": str(out3)})
        ev = p.until({"composited", "error"}, 120, "composited")
        took = time.time() - t0
        assert ev["type"] == "composited", ev
        assert Path(ev["out"]) == out3 and out3.exists(), ev
        assert ev["preview_b64"], ev
        res = np.asarray(Image.open(out3).convert("RGB"), np.int16)
        orig = np.asarray(ImageOps.exif_transpose(Image.open(SRC)).convert("RGB"), np.int16)
        den = np.asarray(Image.open(out2).convert("RGB"), np.int16)
        assert res.shape == orig.shape == den.shape, (res.shape, orig.shape, den.shape)
        left = int(np.abs(res[:, : w // 2] - orig[:, : w // 2]).max())
        right = int(np.abs(res[:, w // 2:] - den[:, w // 2:]).max())
        assert left <= 1, f"protected half differs from the source by {left}"
        assert right <= 1, f"masked-out half differs from the denoised image by {right}"
        pim = Image.open(io.BytesIO(base64.b64decode(ev["preview_b64"])))
        pim.load()
        print(f"  composited in {took * 1000:.0f} ms: left max diff {left}, right max diff {right},"
              f" {out3.name} {Image.open(out3).size}, preview {pim.size}")

        print("[5] cancel: long job (down_pct=80, cold), cancel once the upscaler is running")
        t0 = time.time()
        p.send({"cmd": "denoise", "id": 9, "path": str(SRC), "down_pct": 80, "l0": 0.001,
                "out": str(OUT / "cancelled.png")})
        while True:
            ev = p.next(300, "cancel job progress")
            assert ev["type"] != "done", f"job finished before the cancel landed: {ev}"
            assert ev["type"] != "error", ev
            if ev.get("stage") == "upscale":
                break
        tc = time.time()
        p.send({"cmd": "cancel", "id": 9})
        ev = p.until({"cancelled", "done", "error"}, 120, "cancelled")
        cancel_ms = (time.time() - tc) * 1000
        assert ev["type"] == "cancelled", ev
        assert ev["id"] == 9, ev
        assert cancel_ms < 500, f"cancel took {cancel_ms:.0f} ms"
        print(f"  cancelled in {cancel_ms:.1f} ms ({time.time() - t0:.1f}s after start)")

        print("[6] the worker still works after a cancel: cached denoise queues behind the unwind")
        done4, steps4, wall4 = denoise(p, 10, SRC, 60, 0.001, OUT / "after_cancel.png", timeout=180)
        check_steps(steps4)
        assert done4["sr_cached"] is True, done4
        assert Path(done4["out"]).exists(), done4
        assert not (OUT / "cancelled.png").exists(), "cancelled job still wrote a file"
        print(f"  done: {done4['ms']} ms (wall {wall4:.2f}s), no cancelled.png on disk")

        p.send({"cmd": "shutdown"})
        bye = p.until({"bye"}, 30, "bye")
        assert bye["type"] == "bye", bye
        rc = p.close()
        assert rc == 0, f"worker exit code {rc}"
        assert LOG.exists() and LOG.stat().st_size > 0, "worker --log file is empty"
        print(f"  bye, exit code {rc}, log {LOG.name} {LOG.stat().st_size} bytes")
    except Exception as err:
        ok = False
        print(f"  FAIL: {type(err).__name__}: {err}")
        try:
            p.p.kill()
        except Exception:
            pass
        p.err.close()

    print("\nALL PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
