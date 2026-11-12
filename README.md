# cui-anime-denoise

Windows desktop tool that strips screen-tone moiré ("鱼鳞纹") from AI-generated anime art.
The UI is native C drawn by [CUI](https://github.com/yappologistic/CUI); image work runs in
a Python sidecar that wraps the `gpt-anime-denoise` denoiser.

## Status

Working v1: drop or browse images → serial denoise queue → before/after compare →
PNG output in `outputs/` (next to the exe). GPU badge, per-item cancel, worker-crash
restart, strength presets (Soft/Balanced/Strong) plus Scale %/L0 sliders.
Brush/lasso masking is not implemented (`# TODO mask` in `src/main.c`).

## Architecture

```
┌─────────────────────────────┐   JSON Lines over stdio    ┌──────────────────────────┐
│ denoise_app.exe             │ ─────────────────────────► │ Python worker sidecar    │
│  CUI (immediate-mode UI, C) │ ◄───────────────────────── │  torch + denoise.py      │
│  src/main.c, src/backend.c  │  stdout: one JSON per line │  (RTX 5060 Ti / CUDA)    │
└─────────────────────────────┘                            └──────────────────────────┘
```

- **Front end** — `src/main.c` describes the window every frame with CUI components. No
  widget tree, no retainer; a panel is a function call.
- **Bridge** — `src/backend.c` spawns the worker with `CreateProcessW` and anonymous pipes,
  writes commands to its stdin and drains its stdout on a reader thread into a locked queue.
  Paths travel as UTF-8; `backend.h` exposes `wb_start / wb_push_denoise / wb_cancel /
  wb_poll / wb_shutdown / wb_kill`.
- **Worker** — a Python script that imports `denoise` from `E:\AI\gpt-anime-denoise`,
  loads the ESRGAN model once, and answers each command with progress then a result.

Protocol (one JSON object per line, stderr left for worker logs):

| App → worker | Reply |
| --- | --- |
| `{"cmd":"init","venv_dir":…,"device":"auto","models_dir":null}` | `{"type":"ready","device":"cuda","torch":…,"gpu":…}` |
| `{"cmd":"denoise","id":N,"path":…,"scale":0.6,"l0":0.001,"out_dir":…,"preview_b64":true}` | `{"type":"progress","id":N,"value":0.42}` … `{"type":"done","id":N,"out":…,"preview_b64":…,"w":…,"h":…,"ms":…}` |
| `{"cmd":"cancel","id":N}` | `{"type":"cancelled","id":N}` |
| `{"cmd":"shutdown"}` | `{"type":"bye"}`, then exit |

The worker calls `denoise.sr_at(...)` and `denoise.l0_smooth(...)` directly (not `denoise.run`,
whose `lam`/`stop` arguments cannot be wired through), patching `denoise.DOWN` around the call.
`done` carries `preview_b64` (result) and `src_preview_b64` (source, for formats CUI cannot load).
## Build

Requirements: Visual Studio with the C++ x64 toolset (MSVC 14.44 here), and PowerShell 7.
`toolchain.ps1` finds both itself — nothing needs to be on `PATH` and no
"Developer Command Prompt" is needed.

```powershell
pwsh -NoProfile -File build.ps1          # Release, Ninja, into build\
pwsh -NoProfile -File build.ps1 -Run     # build, then start the window
pwsh -NoProfile -File build.ps1 -Clean -Config Debug
```

Result: `build\denoise_app.exe` (290 KB, Release).

`toolchain.ps1` can also be dot-sourced into an existing session (`. .\toolchain.ps1`), or run
on its own to print the compilers it resolved. From a POSIX shell (git-bash), call
`pwsh -NoProfile -File build.ps1`; do not try to import the MSVC environment into bash — the
captured `PATH` holds Windows entries that bash will not resolve.

How it activates MSVC: `vcvarsall.bat` is a `cmd.exe` script whose environment dies with the
process, so `toolchain.ps1` runs one `cmd /c "call vcvarsall.bat x64 && set"`, then copies
every `NAME=VALUE` line into the PowerShell process environment. CMake and Ninja come from
the copies shipped with Visual Studio.

## Directory layout

```
cui-anime-denoise/
├─ CMakeLists.txt        top-level build: vendored CUI + denoise_app
├─ build.ps1             one-command configure + build (Ninja, single config)
├─ toolchain.ps1         MSVC x64 detection/activation, PATH for cmake + ninja
├─ src/
│  ├─ main.c             CUI window and (next) the denoise panel
│  ├─ backend.h/.c       worker process bridge: spawn, JSONL, event queue
├─ worker/               Python sidecar (JSONL worker script)
├─ third_party/CUI/      vendored CUI 1.4.0 (git history and docs screenshots removed)
├─ build/                CMake + Ninja output (ignored)
├─ outputs/              denoised PNGs and previews (ignored)
└─ tmp/                  scratch scripts (ignored)
```

## Notes

- `third_party/CUI` is a plain copy of the upstream tree with `.git` and `docs/images`
  removed; it is treated as read-only.
- The build is warning-clean for project sources. MSVC prints `D9025 (/W3 overridden by /W4)`
  for CUI's own files — that comes from CUI's CMakeLists, not from this project.
