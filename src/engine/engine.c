/* engine.c -- native inference engine: dynamically loads onnxruntime.dll from
 * <runtime_dir>, runs the SR model on DirectML (high-performance adapter) or
 * the CPU EP, with the fixed-window tiling scheme proven in Phase 1 (tile
 * windows flush-aligned to image edges, interior step = tile - 2*pad, crop
 * `pad` only on interior sides -- this preserves the conv zero-padding
 * semantics of a whole-image pass).
 *
 * ESRGAN / APISR run tiled on DML when the GPU works; CUGAN always runs on the
 * CPU EP whole-image (the DML EP produces wrong output for its graph at every
 * size -- measured in Phase 1). Any DML failure at session creation or during
 * the 64x64 sanity tile falls back to the CPU EP and sets eng_note().
 *
 * POSIX (Linux x86_64): DirectML does not exist, so every device maps to the
 * CPU EP -- AUTO silently, GPU with an eng_note() like the DML-fallback one.
 * The runtime is libonnxruntime.so resolved from cfg->runtime_dir first and
 * the loader's default search path second.
 */
#include "engine.h"
#include "../plat.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>

#include "../../third_party/onnxruntime/include/dml_provider_factory.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#else
typedef struct OrtDmlApi OrtDmlApi;   /* field type only; stays NULL here */
#endif

#include "../../third_party/onnxruntime/include/onnxruntime_c_api.h"

#define ENG_PAD 64            /* >= RRDB receptive field; verified bit-exact */
#define ENG_TILE_LO 256
#define ENG_TILE_MID 384
#define ENG_TILE_HI 512
#define ENG_VRAM_HI (6ull << 30)
#define ENG_VRAM_MID (3ull << 30)

typedef struct {
    OrtSession *sess;
    int scale;
    int gpu;
    int prepared;
} eng_sess;

struct eng {
    eng_config cfg;
    void *ort_dll;
    const OrtApi *api;
    const OrtDmlApi *dml;
#ifndef _WIN32
    /* CUDA EP device found by GetEpDevices; NULL = no usable CUDA GPU.
     * Owned by e->env, so it stays valid until ReleaseEnv. */
    const OrtEpDevice *cuda_ep;
#endif
    OrtEnv *env;
    OrtMemoryInfo *mem_cpu;
    int nthreads;
    int want_gpu;
    int gpu_usable;
    int sessions_gpu;
    int tile_size;
    unsigned long long vram_free;
    wchar_t adapter_w[128];
    char adapter[128];
    char note[256];
    char dev_name[160];
    eng_sess ses[4];
};

/* ----------------------------- helpers ----------------------------------- */

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!err || !cap) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static void ort_err(eng *e, char *err, size_t cap, const char *what,
                    OrtStatus *st) {
    const char *msg = st ? e->api->GetErrorMessage(st) : "out of memory";
    set_err(err, cap, "%s: %s", what, msg);
    if (st) {
        e->api->ReleaseStatus(st);
    }
}

static int copy_image(const eng_image *src, eng_image *out) {
    size_t n = (size_t)src->w * (size_t)src->h * 3;
    out->w = src->w;
    out->h = src->h;
    out->rgb = (uint8_t *)malloc(n);
    if (!out->rgb) {
        return -1;
    }
    memcpy(out->rgb, src->rgb, n);
    return 0;
}

void eng_image_free(eng_image *im) {
    if (im && im->rgb) {
        free(im->rgb);
        im->rgb = NULL;
    }
    if (im) {
        im->w = im->h = 0;
    }
}

/* Python's round() is round-half-even; llrint uses the default FP rounding
 * mode (nearest-even) on the same double, so the results agree bit for bit. */
int eng_round_half_even(double x) {
    return (int)llrint(x);
}

/* ------------------------- device enumeration ---------------------------- */

static void eng_find_adapter(eng *e) {
#ifdef _WIN32
    IDXGIFactory6 *fac = NULL;
    HRESULT hr;
    UINT i;

    e->vram_free = 0;
    e->adapter[0] = 0;
    e->adapter_w[0] = 0;
    hr = CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&fac);
    if (FAILED(hr) || !fac) {
        return;
    }
    for (i = 0;; i++) {
        IDXGIAdapter1 *ad = NULL;
        DXGI_ADAPTER_DESC1 d;
        hr = fac->lpVtbl->EnumAdapterByGpuPreference(
            fac, i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1,
            (void **)&ad);
        if (FAILED(hr) || !ad) {
            break;
        }
        hr = ad->lpVtbl->GetDesc1(ad, &d);
        if (FAILED(hr) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            ad->lpVtbl->Release(ad);
            continue;
        }
        /* first real adapter under HIGH_PERFORMANCE is the GPU we want */
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, e->adapter,
                            (int)sizeof(e->adapter), NULL, NULL);
        wcsncpy(e->adapter_w, d.Description,
                sizeof(e->adapter_w) / sizeof(wchar_t) - 1);
        {
            IDXGIAdapter3 *ad3 = NULL;
            if (SUCCEEDED(ad->lpVtbl->QueryInterface(ad, &IID_IDXGIAdapter3,
                                                     (void **)&ad3)) &&
                ad3) {
                DXGI_QUERY_VIDEO_MEMORY_INFO mi;
                if (SUCCEEDED(ad3->lpVtbl->QueryVideoMemoryInfo(
                        ad3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) {
                    e->vram_free = mi.Budget > mi.CurrentUsage
                                       ? mi.Budget - mi.CurrentUsage
                                       : 0;
                }
                ad3->lpVtbl->Release(ad3);
            }
        }
        ad->lpVtbl->Release(ad);
        break;
    }
    fac->lpVtbl->Release(fac);
#else
    /* POSIX: no DXGI. CUDA detection happens in eng_probe_gpu_posix()
     * once the OrtEnv exists -- GetEpDevices lists only providers whose
     * device enumeration actually succeeded. */
    e->vram_free = 0;
    e->adapter[0] = 0;
    e->adapter_w[0] = 0;
#endif
}

#ifndef _WIN32
/* Find a CUDA-capable EP device. ORT registers each EP's device list only
 * when its provider library loaded and the driver responded, so a hit here
 * means CUDA really works; on a GPU-less box (or the slim CPU runtime) the
 * list just lacks the entry and we silently stay on CPU. */
static void eng_probe_gpu_posix(eng *e) {
    const OrtEpDevice *const *eps = NULL;
    OrtStatus *st;
    size_t i, n = 0;

    if (!e->api->GetEpDevices) {
        return;   /* runtime older than 1.22: CPU only */
    }
    st = e->api->GetEpDevices(e->env, &eps, &n);
    if (st) {
        e->api->ReleaseStatus(st);
        return;
    }
    for (i = 0; i < n; i++) {
        const char *epn = e->api->EpDevice_EpName
                              ? e->api->EpDevice_EpName(eps[i])
                              : NULL;
        if (!epn || strcmp(epn, "CUDAExecutionProvider") != 0) {
            continue;
        }
        e->cuda_ep = eps[i];
        /* adapter name for the device label / JSON, best effort */
        {
            const OrtHardwareDevice *hw = e->api->EpDevice_Device
                                              ? e->api->EpDevice_Device(eps[i])
                                              : NULL;
            const char *nm = NULL;
            if (hw && e->api->HardwareDevice_Metadata &&
                e->api->GetKeyValue) {
                const OrtKeyValuePairs *md =
                    e->api->HardwareDevice_Metadata(hw);
                if (md) {
                    nm = e->api->GetKeyValue(md, "Description");
                }
            }
            if ((!nm || !*nm) && hw && e->api->HardwareDevice_Vendor) {
                nm = e->api->HardwareDevice_Vendor(hw);
            }
            snprintf(e->adapter, sizeof(e->adapter), "%s",
                     (nm && *nm) ? nm : "CUDA GPU");
        }
        /* VRAM is not queryable here; assume a real discrete card. */
        if (e->tile_size < ENG_TILE_MID) {
            e->tile_size = ENG_TILE_MID;
        }
        break;
    }
}
#endif

/* --------------------------- create / destroy ----------------------------- */

#ifdef _WIN32
typedef const OrtApiBase *(WINAPI *eng_apibase_fn)(void);
#else
typedef const OrtApiBase *(*eng_apibase_fn)(void);
#endif

eng *eng_create(const eng_config *cfg, char *err, size_t cap) {
    eng *e;
    eng_path_t *dllpath;
    const OrtApiBase *base = NULL;
    eng_apibase_fn get_base;
    OrtStatus *st;

    if (!cfg || !cfg->runtime_dir || !cfg->models_dir) {
        set_err(err, cap, "eng_create: runtime_dir and models_dir are required");
        return NULL;
    }
    e = (eng *)calloc(1, sizeof(eng));
    if (!e) {
        set_err(err, cap, "out of memory");
        return NULL;
    }
    e->cfg = *cfg;
    e->want_gpu = (cfg->device != ENG_DEV_CPU);
    e->nthreads = cfg->threads > 0 ? cfg->threads : plat_num_cpus();
    eng_find_adapter(e);
    e->tile_size = e->vram_free >= ENG_VRAM_HI   ? ENG_TILE_HI
                   : e->vram_free >= ENG_VRAM_MID ? ENG_TILE_MID
                                                 : ENG_TILE_LO;

#ifdef _WIN32
    dllpath = plat_join_path(cfg->runtime_dir, L"onnxruntime.dll");
#else
    dllpath = plat_join_path(cfg->runtime_dir, PLAT_LIT("libonnxruntime.so"));
#endif
    if (!dllpath) {
        free(e);
        set_err(err, cap, "out of memory");
        return NULL;
    }
    e->ort_dll = plat_dlopen(dllpath);
#ifndef _WIN32
    if (!e->ort_dll) {
        /* Not staged next to the binary: let the loader fall back to the
         * system search path (LD_LIBRARY_PATH / ldconfig), like a normal
         * shared library dependency. */
        e->ort_dll = plat_dlopen(PLAT_LIT("libonnxruntime.so"));
    }
#endif
    free(dllpath);
    if (!e->ort_dll) {
        free(e);
#ifdef _WIN32
        set_err(err, cap,
                "onnxruntime.dll not found or failed to load (0x%lx): "
                "run tools/fetch_deps.ps1 and rebuild so <exe>\\runtime\\ "
                "contains onnxruntime.dll + DirectML.dll",
                (unsigned long)GetLastError());
#else
        set_err(err, cap,
                "libonnxruntime.so not found or failed to load: place it in "
                "<exe>/runtime/ or on the loader search path");
#endif
        return NULL;
    }
    get_base = (eng_apibase_fn)plat_dlsym(e->ort_dll, "OrtGetApiBase");
    if (!get_base || !(base = get_base())) {
        plat_dlclose(e->ort_dll);
        free(e);
#ifdef _WIN32
        set_err(err, cap, "OrtGetApiBase not found in onnxruntime.dll");
#else
        set_err(err, cap, "OrtGetApiBase not found in libonnxruntime.so");
#endif
        return NULL;
    }
    e->api = base->GetApi(ORT_API_VERSION);
    if (!e->api) {
        plat_dlclose(e->ort_dll);
        free(e);
        set_err(err, cap, "%s does not expose ORT API %u",
#ifdef _WIN32
                "onnxruntime.dll",
#else
                "libonnxruntime.so",
#endif
                (unsigned)ORT_API_VERSION);
        return NULL;
    }
    e->dml = NULL;
#ifdef _WIN32
    if (e->api->GetExecutionProviderApi) {
        e->api->GetExecutionProviderApi("DML", ORT_API_VERSION,
                                        (const void **)&e->dml);
        /* failure is non-fatal: DML EP may just be absent */
    }
#endif
    st = e->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "anime_engine", &e->env);
    if (st) {
        ort_err(e, err, cap, "CreateEnv", st);
        plat_dlclose(e->ort_dll);
        free(e);
        return NULL;
    }
    st = e->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                     &e->mem_cpu);
    if (st) {
        ort_err(e, err, cap, "CreateCpuMemoryInfo", st);
        e->api->ReleaseEnv(e->env);
        plat_dlclose(e->ort_dll);
        free(e);
        return NULL;
    }
#ifndef _WIN32
    e->cuda_ep = NULL;
    if (e->want_gpu) {
        eng_probe_gpu_posix(e);
    }
    if (e->want_gpu && !e->cuda_ep) {
        /* same semantics as the DML-fallback note: a GPU was wanted (auto
         * or explicit) and none can be used -- say so in the user's
         * language, once, up front */
        snprintf(e->note, sizeof(e->note),
                 "\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84"
                 " CUDA GPU\xEF\xBC\x8C\xE5\xB7\xB2\xE6\x94\xB9\xE7\x94\xA8 "
                 "CPU\xEF\xBC\x9B\xE9\x80\x9F\xE5\xBA\xA6\xE4\xBC\x9A\xE6\x85"
                 "\xA2\xE4\xB8\x80\xE4\xBA\x9B\xE3\x80\x82");
    }
#endif
    snprintf(e->dev_name, sizeof(e->dev_name), "CPU (%d threads)", e->nthreads);
    return e;
}

void eng_destroy(eng *e) {
    int i;
    if (!e) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (e->ses[i].sess) {
            e->api->ReleaseSession(e->ses[i].sess);
        }
    }
    if (e->mem_cpu) {
        e->api->ReleaseMemoryInfo(e->mem_cpu);
    }
    if (e->env) {
        e->api->ReleaseEnv(e->env);
    }
    if (e->ort_dll) {
        plat_dlclose(e->ort_dll);
    }
    free(e);
}

void eng_release_sessions(eng *e) {
    int i;
    if (!e) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (e->ses[i].sess) {
            e->api->ReleaseSession(e->ses[i].sess);
        }
        memset(&e->ses[i], 0, sizeof(e->ses[i]));
    }
}

const char *eng_device_name(const eng *e) {
    return e ? e->dev_name : "";
}

bool eng_using_gpu(const eng *e) {
    return e ? e->sessions_gpu != 0 : false;
}

const char *eng_note(const eng *e) {
    return e ? e->note : "";
}

/* ------------------------------ sessions ---------------------------------- */

static const eng_path_t *model_file(eng_model m) {
    switch (m) {
        case ENG_M_ESRGAN: return PLAT_LIT("esrgan_anime6b_x4.onnx");
        case ENG_M_CUGAN:  return PLAT_LIT("cugan_up2x_denoise3x.skel.onnx");
        case ENG_M_APISR:  return PLAT_LIT("apisr_rrdb_x2.skel.onnx");
        default:           return NULL;
    }
}

static int model_scale(eng_model m) {
    switch (m) {
        case ENG_M_ESRGAN: return 4;
        case ENG_M_CUGAN:  return 2;
        case ENG_M_APISR:  return 2;
        default:           return 1;
    }
}

/* The .pth a skeleton reads as external data (must sit next to the .onnx). */
static const eng_path_t *model_weights(eng_model m) {
    switch (m) {
        case ENG_M_CUGAN: return PLAT_LIT("up2x-latest-denoise3x.pth");
        case ENG_M_APISR: return PLAT_LIT("2x_APISR_RRDB_GAN_generator.pth");
        default:          return NULL;
    }
}

/* skeleton + its .pth present as a pair in `dir`? */
static int skel_pair_in(const eng_path_t *dir, const eng_path_t *skel,
                        const eng_path_t *pth) {
    eng_path_t *p;
    int ok;
    if (!dir || !pth) {
        return 0;
    }
    p = plat_join_path(dir, skel);
    ok = p && plat_file_exists(p);
    free(p);
    if (!ok) {
        return 0;
    }
    p = plat_join_path(dir, pth);
    ok = p && plat_file_exists(p);
    free(p);
    return ok;
}

/* Locate the model file: skeletons look in user_models_dir first, then
 * models_dir; the bundled ESRGAN lives in models_dir. Returns malloc'ed path
 * or NULL. For skeletons the .pth must sit next to the .onnx. */
static eng_path_t *find_model(eng *e, eng_model m, char *err, size_t cap) {
    const eng_path_t *fn = model_file(m);
    eng_path_t *p;
    if (!fn) {
        set_err(err, cap, "no model file for model %d", (int)m);
        return NULL;
    }
    if (m == ENG_M_ESRGAN) {
        p = plat_join_path(e->cfg.models_dir, fn);
        if (p && plat_file_exists(p)) {
            return p;
        }
        free(p);
        set_err(err, cap, "models/esrgan_anime6b_x4.onnx missing: run "
                          "tools/export_onnx.py and rebuild");
        return NULL;
    }
    /* skeletons resolve external weights relative to the .onnx location, so
     * the .skel.onnx and its .pth must live in the same directory */
    {
        const eng_path_t *pth = model_weights(m);
        if (skel_pair_in(e->cfg.user_models_dir, fn, pth)) {
            return plat_join_path(e->cfg.user_models_dir, fn);
        }
        if (skel_pair_in(e->cfg.models_dir, fn, pth)) {
            return plat_join_path(e->cfg.models_dir, fn);
        }
    }
    {
        char a[600], b[600];
        plat_path_to_u8(fn, a, sizeof a);
        plat_path_to_u8(model_weights(m), b, sizeof b);
        set_err(err, cap,
                "%s + %s pair not found in user_models_dir or models_dir",
                a, b);
    }
    return NULL;
}

static OrtSession *new_session(eng *e, const eng_path_t *path, int gpu,
                               char *err, size_t cap) {
    OrtSessionOptions *so = NULL;
    OrtSession *sess = NULL;
    OrtStatus *st;

    st = e->api->CreateSessionOptions(&so);
    if (st) {
        ort_err(e, err, cap, "CreateSessionOptions", st);
        return NULL;
    }
    st = e->api->SetIntraOpNumThreads(so, e->nthreads);
    if (st) {
        /* non-fatal: a runtime that refuses thread count keeps its default */
        e->api->ReleaseStatus(st);
        st = NULL;
    }
#ifdef _WIN32
    if (gpu) {
        OrtDmlDeviceOptions opts;
        /* DML requires the memory pattern off and sequential execution. */
        e->api->DisableMemPattern(so);
        e->api->SetSessionExecutionMode(so, ORT_SEQUENTIAL);
        opts.Preference = HighPerformance;
        opts.Filter = Gpu;
        st = e->dml->SessionOptionsAppendExecutionProvider_DML2(so, &opts);
        if (st) {
            ort_err(e, err, cap, "AppendExecutionProvider_DML2", st);
            e->api->ReleaseSessionOptions(so);
            return NULL;
        }
    }
#else
    if (gpu) {
        /* Append the EP for the device eng_probe_gpu_posix() found. If the
         * provider cannot actually be created here the call fails and the
         * caller retries with a CPU session -- same contract as DML. */
        st = e->api->SessionOptionsAppendExecutionProvider_V2(
            so, e->env, &e->cuda_ep, 1, NULL, NULL, 0);
        if (st) {
            ort_err(e, err, cap, "AppendExecutionProvider_CUDA", st);
            e->api->ReleaseSessionOptions(so);
            return NULL;
        }
    }
#endif
    st = e->api->CreateSession(e->env, path, so, &sess);
    e->api->ReleaseSessionOptions(so);
    if (st) {
        ort_err(e, err, cap, "CreateSession", st);
        return NULL;
    }
    return sess;
}

/* One 64x64 all-mid-grey tile through the session; output must be finite and
 * within [-0.5, 1.5] (a broken DML driver/compiler returns zeros or NaNs). */
static int sanity_tile(eng *e, OrtSession *sess, int scale) {
    const int n = 64;
    float *in;
    OrtValue *tin = NULL, *tout = NULL;
    int64_t dims[4] = {1, 3, n, n};
    const char *const names_in[] = {"input"};
    const char *const names_out[] = {"output"};
    OrtStatus *st;
    float *y;
    size_t i, count;
    int ok = 0;

    in = (float *)malloc((size_t)3 * n * n * sizeof(float));
    if (!in) {
        return 0;
    }
    for (i = 0; i < (size_t)3 * n * n; i++) {
        in[i] = 0.5f;
    }
    st = e->api->CreateTensorWithDataAsOrtValue(
        e->mem_cpu, in, (size_t)3 * n * n * sizeof(float), dims, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tin);
    if (st) {
        e->api->ReleaseStatus(st);
        free(in);
        return 0;
    }
    st = e->api->Run(sess, NULL, names_in,
                     (const OrtValue *const *)&tin, 1, names_out, 1, &tout);
    free(in);
    e->api->ReleaseValue(tin);
    if (st) {
        e->api->ReleaseStatus(st);
        return 0;
    }
    st = e->api->GetTensorMutableData(tout, (void **)&y);
    if (st) {
        e->api->ReleaseStatus(st);
        e->api->ReleaseValue(tout);
        return 0;
    }
    count = (size_t)3 * n * scale * n * scale;
    ok = 1;
    for (i = 0; i < count; i++) {
        if (!isfinite(y[i]) || y[i] < -0.5f || y[i] > 1.5f) {
            ok = 0;
            break;
        }
    }
    e->api->ReleaseValue(tout);
    return ok;
}

bool eng_prepare(eng *e, eng_model m, char *err, size_t cap) {
    eng_path_t *path;
    eng_sess *s;
    int gpu;

    if (!e || m < 0 || m > ENG_M_APISR) {
        set_err(err, cap, "eng_prepare: bad model %d", (int)m);
        return false;
    }
    s = &e->ses[m];
    if (s->prepared) {
        return true;
    }
    path = find_model(e, m, err, cap);
    if (!path) {
        return false;
    }
    s->scale = model_scale(m);
    /* CUGAN is wrong on DML at any size (Phase 1) -> CPU EP always. */
#ifdef _WIN32
    gpu = e->want_gpu && e->dml && m != ENG_M_CUGAN;
#else
    gpu = e->want_gpu && e->cuda_ep && m != ENG_M_CUGAN;
#endif

    if (gpu) {
        s->sess = new_session(e, path, 1, err, cap);
        if (s->sess && !sanity_tile(e, s->sess, s->scale)) {
            e->api->ReleaseSession(s->sess);
            s->sess = NULL;
            set_err(err, cap, "GPU sanity tile returned garbage");
        }
        if (s->sess) {
            s->gpu = 1;
            s->prepared = 1;
            e->sessions_gpu = 1;
            snprintf(e->dev_name, sizeof(e->dev_name),
#ifdef _WIN32
                     "%s (DirectML)",
#else
                     "%s (CUDA)",
#endif
                     e->adapter[0] ? e->adapter : "GPU");
        } else {
            /* the GPU EP can't run this model -> CPU, like denoise.py's note */
            snprintf(e->note, sizeof(e->note),
#ifdef _WIN32
                     "DirectML \xE6\x97\xA0\xE6\xB3\x95\xE5\x9C\xA8\xE8\xBF"
#else
                     "CUDA \xE6\x97\xA0\xE6\xB3\x95\xE5\x9C\xA8\xE8\xBF"
#endif
                     "\x99\xE5\xBC\xA0\xE6\x98\xBE\xE5\x8D\xA1\xE4\xB8\x8A"
                     "\xE8\xBF\x90\xE8\xA1\x8C"
                     "\xEF\xBC\x88%s\xEF\xBC\x89\xEF\xBC\x8C\xE5\xB7\xB2"
                     "\xE6\x94\xB9\xE7\x94\xA8 CPU\xEF\xBC\x9B\xE9\x80\x9F"
                     "\xE5\xBA\xA6\xE4\xBC\x9A\xE6\x85\xA2\xE4\xB8\x80"
                     "\xE4\xBA\x9B\xE3\x80\x82",
                     e->adapter[0] ? e->adapter : "?");
            gpu = 0;
        }
    }
    if (!s->sess) {
        char cpu_err[256] = {0};
        s->sess = new_session(e, path, 0, cpu_err, sizeof(cpu_err));
        if (!s->sess) {
            if (!err || !err[0] || !gpu) {
                set_err(err, cap, "%s", cpu_err[0] ? cpu_err : "session failed");
            }
            free(path);
            return false;
        }
        s->gpu = 0;
        s->prepared = 1;
    }
    free(path);
    return true;
}

/* ------------------------------ inference --------------------------------- */

/* Fill the NCHW float input for window (x,y,w,h) of img: rgb/255. */
static void fill_input(float *dst, const eng_image *img, int x, int y, int w,
                       int h) {
    int i, j, c;
    for (c = 0; c < 3; c++) {
        for (i = 0; i < h; i++) {
            const uint8_t *px = img->rgb + ((size_t)(y + i) * img->w + x) * 3;
            float *row = dst + ((size_t)c * h + i) * w;
            for (j = 0; j < w; j++) {
                row[j] = px[(size_t)j * 3 + c] / 255.0f;
            }
        }
    }
}

/* Quantize NCHW float output -> rgb region of `out` at (ox,oy) of size w,h,
 * reading the source window at (sx,sy) inside the nchw plane.
 * y -> clamp(0,1) -> (y*255+0.5) trunc, float32 like the reference. */
static void store_output(uint8_t *out, int out_w, int ox, int oy,
                         const float *y, int nchw_h, int nchw_w,
                         int sx, int sy, int w, int h) {
    int i, j, c;
    for (c = 0; c < 3; c++) {
        for (i = 0; i < h; i++) {
            const float *row = y + ((size_t)c * nchw_h + (sy + i)) * nchw_w + sx;
            uint8_t *px = out + ((size_t)(oy + i) * out_w + ox) * 3;
            for (j = 0; j < w; j++) {
                float v = row[j];
                long q;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                q = (long)(v * 255.0f + 0.5f);
                px[(size_t)j * 3 + c] = (uint8_t)q;
            }
        }
    }
}

static int run_one(eng *e, eng_sess *s, const float *in, int iw, int ih,
                   OrtValue **out, char *err, size_t cap) {
    OrtValue *tin = NULL, *tout = NULL;
    int64_t dims[4] = {1, 3, ih, iw};
    const char *const names_in[] = {"input"};
    const char *const names_out[] = {"output"};
    OrtStatus *st;

    st = e->api->CreateTensorWithDataAsOrtValue(
        e->mem_cpu, (void *)in, (size_t)3 * ih * iw * sizeof(float), dims, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tin);
    if (st) {
        ort_err(e, err, cap, "CreateTensor", st);
        return -1;
    }
    st = e->api->Run(s->sess, NULL, names_in,
                     (const OrtValue *const *)&tin, 1, names_out, 1, &tout);
    e->api->ReleaseValue(tin);
    if (st) {
        ort_err(e, err, cap, "OrtSession::Run", st);
        return -1;
    }
    *out = tout;
    return 0;
}

static int out_dims(eng *e, OrtValue *v, int *h, int *w) {
    OrtTensorTypeAndShapeInfo *ti = NULL;
    int64_t dims[8];
    size_t nd = 0;
    OrtStatus *st = e->api->GetTensorTypeAndShape(v, &ti);
    if (st) {
        e->api->ReleaseStatus(st);
        return -1;
    }
    st = e->api->GetDimensionsCount(ti, &nd);
    if (st) {
        e->api->ReleaseStatus(st);
        nd = 0;
    }
    if (nd >= 4) {
        st = e->api->GetDimensions(ti, dims, nd);
        if (st) {
            e->api->ReleaseStatus(st);
            nd = 0;
        } else {
            *h = (int)dims[2];
            *w = (int)dims[3];
        }
    }
    e->api->ReleaseTensorTypeAndShapeInfo(ti);
    return nd >= 4 ? 0 : -1;
}

/* Tile positions on one axis: step = tile - 2*pad, windows flush-aligned to
 * both ends; when n < tile a single [0, n) window uses the true dimension.
 * Non-static (eng__ prefix) so tests/test_engine.c can cover the math. */
int eng__axis_windows(int n, int tile, int pad, int *starts, int *sizes,
                      int cap) {
    int step = tile - 2 * pad;
    int count = 0, x = 0;
    if (n <= tile) {
        starts[0] = 0;
        sizes[0] = n;
        return 1;
    }
    while (x + tile < n && count < cap) {
        starts[count] = x;
        sizes[count] = tile;
        count++;
        x += step;
    }
    if (count < cap && x + tile >= n) {
        starts[count] = n - tile;   /* flush right */
        sizes[count] = tile;
        count++;
    }
    return count;
}

/* Tiled SR of `scaled` into `sr` (dims * scale). One ORT run per window; pad
 * cropped only on interior sides so edge windows keep real borders. */
static int infer_tiled(eng *e, eng_sess *s, const eng_image *scaled,
                       eng_image *sr, eng_progress_fn cb, void *ud,
                       volatile long *cancel, char *err, size_t cap) {
    int tile = s->gpu ? e->tile_size : ENG_TILE_MID;
    int scale = s->scale;
    int xs[256], xw[256], ys[256], yh[256], nx, ny, ix, iy, done = 0;
    float *in;
    sr->w = scaled->w * scale;
    sr->h = scaled->h * scale;
    sr->rgb = (uint8_t *)calloc((size_t)sr->w * (size_t)sr->h, 3);
    if (!sr->rgb) {
        set_err(err, cap, "out of memory");
        return -1;
    }
    nx = eng__axis_windows(scaled->w, tile, ENG_PAD, xs, xw, 256);
    ny = eng__axis_windows(scaled->h, tile, ENG_PAD, ys, yh, 256);
    /* the window array is capped: if it filled without reaching the far
     * edge the tail of the image would silently stay black -- fail loudly */
    if (nx >= 256 || ny >= 256 || xs[nx - 1] + xw[nx - 1] < scaled->w ||
        ys[ny - 1] + yh[ny - 1] < scaled->h) {
        free(sr->rgb);
        sr->rgb = NULL;
        sr->w = sr->h = 0;
        set_err(err, cap, "image exceeds the 256-window tiling limit");
        return -1;
    }
    in = (float *)malloc((size_t)3 * tile * tile * sizeof(float));
    if (!in) {
        set_err(err, cap, "out of memory");
        return -1;
    }
    for (iy = 0; iy < ny; iy++) {
        for (ix = 0; ix < nx; ix++) {
            int x = xs[ix], y = ys[iy], w = xw[ix], h = yh[iy];
            int cl = x > 0 ? ENG_PAD * scale : 0;
            int cr = x + w < scaled->w ? ENG_PAD * scale : 0;
            int ct = y > 0 ? ENG_PAD * scale : 0;
            int cbm = y + h < scaled->h ? ENG_PAD * scale : 0;
            OrtValue *tout = NULL;
            float *ydata;
            int oh = 0, ow = 0;
            if (cancel && *cancel) {
                free(in);
                return 1;
            }
            fill_input(in, scaled, x, y, w, h);
            if (run_one(e, s, in, w, h, &tout, err, cap)) {
                free(in);
                return -1;
            }
            if (out_dims(e, tout, &oh, &ow) ||
                e->api->GetTensorMutableData(tout, (void **)&ydata)) {
                e->api->ReleaseValue(tout);
                free(in);
                set_err(err, cap, "bad output tensor");
                return -1;
            }
            store_output(sr->rgb, sr->w, x * scale + cl, y * scale + ct,
                         ydata, oh, ow, cl, ct,
                         w * scale - cl - cr, h * scale - ct - cbm);
            e->api->ReleaseValue(tout);
            done++;
            if (cb) {
                cb(ud, "upscale", (float)done / (float)(nx * ny));
            }
        }
    }
    free(in);
    return 0;
}

int eng_upscale(eng *e, const eng_image *src, const eng_params *p,
                eng_image *out_sr, eng_progress_fn cb, void *ud,
                volatile long *cancel, char *err, size_t cap) {
    eng_image scaled = {0, 0, NULL};
    int sw, sh, r = -1;

    if (!e || !src || !src->rgb || !p || !out_sr) {
        set_err(err, cap, "eng_upscale: bad args");
        return -1;
    }
    out_sr->w = out_sr->h = 0;
    out_sr->rgb = NULL;
    if (p->model == ENG_M_NONE) {
        return copy_image(src, out_sr) ? (set_err(err, cap, "out of memory"), -1) : 0;
    }
    if (p->model < ENG_M_ESRGAN || p->model > ENG_M_APISR) {
        set_err(err, cap, "eng_upscale: bad model");
        return -1;
    }
    if (!eng_prepare(e, p->model, err, cap)) {
        return -1;
    }
    /* downscale: APISR runs on the source, the x2/x4 models on `down` */
    if (p->model == ENG_M_APISR) {
        if (copy_image(src, &scaled)) {
            set_err(err, cap, "out of memory");
            return -1;
        }
    } else {
        double d = p->down > 0.0 ? p->down : 0.6;
        sw = eng_round_half_even(src->w * d);
        sh = eng_round_half_even(src->h * d);
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (!eng_resize(src, sw, sh, ENG_BOX, &scaled)) {
            set_err(err, cap, "BOX downscale failed");
            return -1;
        }
    }
    if (cancel && *cancel) {
        eng_image_free(&scaled);
        return 1;
    }
    {
        eng_sess *s = &e->ses[p->model];
        eng_image raw = {0, 0, NULL};
        if (p->model == ENG_M_CUGAN) {
            /* CUGAN: CPU EP, whole image; graph pads internally to a multiple
             * of 4 and returns 2*padded -- crop the output to 2*input. */
            int inw = scaled.w, inh = scaled.h;
            float *in = (float *)malloc((size_t)3 * inh * inw * sizeof(float));
            OrtValue *tout = NULL;
            float *yd;
            int oh = 0, ow = 0;
            if (!in) {
                eng_image_free(&scaled);
                set_err(err, cap, "out of memory");
                return -1;
            }
            fill_input(in, &scaled, 0, 0, inw, inh);
            r = run_one(e, s, in, inw, inh, &tout, err, cap);
            free(in);
            if (!r) {
                if (out_dims(e, tout, &oh, &ow) ||
                    e->api->GetTensorMutableData(tout, (void **)&yd)) {
                    e->api->ReleaseValue(tout);
                    eng_image_free(&scaled);
                    set_err(err, cap, "bad output tensor");
                    return -1;
                }
                raw.w = inw * s->scale;
                raw.h = inh * s->scale;
                raw.rgb = (uint8_t *)malloc((size_t)raw.w * raw.h * 3);
                if (!raw.rgb) {
                    e->api->ReleaseValue(tout);
                    eng_image_free(&scaled);
                    set_err(err, cap, "out of memory");
                    return -1;
                }
                if (ow >= raw.w && oh >= raw.h) {
                    store_output(raw.rgb, raw.w, 0, 0, yd, oh, ow, 0, 0,
                                 raw.w, raw.h);
                } else {
                    e->api->ReleaseValue(tout);
                    eng_image_free(&scaled);
                    eng_image_free(&raw);
                    set_err(err, cap, "CUGAN output smaller than 2x input");
                    return -1;
                }
                e->api->ReleaseValue(tout);
                if (cb) {
                    cb(ud, "upscale", 1.0f);
                }
            }
        } else {
            r = infer_tiled(e, s, &scaled, &raw, cb, ud, cancel, err, cap);
        }
        eng_image_free(&scaled);
        if (r) {
            eng_image_free(&raw);
            return r;
        }
        /* LANCZOS back to the source size (operates on the uint8 SR image,
         * exactly like the reference's `Image.resize((W,H), LANCZOS)`) */
        if (!eng_resize(&raw, src->w, src->h, ENG_LANCZOS, out_sr)) {
            eng_image_free(&raw);
            set_err(err, cap, "LANCZOS upsize failed");
            return -1;
        }
        eng_image_free(&raw);
    }
    return 0;
}

int eng_denoise(eng *e, const eng_image *src, const eng_params *p,
                eng_image *out, eng_progress_fn cb, void *ud,
                volatile long *cancel, char *err, size_t cap) {
    eng_image sr = {0, 0, NULL};
    int r;

    if (!p) {
        set_err(err, cap, "eng_denoise: bad args");
        return -1;
    }
    r = eng_upscale(e, src, p, &sr, cb, ud, cancel, err, cap);
    if (r) {
        return r;
    }
    if (p->l0 > 0.0) {
        r = eng_l0(&sr, p->l0, out, e ? e->nthreads : 0, cb, ud, cancel);
        if (r < 0) {
            set_err(err, cap, "L0 smoothing failed");
        }
    } else {
        *out = sr;
        sr.rgb = NULL;
        r = 0;
    }
    eng_image_free(&sr);
    return r;
}
