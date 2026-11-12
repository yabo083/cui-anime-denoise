"""Protocol stub for backend.c tests: speaks the JSONL worker protocol with a
fixed 4x4 PNG preview and no torch dependency."""
import sys
import json
import base64
import threading
import time

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8", errors="replace")

PNG_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAGUlEQVR42mP4f6LhvwAQw2gGZA6I"
    "ZiCoAgDuayzpEKTJMgAAAABJRU5ErkJggg=="
)

_lock = threading.Lock()
_cancelled = set()


def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(msg):
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def run_job(msg):
    jid = msg.get("id", 0)
    for step in range(1, 6):
        time.sleep(0.08)
        with _lock:
            if jid in _cancelled:
                emit({"type": "cancelled", "id": jid})
                return
        emit({"type": "progress", "id": jid, "value": step / 5.0})
    with _lock:
        if jid in _cancelled:
            emit({"type": "cancelled", "id": jid})
            return
    out_dir = msg.get("out_dir") or "."
    emit({
        "type": "done",
        "id": jid,
        "out": out_dir.rstrip("/\\") + "/stub_out.png",
        "preview_b64": PNG_B64,
        "w": 4,
        "h": 4,
        "ms": 480,
    })


def main():
    log("stub worker started")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except Exception as exc:  # noqa: BLE001
            emit({"type": "error", "id": 0, "message": f"bad json: {exc}"})
            continue
        cmd = msg.get("cmd")
        if cmd == "init":
            log(f"init venv_dir={msg.get('venv_dir')!r} device={msg.get('device')!r}")
            emit({
                "type": "ready",
                "device": "cuda",
                "torch": "2.15.0+cu134",
                "gpu": "NVIDIA GeForce RTX 5060 Ti (stub)",
            })
        elif cmd == "denoise":
            log(f"denoise id={msg.get('id')} path={msg.get('path')!r} scale={msg.get('scale')} l0={msg.get('l0')}")
            threading.Thread(target=run_job, args=(msg,), daemon=True).start()
        elif cmd == "cancel":
            with _lock:
                _cancelled.add(msg.get("id"))
        elif cmd == "shutdown":
            log("shutdown")
            emit({"type": "bye"})
            return
        else:
            emit({"type": "error", "id": 0, "message": f"unknown command {cmd!r}"})
    log("stdin closed")


if __name__ == "__main__":
    main()
