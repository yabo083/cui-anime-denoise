# Worker protocol (v2)

The app spawns `worker/worker.py` with the runtime's Python, then talks to it over
stdin/stdout. One JSON object per line, both directions. `stderr` is free-form
logging (the app writes it to `%TEMP%\cui_anime_denoise_worker.log`).

## Startup

```
<runtime>\python\Scripts\python.exe -u worker\worker.py --venv <runtime> --models <runtime>\models
```

`denoise.py` is located through, in order: `--denoise-root`, the environment variable
`ANIME_DENOISE_ROOT` (the app sets it to `<install dir>\assets\python`), `init.runtime`,
`--venv`, `sys.prefix` — each checked in the folder itself and its parent.

**The first line must be `init`.** The worker reads it synchronously on the main thread and
performs every import (`torch`, `numpy`, `PIL`, `spandrel`) and the model load there, before
it starts the stdin reader thread. This is not laziness: on Windows a thread blocked in
`stdin.readline()` deadlocks any other thread importing a `.pyd` extension.

## Messages

App → worker:

| message | meaning |
| --- | --- |
| `{"cmd":"init","runtime":null,"device":"auto","models_dir":null}` | load torch + model; answers `ready` |
| `{"cmd":"denoise","id":N,"path":"…","down_pct":60,"l0":0.001,"out":"…"}` | run the pipeline into `out` |
| `{"cmd":"composite","id":N,"mask_b64":"…","out":"…"}` | blend the cached result with a full-res 8-bit grey mask (PNG, base64) and write `out` |
| `{"cmd":"cancel","id":N}` | stop job `N` |
| `{"cmd":"models","action":"status"\|"download"}` | report / fetch the ESRGAN weights |
| `{"cmd":"shutdown"}` | exit after `bye` |

Worker → app:

| message | meaning |
| --- | --- |
| `{"type":"boot","stage":"…","value":0.42}` | progress while initialising or downloading weights |
| `{"type":"ready","device":"cuda","torch":"…","gpu":"…","model_ready":true,"models_dir":"…"}` | init finished (`model_ready:false` when the weights are missing) |
| `{"type":"progress","id":N,"stage":"upscale\|smooth\|decode\|encode\|composite","value":0.55}` | job progress |
| `{"type":"done","id":N,"out":"…","w":2096,"h":1586,"ms":7400,"sr_cached":false,"preview_b64":"…"}` | job finished |
| `{"type":"composited","id":N,"out":"…","preview_b64":"…"}` | mask blended into the cached result |
| `{"type":"models","ready":true,"path":"…","bytes":17938799}` | weights status/download result |
| `{"type":"cancelled","id":N}` | the job stopped |
| `{"type":"error","id":N\|null,"message":"…"}` | failure; `id:null` means worker-level |
| `{"type":"bye"}` | shutdown acknowledged |

`preview_b64` is a PNG whose longest side is at most 1600 px; the app decodes it with WIC.

## Caching

- **Upscaler cache**: key `(sha1 of the source file bytes, down_pct)`, LRU 3, full-resolution
  RGB. A repeat with the same parameters skips ESRGAN entirely (`sr_cached:true`) and only
  pays the L0 pass.
- **Result cache**: the last 2 denoised results by job id, so `composite` never re-runs the
  network. `composite` must reuse the `id` of the `denoise` that produced the result; a stale
  id answers `error` and the app re-runs the job.

## Cancellation

`cancel` is answered immediately on the reader thread. The running job unwinds at its next
checkpoint (per upscaler band or per L0 iteration, which can be a couple of seconds), its
output file is deleted and it never reports `done` afterwards.
