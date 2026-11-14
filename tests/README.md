# tests

Manual checks. Nothing here is part of the application build (CMake only
compiles `src/`); each driver is a plain C program you build with `cl` from the
repository root.

## test_mask.c — the protection mask (src/mask.c)

Covers the brush geometry (round caps, hard edges, overwrite instead of
accumulate), lasso fill (nonzero winding, pixel centres at +0.5, 1 px inside
outline, sub-4 px polygons ignored), the tile-journal undo/redo pair with its
byte budget, feathering, the edge map / live wire / snap trio, the PNG round
trip, and the 4096x4096 timings from the acceptance list.

Build and run the core checks (no WIC, so the PNG case prints `SKIP`):

```
pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
  New-Item -ItemType Directory -Force tmp\maskbuild | Out-Null;
  cl /nologo /utf-8 /W4 /O2 /I src /c tests\test_mask.c   /Fo:tmp\maskbuild\test_mask.obj;
  cl /nologo /utf-8 /W4 /O2 /I src /c src\mask.c          /Fo:tmp\maskbuild\mask.obj;
  cl /nologo /utf-8 /W4 /O2 /I src /c tests\img_stub.c    /Fo:tmp\maskbuild\img_stub.obj;
  link /nologo tmp\maskbuild\test_mask.obj tmp\maskbuild\mask.obj tmp\maskbuild\img_stub.obj
    /OUT:tmp\maskbuild\test_mask.exe"
tmp\maskbuild\test_mask.exe
```

`tests/img_stub.c` only exists so that `mask_save`/`mask_load` link without
WIC; every stub fails, which is what turns the PNG case into a `SKIP`.

Build the same test against the real image IO (`src/img.c`) to run the PNG
round trip as well:

```
pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
  cl /nologo /utf-8 /W4 /O2 /I src /c src\img.c /Fo:tmp\maskbuild\img.obj;
  cl /nologo /utf-8 /W4 /O2 /I src /c src\img_common.c /Fo:tmp\maskbuild\img_common.obj;
  link /nologo tmp\maskbuild\test_mask.obj tmp\maskbuild\mask.obj tmp\maskbuild\img.obj
    tmp\maskbuild\img_common.obj
    ole32.lib oleaut32.lib uuid.lib windowscodecs.lib bcrypt.lib shell32.lib
    shlwapi.lib advapi32.lib user32.lib gdi32.lib /OUT:tmp\maskbuild\test_mask_png.exe"
tmp\maskbuild\test_mask_png.exe
```

Both drivers must be started from the repository root: the PNG case writes
`tmp/mask_roundtrip.png`. Exit code 0 = every check passed; each failing check
prints `FAIL` with a one-line description.

`test_mask.c` also reads the internal journal size through a plain `extern`
declaration (`mask_hist_bytes_used`, defined in `src/mask.c`). It is
deliberately not in `mask.h`: the contract header only carries what the
application needs.

## test_models.c — the model-pair logic (src/engine/models.c)

Covers the states the settings surface renders, without a network: the
same-directory pair invariant (a .pth and a skeleton in different dirs is
NOT available), the skip-the-network repair when weights already verify,
the skeleton copy + rollback that ends a real download, and delete. The
update_io / CUI helpers models.c borrows are stubbed in the file; build
lines are in its header comment.

## test_denoiser.c — the in-process inference worker (src/denoiser.c)

Covers the thread lifecycle (start -> READY, restart, shutdown), job
queueing, progress monotonicity, cancellation, the two-entry SR cache
(re-run with only l0 changed must report `sr_cached`), and event ownership.
Needs the staged `build\runtime` + `build\models` (or the `DN_RUNTIME_DIR` /
`DN_MODELS_DIR` env overrides) and the real engine. Build lines are in the
file's header comment.

## test_config.c — settings, log, crash handler, diagnostics

The driver links the two halves of the config module (`src/config.c` for the
settings and paths, `src/log.c` for the rotating log, the crash report and the
diagnostics zip):

```
pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
  New-Item -ItemType Directory -Force tmp\configbuild | Out-Null;
  cl /nologo /utf-8 /W4 /O2 /I src /c tests\test_config.c /Fo:tmp\configbuild\test_config.obj;
  cl /nologo /utf-8 /W4 /O2 /I src /c src\config.c      /Fo:tmp\configbuild\config.obj;
  cl /nologo /utf-8 /W4 /O2 /I src /c src\log.c         /Fo:tmp\configbuild\log.obj;
  link /nologo tmp\configbuild\test_config.obj tmp\configbuild\config.obj tmp\configbuild\log.obj
    ole32.lib shell32.lib advapi32.lib user32.lib gdi32.lib
    /OUT:tmp\configbuild\test_config.exe"
tmp\configbuild\test_config.exe
```

Started from the repository root. The run is hermetic: it points
`ANIME_DENOISE_DATA_DIR` at `<exe dir>\cfgtest` and deletes that tree on exit.
The diagnostics case shells out to `python` (`zipfile.testzip()`), so a Python
on `PATH` is required; the crash case re-launches the same executable with
`--crash-child` and expects exit code `0xC0000005`.

## test_img.c — WIC decode/encode parity + host compositing (src/img.c)

Format-parity cases (alpha, ICC, orientation, 16-bit, CMYK) plus
`img_composite_masked`: the host-side blend that replaced the Python
worker's protect-region composite -- mask 0/255 endpoints, feathered
interior values, alpha passthrough. Build lines are in the file's header
comment.

## test_engine.c — the native engine's deterministic maths (src/engine/)

Covers Python round-half-even sizing, Pillow-identical BOX/LANCZOS output
bytes, the L0 pass against reference fixture bytes, the fixed-window tiling
math (including the 256-window cap), and cancel semantics. No ONNX Runtime or
network needed -- sessions are exercised by the `tmp/phase2` parity driver
instead. The exact build lines are in the file's header comment; run the
resulting `test_engine.exe` from anywhere (exit 0 = all checks passed).
