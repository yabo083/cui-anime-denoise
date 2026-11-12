#!/usr/bin/env python3
"""End-to-end drive of worker/worker.py: init -> denoise -> done -> shutdown, then init -> cancel -> shutdown.

    E:/AI/gpt-anime-denoise/.venv/Scripts/python.exe tmp/smoke_worker.py
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

ROOT = Path(__file__).resolve().parents[1]
PY = Path(r"E:\AI\gpt-anime-denoise\.venv\Scripts\python.exe")
VENV = Path(r"E:\AI\gpt-anime-denoise")
WORKER = ROOT / "worker" / "worker.py"
SRC = Path(r"E:\AI\gpt-anime-denoise\examples\furina_before_after.webp")
OUT = ROOT / "outputs"


class Proc:
    def __init__(self):
        self.p = subprocess.Popen([str(PY), "-X", "utf8", str(WORKER)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                                  bufsize=1, cwd=str(ROOT))
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
        print(f"  <- {line if len(line) < 160 else line[:150] + '...'}")
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
        try:
            self.p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.p.kill()
            raise AssertionError("worker did not exit after shutdown")


def init(p, timeout=180):
    p.send({"cmd": "init", "venv_dir": str(VENV), "device": "auto", "models_dir": None})
    ev = p.until({"ready", "error"}, timeout, "ready")
    assert ev["type"] == "ready", f"init failed: {ev}"
    return ev


def check_preview(done):
    raw = base64.b64decode(done["preview_b64"])
    from PIL import Image
    im = Image.open(io.BytesIO(raw))
    im.load()
    assert im.format == "PNG", im.format
    assert max(im.size) <= 1280, im.size
    assert min(im.size) > 0, im.size
    ratio = done["w"] / done["h"]
    assert abs(im.size[0] / im.size[1] - ratio) < 0.02, (im.size, done["w"], done["h"])
    return im.size, im.mode, len(raw)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    ok = True

    print("[1] init -> denoise -> done -> shutdown")
    p = Proc()
    try:
        ready = init(p)
        print(f"  ready: device={ready['device']} gpu={ready['gpu']} torch={ready['torch']}")
        assert ready["device"] == "cuda", f"expected cuda, got {ready}"

        t0 = time.time()
        p.send({"cmd": "denoise", "id": 1, "path": str(SRC), "scale": 0.6, "l0": 0.001,
                "out_dir": str(OUT), "preview_b64": True})
        values = []
        while True:
            ev = p.next(300, "done for job 1")
            if ev.get("type") == "progress":
                values.append(ev["value"])
                continue
            break
        assert ev["type"] == "done", f"job 1 failed: {ev}"
        wall = time.time() - t0
        assert values == sorted(values), f"progress went backwards: {values}"
        assert values and values[-1] >= 0.95, values
        assert all(0.0 <= v <= 1.0 for v in values), values

        out = Path(ev["out"])
        assert out.exists(), f"missing output {out}"
        assert out.name.endswith("_cui_denoise.png"), out.name
        assert out.parent == OUT, (out.parent, OUT)
        size, mode, nbytes = check_preview(ev)
        print(f"  done: {out.name} {ev['w']}x{ev['h']} ms={ev['ms']} wall={wall:.2f}s")
        print(f"  progress values: {values}")
        print(f"  preview: {size} {mode} {nbytes} bytes")

        p.send({"cmd": "shutdown"})
        bye = p.until({"bye"}, 30, "bye")
        assert bye["type"] == "bye", bye
        p.close()
        print("  PASS")
    except Exception as err:
        ok = False
        print(f"  FAIL: {err}")
        try:
            p.p.kill()
        except Exception:
            pass

    print("[2] init -> denoise -> cancel -> shutdown")
    p = Proc()
    try:
        ready = init(p)
        assert ready["device"] == "cuda", ready
        p.send({"cmd": "denoise", "id": 7, "path": str(SRC), "scale": 0.6, "l0": 0.001,
                "out_dir": str(OUT), "preview_b64": True})
        first = p.until({"progress", "done", "error"}, 120, "first progress")
        t0 = time.time()
        p.send({"cmd": "cancel", "id": 7})
        ev = p.until({"cancelled", "done", "error"}, 120, "cancelled")
        took = time.time() - t0
        assert ev["type"] in ("cancelled", "done"), ev
        if ev["type"] == "done":
            print(f"  note: job finished before the cancel landed ({ev['ms']} ms)")
        print(f"  {ev['type']} {took:.2f}s after cancel (first event was {first.get('type')})")

        p.send({"cmd": "shutdown"})
        bye = p.until({"bye"}, 30, "bye")
        assert bye["type"] == "bye", bye
        p.close()
        print("  PASS")
    except Exception as err:
        ok = False
        print(f"  FAIL: {err}")
        try:
            p.p.kill()
        except Exception:
            pass

    print("\nALL PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
