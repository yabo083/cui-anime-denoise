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
  link /nologo tmp\maskbuild\test_mask.obj tmp\maskbuild\mask.obj tmp\maskbuild\img.obj
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

## test_backend.c / stub_worker.py / smoke_worker.py

See the header comment of `tests/test_backend.c` for the worker-side driver
(`test_backend.exe [python.exe] [worker.py] [venv_dir] [image]`).
