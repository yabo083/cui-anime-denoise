/* anime-denoise -- console front end over the native engine (see
 * docs/cli-spec.md). One process, no GUI state, no single-instance mutex:
 * decode via WIC on Windows (img.c) or stb on POSIX (posix/img_posix.c),
 * denoise via anime_engine, save PNGs with alpha and ICC put back, like
 * denoise.py's save_png.
 *
 * Exit codes: 0 all done, 1 some/every image failed, 2 bad arguments,
 * 3 inference engine unavailable (missing runtime libs or model files).
 *
 * The weights download expects process-level hooks the GUI normally
 * provides (update_io.c on Windows, posix/download_curl.c on POSIX).
 * The console versions live at the bottom of this file: no settings dir,
 * logging goes to stderr, and "stop requested" is our Ctrl-C/SIGINT flag.
 */
#include "../img.h"
#include "../img_priv.h"   /* img__pixels_alloc (same-module internals) */
#include "../engine/engine.h"
#include "../plat.h"
#include "../config.h"     /* log_level, for the log_write stub signature */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <signal.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
static char *w2u(const wchar_t *w)
{
    char *s;
    if (!w) return NULL;
    /* worst case: each UTF-16 unit becomes 4 UTF-8 bytes */
    s = (char *)malloc(wcslen(w) * 4 + 1);
    if (!s) return NULL;
    if (!plat_path_to_u8(w, s, wcslen(w) * 4 + 1)) {
        free(s);
        return NULL;
    }
    return s;
}
#endif

/* eng_path_t -> freshly malloc'ed UTF-8 (scan_dir feeds file names to the
 * UTF-8-only helpers). Worst case 4 bytes per path unit covers surrogate
 * pairs; POSIX is 1:1 so the same bound is simply generous. */
static char *path_u8_alloc(const eng_path_t *p)
{
    size_t cap = plat_plen(p) * 4 + 1;
    char *s = (char *)malloc(cap);
    if (!s) return NULL;
    if (!plat_path_to_u8(p, s, cap)) {
        free(s);
        return NULL;
    }
    return s;
}

static void err_line(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* mkdir -p on a UTF-8 path. */
static bool mkdirs(const char *utf8)
{
    eng_path_t *w = plat_u8_to_path(utf8);
    size_t n;
    if (!w) return false;
    n = plat_plen(w);
    for (size_t i = 1; i <= n; i++) {        /* skip a leading separator */
        if (w[i] == '\\' || w[i] == '/' || i == n) {
            eng_path_t keep = w[i];
            w[i] = 0;
            if (w[0] && !(i == 2 && w[1] == ':')) {   /* drive letter / UNC edge */
                if (!plat_mkdir(w)) {
                    w[i] = keep;
                    free(w);
                    return false;
                }
            }
            w[i] = keep;
        }
    }
    free(w);
    return true;
}

/* UTF-8 path -> {dir, stem}: dir keeps the trailing slash when present. */
static void split_path(const char *path, char *dir, size_t dcap,
                       char *stem, size_t scap)
{
    const char *base = path;
    const char *dot;
    size_t n;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    n = (size_t)(base - path);
    if (n >= dcap) n = dcap - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
    dot = strrchr(base, '.');
    if (!dot || dot == base) dot = base + strlen(base);
    n = (size_t)(dot - base);
    if (n >= scap) n = scap - 1;
    memcpy(stem, base, n);
    stem[n] = 0;
}

static bool is_dir(const char *utf8)
{
    eng_path_t *w = plat_u8_to_path(utf8);
    bool r = w && plat_is_dir(w);
    free(w);
    return r;
}

/* denoise.py's IMAGE_EXTS, case-insensitive on the suffix. */
static const char *const IMAGE_EXTS[] = {
    ".png", ".jpg", ".jpeg", ".jfif", ".webp", ".bmp",
    ".gif", ".tif", ".tiff", ".avif", ".tga"
};

static bool is_image_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (size_t i = 0; i < sizeof IMAGE_EXTS / sizeof IMAGE_EXTS[0]; i++)
        if (plat_stricmp(dot, IMAGE_EXTS[i]) == 0) return true;
    return false;
}

/* Sorted (code-point order, which UTF-8 byte order preserves) list of image
 * files inside `dir`, as full UTF-8 paths. */
static char **scan_dir(const char *dir, int *count)
{
    eng_path_t *pd;
    plat_dir *d = NULL;
    eng_path_t *name;
    char **list = NULL;
    int n = 0, cap = 0;

    *count = 0;
    pd = plat_u8_to_path(dir);
    if (!pd) return NULL;
    d = plat_dir_open(pd);
    free(pd);
    if (!d) return NULL;

    while ((name = plat_dir_next_file(d)) != NULL) {
        char *uname, *full;
        uname = path_u8_alloc(name);
        free(name);
        if (!uname) continue;
        if (!is_image_name(uname)) {
            free(uname);
            continue;
        }
        full = (char *)malloc(strlen(dir) + strlen(uname) + 2);
        if (full) sprintf(full, "%s/%s", dir, uname);
        free(uname);
        if (!full) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nl = (char **)realloc(list, (size_t)cap * sizeof *nl);
            if (!nl) {
                free(full);
                break;
            }
            list = nl;
        }
        list[n++] = full;
    }
    plat_dir_close(d);

    /* insertion sort; folder counts are small */
    for (int i = 1; i < n; i++) {
        char *key = list[i];
        int j = i - 1;
        while (j >= 0 && strcmp(list[j], key) > 0) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = key;
    }
    *count = n;
    return list;
}

/* ------------------------------------------------------------------------- */
/* Methods                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum { L0_NONE = 0, L0_ARG, L0_FIXED } l0_mode;

typedef struct {
    const char *name;
    eng_model model;    /* ENG_M_NONE for the l0_* methods */
    l0_mode l0;
    double fixed_lam;
} method_def;

static const method_def METHODS[] = {
    { "esrgan60_l0", ENG_M_ESRGAN, L0_ARG,   0 },
    { "esrgan60",    ENG_M_ESRGAN, L0_NONE,  0 },
    { "cugan60_l0",  ENG_M_CUGAN,  L0_ARG,   0 },
    { "cugan60",     ENG_M_CUGAN,  L0_NONE,  0 },
    { "apisr",       ENG_M_APISR,  L0_NONE,  0 },
    { "l0_0.001",    ENG_M_NONE,   L0_FIXED, 0.001 },
    { "l0_0.0005",   ENG_M_NONE,   L0_FIXED, 0.0005 },
};
#define N_METHODS (sizeof METHODS / sizeof METHODS[0])

/* denoise.py SHEET_ORDER: the original plus the seven results. */
static const char *const SHEET_ORDER[] = {
    "original", "l0_0.0005", "l0_0.001", "apisr",
    "cugan60", "cugan60_l0", "esrgan60", "esrgan60_l0"
};

static const method_def *find_method(const char *name)
{
    for (size_t i = 0; i < N_METHODS; i++)
        if (strcmp(METHODS[i].name, name) == 0) return &METHODS[i];
    return NULL;
}

static int method_index(const char *name)
{
    for (int i = 0; i < (int)N_METHODS; i++)
        if (strcmp(METHODS[i].name, name) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Progress + output                                                         */
/* ------------------------------------------------------------------------- */

static volatile long g_cancel;

#ifdef _WIN32
static BOOL WINAPI on_ctrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        InterlockedExchange(&g_cancel, 1);
        return TRUE;
    }
    return FALSE;
}

static void install_cancel_handler(void)
{
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}
#else
static void on_sigint(int sig)
{
    (void)sig;
    plat_atomic_set(&g_cancel, 1);   /* async-signal-safe exchange */
}

static void install_cancel_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* --stdout writes PNG bytes into a pipe: a closed consumer must not
     * kill the process mid-write, the write error is handled instead */
    signal(SIGPIPE, SIG_IGN);
}
#endif

typedef struct {
    unsigned long long last_ms;
    int quiet;
} prog_ctx;

/* Engine progress -> "[stage] NN%" lines on stderr, ~10 Hz max. */
static void on_progress(void *ud, const char *stage, float v01)
{
    prog_ctx *p = (prog_ctx *)ud;
    unsigned long long now;
    int pct;
    if (p->quiet) return;
    now = plat_tick_ms();
    if (v01 < 1.0f && now - p->last_ms < 100) return;
    p->last_ms = now;
    pct = (int)(v01 * 100.0f + 0.5f);
    if (pct > 100) pct = 100;
    fprintf(stderr, "[%s] %d%%\n", stage ? stage : "work", pct);
}

/* JSON string escaping into a fixed buffer (paths are short enough). */
static void json_escape(const char *s, char *out, size_t cap)
{
    char *o = out;
    char *end = out + cap - 1;
    for (; *s && o + 8 < end; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': *o++ = '\\'; *o++ = '"'; break;
        case '\\': *o++ = '\\'; *o++ = '\\'; break;
        case '\n': *o++ = '\\'; *o++ = 'n'; break;
        case '\r': *o++ = '\\'; *o++ = 'r'; break;
        case '\t': *o++ = '\\'; *o++ = 't'; break;
        default:
            if (c < 0x20) {
                o += snprintf(o, 8, "\\u%04x", c);
            } else {
                *o++ = (char)c;
            }
        }
    }
    *o = 0;
}

/* ------------------------------------------------------------------------- */
/* Options                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    char **inputs;
    int n_inputs;
    const char *out_dir;
    const char *method;
    int all;
    double l0;
    double down_pct;
    eng_device device;
    const char *models_dir;
    const char *download;
    int json;
    int quiet;
    int use_stdin;
    int use_stdout;
    int show_help;
    int show_version;
} cli_opts;

static const char USAGE[] =
    "anime-denoise <input...> [options]\n"
    "  input      image files or folders (folders take every supported image);\n"
    "             \"-\" reads one image from stdin\n"
    "options:\n"
    "  --out DIR           output dir (default <image dir>/<stem>_denoise/)\n"
    "  --method NAME       esrgan60_l0(default) esrgan60 cugan60_l0 cugan60 apisr\n"
    "                      l0_0.001 l0_0.0005\n"
    "  --all               run all 7 methods + 4x2 comparison <stem>_sheet.png\n"
    "  --l0 X              L0 strength (default 0.001; 0 skips L0)\n"
    "  --down PCT          pre-downscale percent (default 60; apisr ignores it)\n"
    "  --device auto|gpu|cpu   inference device (default auto)\n"
#ifdef _WIN32
    "  --models DIR        model dir (default %%LOCALAPPDATA%%\\AnimeDenoise\\models)\n"
#else
    "  --models DIR        model dir (default $XDG_DATA_HOME/anime-denoise/models\n"
    "                      or ~/.local/share/anime-denoise/models)\n"
#endif
    "  --download NAME     fetch cugan|apisr weights to --models and exit\n"
    "  --json              one JSON object per completed item on stdout\n"
    "  --quiet, -q         no progress output\n"
    "  --stdin             read one image from stdin\n"
    "  --stdout            write the result PNG to stdout (stdin mode only)\n"
    "  --help, --version\n";

static const char *take_value(int *i, int argc, char **argv, const char *arg,
                              const char *eq)
{
    if (eq) return eq + 1;
    if (*i + 1 < argc) return argv[++*i];
    err_line("anime-denoise: %s needs a value", arg);
    return NULL;
}

static int parse_args(int argc, char **argv, cli_opts *o)
{
    memset(o, 0, sizeof *o);
    o->method = "esrgan60_l0";
    o->l0 = 0.001;
    o->down_pct = 60.0;
    o->device = ENG_DEV_AUTO;
    o->inputs = (char **)calloc((size_t)argc, sizeof(char *));

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        const char *eq = strchr(a, '=');
        size_t namelen = eq ? (size_t)(eq - a) : strlen(a);

        if (strcmp(a, "-") == 0) {
            o->use_stdin = 1;
        } else if (strncmp(a, "--out", namelen) == 0 && (namelen == 5)) {
            o->out_dir = take_value(&i, argc, argv, a, eq);
            if (!o->out_dir) return 2;
        } else if (namelen == 8 && strncmp(a, "--method", namelen) == 0) {
            o->method = take_value(&i, argc, argv, a, eq);
            if (!o->method) return 2;
        } else if (strcmp(a, "--all") == 0) {
            o->all = 1;
        } else if (namelen == 4 && strncmp(a, "--l0", namelen) == 0) {
            const char *v = take_value(&i, argc, argv, a, eq);
            char *end;
            if (!v) return 2;
            o->l0 = strtod(v, &end);
            if (end == v || *end || !(o->l0 >= 0.0) || o->l0 > 1e30) {
                err_line("anime-denoise: --l0 must be a finite number >= 0, got '%s'", v);
                return 2;
            }
        } else if (namelen == 6 && strncmp(a, "--down", namelen) == 0) {
            const char *v = take_value(&i, argc, argv, a, eq);
            char *end;
            if (!v) return 2;
            o->down_pct = strtod(v, &end);
            if (end == v || *end || o->down_pct <= 0.0 || o->down_pct > 100.0) {
                err_line("anime-denoise: --down must be in (0,100], got '%s'", v);
                return 2;
            }
        } else if (namelen == 8 && strncmp(a, "--device", namelen) == 0) {
            const char *v = take_value(&i, argc, argv, a, eq);
            if (!v) return 2;
            if (plat_stricmp(v, "auto") == 0) o->device = ENG_DEV_AUTO;
            else if (plat_stricmp(v, "gpu") == 0) o->device = ENG_DEV_GPU;
            else if (plat_stricmp(v, "cpu") == 0) o->device = ENG_DEV_CPU;
            else {
                err_line("anime-denoise: --device must be auto|gpu|cpu, got '%s'", v);
                return 2;
            }
        } else if (namelen == 8 && strncmp(a, "--models", namelen) == 0) {
            o->models_dir = take_value(&i, argc, argv, a, eq);
            if (!o->models_dir) return 2;
        } else if (namelen == 10 && strncmp(a, "--download", namelen) == 0) {
            o->download = take_value(&i, argc, argv, a, eq);
            if (!o->download) return 2;
        } else if (strcmp(a, "--json") == 0) {
            o->json = 1;
        } else if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0) {
            o->quiet = 1;
        } else if (strcmp(a, "--stdin") == 0) {
            o->use_stdin = 1;
        } else if (strcmp(a, "--stdout") == 0) {
            o->use_stdout = 1;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            o->show_help = 1;
        } else if (strcmp(a, "--version") == 0) {
            o->show_version = 1;
        } else if (a[0] == '-' && a[1]) {
            err_line("anime-denoise: unknown option '%s'", a);
            return 2;
        } else {
            o->inputs[o->n_inputs++] = argv[i];
        }
    }

    if (o->show_help || o->show_version) return 0;
    if (!find_method(o->method)) {
        err_line("anime-denoise: unknown method '%s'", o->method);
        return 2;
    }
    if (o->use_stdout && !o->use_stdin) {
        err_line("anime-denoise: --stdout is only valid with stdin input");
        return 2;
    }
    if (o->use_stdin && o->n_inputs > 0) {
        err_line("anime-denoise: stdin is a single image; do not mix with file inputs");
        return 2;
    }
    if (o->use_stdout && o->json) {
        err_line("anime-denoise: --json cannot share stdout with PNG bytes");
        return 2;
    }
    if (o->use_stdout && o->all) {
        err_line("anime-denoise: --all writes files; it cannot write to stdout");
        return 2;
    }
    if (o->use_stdout) o->quiet = 1;    /* binary on stdout: progress is off */
    if (o->n_inputs == 0 && !o->use_stdin && !o->download) {
        err_line("anime-denoise: no input images");
        return 2;
    }
    if (o->download && (o->n_inputs || o->use_stdin)) {
        err_line("anime-denoise: --download takes no image inputs");
        return 2;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Engine wiring                                                             */
/* ------------------------------------------------------------------------- */

static eng_path_t *join_w(const eng_path_t *a, const eng_path_t *b)
{
    return plat_join_path(a, b);
}

/* Windows: "%LOCALAPPDATA%\AnimeDenoise\models".
 * POSIX:   "$XDG_DATA_HOME/anime-denoise/models" (or
 *          ~/.local/share/anime-denoise/models), with $DN_USER_MODELS_DIR
 *          honored first -- the same test hook the GUI's denoiser.c uses. */
static eng_path_t *default_models_dir(void)
{
#ifdef _WIN32
    const wchar_t *base = _wgetenv(L"LOCALAPPDATA");
    eng_path_t *dir, *out;
    if (!base || !*base) base = L".";
    dir = join_w(base, L"AnimeDenoise");
    if (!dir) return NULL;
    out = join_w(dir, L"models");
    free(dir);
    return out;
#else
    const char *xdg, *home;
    eng_path_t *base = NULL, *dir, *out;
    const char *env = getenv("DN_USER_MODELS_DIR");
    if (env && *env) return plat_u8_to_path(env);
    xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        base = plat_u8_to_path(xdg);
    } else {
        home = getenv("HOME");
        if (home && *home) {
            eng_path_t *h = plat_u8_to_path(home);
            if (h) {
                base = join_w(h, ".local/share");
                free(h);
            }
        }
    }
    if (!base) base = plat_u8_to_path(".");
    if (!base) return NULL;
    dir = join_w(base, "anime-denoise");
    free(base);
    if (!dir) return NULL;
    out = join_w(dir, "models");
    free(dir);
    return out;
#endif
}

/* The JSON "device" string: "dml:<adapter>" or "cpu". */
static void device_json(const eng *e, int gpu, char *out, size_t cap)
{
    const char *name;
    static const char *const prefixes[] = {
        "NVIDIA GeForce ", "NVIDIA ", "AMD ", "Intel(R) ", "Intel "
    };
    if (!e || !gpu) {
        snprintf(out, cap, "cpu");
        return;
    }
    name = eng_device_name(e);
    if (!name) name = "gpu";
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
        size_t n = strlen(prefixes[i]);
        if (plat_strnicmp(name, prefixes[i], n) == 0) {
            name += n;
            break;
        }
    }
    {
        char tmp[256];
        char *cut;
        snprintf(tmp, sizeof tmp, "%s", name);
        cut = strstr(tmp, " (DirectML)");
        if (cut) *cut = 0;
        snprintf(out, cap, "dml:%.59s", tmp);
    }
}

/* ------------------------------------------------------------------------- */
/* Pixel plumbing: img RGBA <-> engine packed RGB                            */
/* ------------------------------------------------------------------------- */

static eng_image px_to_rgb(const img_pixels *p)
{
    eng_image im = { p->w, p->h, NULL };
    size_t n = (size_t)p->w * (size_t)p->h;
    im.rgb = (uint8_t *)malloc(n * 3);
    if (!im.rgb) return im;
    for (size_t i = 0; i < n; i++) {
        im.rgb[i * 3 + 0] = p->rgba[i * 4 + 0];
        im.rgb[i * 3 + 1] = p->rgba[i * 4 + 1];
        im.rgb[i * 3 + 2] = p->rgba[i * 4 + 2];
    }
    return im;
}

/* Result RGB + the source's alpha -> straight RGBA8 for PNG output. */
static bool rgb_to_px(const eng_image *im, const img_pixels *src, img_pixels *out)
{
    size_t n;
    if (im->w != src->w || im->h != src->h) return false;
    n = (size_t)im->w * (size_t)im->h;
    if (!img__pixels_alloc(im->w, im->h, out)) return false;
    for (size_t i = 0; i < n; i++) {
        out->rgba[i * 4 + 0] = im->rgb[i * 3 + 0];
        out->rgba[i * 4 + 1] = im->rgb[i * 3 + 1];
        out->rgba[i * 4 + 2] = im->rgb[i * 3 + 2];
        out->rgba[i * 4 + 3] = src->rgba[i * 4 + 3];
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* The comparison sheet (implemented in sheet.c)                             */
/* ------------------------------------------------------------------------- */

bool cli_sheet_make(const eng_path_t *exe_dir, const img_pixels *cells[8],
                    const char *const labels[8], img_pixels *out);

/* Labels: denoise.py's LABELS with {lam} filled in. */
static void sheet_labels(double lam, const char *out[8], char buf_cugan[64],
                         char buf_esr[64])
{
    snprintf(buf_cugan, 64, "CUGAN @60%% + L0 %g", lam);
    snprintf(buf_esr, 64, "ESRGAN @60%% + L0 %g (default)", lam);
    out[0] = "original";
    out[1] = "L0 0.0005";
    out[2] = "L0 0.001";
    out[3] = "APISR";
    out[4] = "CUGAN @60%";
    out[5] = buf_cugan;
    out[6] = "ESRGAN @60%";
    out[7] = buf_esr;
}

/* ------------------------------------------------------------------------- */
/* Per-image work                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    eng *e;
    const cli_opts *o;
    prog_ctx prog;
    char dev[64];
    const eng_path_t *exe_dir;
} job;

/* Runs one method on `src` (results in `res`/`res_shared`). `cache` holds the
 * per-model upscale so --all only computes each base model once. */
static int run_method(job *j, const eng_image *src, eng_image cache[3],
                      const method_def *m, eng_image *res, int *res_shared,
                      char *err, size_t cap)
{
    int r;
    if (m->model == ENG_M_NONE) {
        r = eng_l0(src, m->fixed_lam, res, 0, on_progress, &j->prog, &g_cancel);
        if (r < 0) snprintf(err, cap, "L0 smoothing failed");
        return r;
    }
    if (!cache[m->model].rgb) {
        eng_params p;
        p.model = m->model;
        p.down = j->o->down_pct / 100.0;
        p.l0 = 0.0;
        r = eng_upscale(j->e, src, &p, &cache[m->model],
                        on_progress, &j->prog, &g_cancel, err, cap);
        if (r) return r;
    }
    if (m->l0 == L0_ARG && j->o->l0 > 0.0) {
        r = eng_l0(&cache[m->model], j->o->l0, res, 0,
                   on_progress, &j->prog, &g_cancel);
        if (r < 0) snprintf(err, cap, "L0 smoothing failed");
        return r;
    }
    /* lam 0 or a plain *_60 method: the upscale IS the result */
    *res = cache[m->model];
    *res_shared = 1;
    return 0;
}

static void report_item(const cli_opts *o, const char *input, const char *output,
                        const char *method, int w, int h, unsigned long long ms,
                        const char *dev)
{
    if (o->json) {
        char i[2048], pth[2048];
        json_escape(input, i, sizeof i);
        json_escape(output, pth, sizeof pth);
        printf("{\"input\":\"%s\",\"output\":\"%s\",\"method\":\"%s\","
               "\"w\":%d,\"h\":%d,\"ms\":%llu,\"device\":\"%s\"}\n",
               i, pth, method, w, h, ms, dev);
    } else {
        printf("%s\n", output);
    }
    fflush(stdout);
}

/* Decode + run + write one image. Returns 0 ok / 1 cancelled / -1 failed. */
static int process_one(job *j, const char *input_label, const img_pixels *px,
                       const unsigned char *icc, size_t icc_size,
                       const char *out_dir, const char *stem)
{
    eng_image src = { 0, 0, NULL };
    eng_image cache[3] = { {0,0,NULL}, {0,0,NULL}, {0,0,NULL} };
    eng_image results[N_METHODS] = {{0}};
    int shared[N_METHODS] = {0};
    img_pixels sheet_cells[8];
    char err[512];
    int r = 0, any = 0;
    const method_def *sel[N_METHODS];
    int n_sel = 0;

    memset(sheet_cells, 0, sizeof sheet_cells);
    err[0] = 0;
    src = px_to_rgb(px);
    if (!src.rgb) {
        err_line("%s: out of memory", input_label);
        return -1;
    }

    if (j->o->all) {
        for (size_t i = 0; i < N_METHODS; i++) sel[n_sel++] = &METHODS[i];
    } else {
        sel[n_sel++] = find_method(j->o->method);
    }

    for (int i = 0; i < n_sel && !g_cancel; i++) {
        const method_def *m = sel[i];
        unsigned long long t0 = plat_tick_ms();
        int idx = method_index(m->name);
        r = run_method(j, &src, cache, m, &results[idx], &shared[idx], err, sizeof err);
        if (r != 0) {
            if (r == 1) err_line("%s: cancelled", input_label);
            else err_line("%s: %s (%s)", input_label, m->name, err[0] ? err : "processing failed");
            goto done;
        }
        {
            img_pixels outp = { NULL, 0, 0 };
            char outpath[2500];   /* outdir(<1800) + stem(<600) + _method.png */
            if (!rgb_to_px(&results[idx], px, &outp)) {
                err_line("%s: %s: output packing failed", input_label, m->name);
                r = -1;
                goto done;
            }
            snprintf(outpath, sizeof outpath, "%s%s_%s.png", out_dir, stem,
                     m->name);
            if (!img_encode_png_icc(outpath, &outp, icc, icc_size)) {
                err_line("%s: cannot write %s", input_label, outpath);
                img_free(&outp);
                r = -1;
                goto done;
            }
            report_item(j->o, input_label, outpath, m->name,
                        outp.w, outp.h, plat_tick_ms() - t0, j->dev);
            sheet_cells[idx + 1] = outp;      /* owned by us, freed below */
        }
    }

    if (r == 0 && j->o->all) {
        img_pixels sheet = { NULL, 0, 0 };
        const img_pixels *cells[8];
        const char *labels[8];
        char lc[64], le[64];
        unsigned long long t0 = plat_tick_ms();
        char outpath[2500];

        sheet_labels(j->o->l0, labels, lc, le);
        cells[0] = px;   /* "original" */
        for (int s = 1; s < 8; s++) {
            const method_def *m = find_method(SHEET_ORDER[s]);
            cells[s] = &sheet_cells[method_index(m->name) + 1];
        }
        if (!cli_sheet_make(j->exe_dir, cells, labels, &sheet)) {
            err_line("%s: sheet composition failed", input_label);
            r = -1;
        } else {
            snprintf(outpath, sizeof outpath, "%s%s_sheet.png", out_dir, stem);
            if (!img_encode_png(outpath, &sheet)) {
                err_line("%s: cannot write %s", input_label, outpath);
                r = -1;
            } else {
                report_item(j->o, input_label, outpath, "sheet",
                            sheet.w, sheet.h, plat_tick_ms() - t0, j->dev);
            }
            img_free(&sheet);
        }
    }
    any = 1;

done:
    for (size_t i = 0; i < N_METHODS; i++) {
        if (!shared[i]) eng_image_free(&results[i]);
        if (i >= 1 && i <= 7) img_free(&sheet_cells[i]);
    }
    for (int i = 0; i < 3; i++) eng_image_free(&cache[i]);
    free(src.rgb);
    return r != 0 ? r : (any ? 0 : -1);
}

/* ------------------------------------------------------------------------- */
/* Download                                                                  */
/* ------------------------------------------------------------------------- */

static void dl_progress(void *ud, unsigned long long done, unsigned long long total)
{
    prog_ctx *p = (prog_ctx *)ud;
    unsigned long long now;
    if (p->quiet) return;
    now = plat_tick_ms();
    if (total && done < total && now - p->last_ms < 100) return;
    p->last_ms = now;
    if (total)
        fprintf(stderr, "[download] %llu%%\n", done * 100 / total);
    else
        fprintf(stderr, "[download] %llu bytes\n", done);
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

/* The whole program, on a UTF-8 argv. Entry points at the bottom produce
 * that argv (wmain converts; POSIX main already has it). */
static int cli_run(int argc, char **argv)
{
    cli_opts o;
    char err[512];
    job j;
    int rc = 0;

    rc = parse_args(argc - 1, argv + 1, &o);   /* argv[0] is the exe path */
    if (o.show_help) {
        fputs(USAGE, stdout);
        return 0;
    }
    if (o.show_version) {
        printf("anime-denoise %s\n", APP_VERSION);
        return 0;
    }
    if (rc) return rc;

    memset(&j, 0, sizeof j);
    j.o = &o;
    j.prog.quiet = o.quiet;
    j.exe_dir = plat_exe_dir();
    if (!j.exe_dir) {
        err_line("anime-denoise: cannot locate the executable directory");
        return 3;
    }

    /* ---------------- download-only mode ---------------- */
    if (o.download) {
        eng_model m;
        eng_config cfg;
        if (plat_stricmp(o.download, "cugan") == 0) m = ENG_M_CUGAN;
        else if (plat_stricmp(o.download, "apisr") == 0) m = ENG_M_APISR;
        else {
            err_line("anime-denoise: --download expects cugan or apisr, got '%s'",
                     o.download);
            return 2;
        }
        /* the weights land in --models (the user dir); the bundled skeleton
         * is copied out of <exe>/models next to them to complete the pair */
        memset(&cfg, 0, sizeof cfg);
        cfg.models_dir = join_w(j.exe_dir, PLAT_LIT("models"));
        cfg.user_models_dir = o.models_dir ? plat_u8_to_path(o.models_dir)
                                         : default_models_dir();
        if (!cfg.models_dir || !cfg.user_models_dir) {
            free((void *)cfg.models_dir);
            free((void *)cfg.user_models_dir);
            return 3;
        }
        fprintf(stderr, "downloading %s weights...\n", o.download);
        rc = eng_model_download(&cfg, m, dl_progress, &j.prog, &g_cancel,
                                err, sizeof err);
        free((void *)cfg.models_dir);
        free((void *)cfg.user_models_dir);
        if (rc == 0) {
            err_line("done");
            return 0;
        }
        err_line("download failed: %s", err);
        return 1;
    }

    /* ---------------- engine setup ---------------- */
    {
        int need[3] = { 0, 0, 0 };
        if (o.all) {
            need[ENG_M_ESRGAN] = need[ENG_M_CUGAN] = need[ENG_M_APISR] = 1;
        } else {
            const method_def *m = find_method(o.method);
            if (m->model != ENG_M_NONE) need[m->model] = 1;
        }
        if (need[0] || need[1] || need[2]) {
            eng_config cfg;
            eng_path_t *runtime, *models;
            memset(&cfg, 0, sizeof cfg);
            runtime = join_w(j.exe_dir, PLAT_LIT("runtime"));
            models = join_w(j.exe_dir, PLAT_LIT("models"));
            cfg.runtime_dir = runtime;
            cfg.models_dir = models;
            cfg.user_models_dir = o.models_dir ? plat_u8_to_path(o.models_dir)
                                             : default_models_dir();
            cfg.device = o.device;
            cfg.threads = 0;
            j.e = eng_create(&cfg, err, sizeof err);
            if (!j.e) {
                err_line("anime-denoise: engine unavailable: %s", err);
                free(runtime);
                free(models);
                free((void *)cfg.user_models_dir);
                return 3;
            }
            for (int m = 0; m <= ENG_M_APISR; m++) {
                eng_model_state st;
                if (!need[m]) continue;
                if (m != ENG_M_ESRGAN) {
                    eng_model_status(&cfg, (eng_model)m, &st);
                    if (!st.available) {
                        err_line("anime-denoise: %s model unavailable: missing %s",
                                 m == ENG_M_CUGAN ? "cugan" : "apisr",
                                 st.missing[0] ? st.missing : "model files");
                        err_line("  fetch the weights with: anime-denoise --download %s%s%s",
                                 m == ENG_M_CUGAN ? "cugan" : "apisr",
                                 o.models_dir ? " --models " : "",
                                 o.models_dir ? o.models_dir : "");
                        eng_destroy(j.e);
                        free(runtime);
                        free(models);
                        free((void *)cfg.user_models_dir);
                        return 3;
                    }
                }
                if (!eng_prepare(j.e, (eng_model)m, err, sizeof err)) {
                    err_line("anime-denoise: %s", err);
                    eng_destroy(j.e);
                    free(runtime);
                    free(models);
                    free((void *)cfg.user_models_dir);
                    return 3;
                }
            }
            device_json(j.e, eng_using_gpu(j.e), j.dev, sizeof j.dev);
            if (eng_note(j.e) && eng_note(j.e)[0])
                err_line("note: %s", eng_note(j.e));
            free(runtime);
            free(models);
            free((void *)cfg.user_models_dir);
        } else {
            snprintf(j.dev, sizeof j.dev, "cpu");
        }
    }

    /* ---------------- stdin image ---------------- */
    if (o.use_stdin) {
        img_pixels px = { NULL, 0, 0 };
        unsigned char *icc = NULL, *buf = NULL;
        size_t icc_size = 0, len = 0, cap = 1 << 20;
        img_pixels outp;
        eng_image src, res;
        char stem[64] = "stdin";
        char outdir[1200];
        const method_def *m = find_method(o.method);

        buf = (unsigned char *)malloc(cap);
        if (buf) {
            for (;;) {
                size_t got;
                if (len + 65536 > cap) {
                    cap *= 2;
                    unsigned char *nb = (unsigned char *)realloc(buf, cap);
                    if (!nb) break;
                    buf = nb;
                }
                got = fread(buf + len, 1, 65536, stdin);
                len += got;
                if (got < 65536) break;
            }
        }
        if (!len || !img_decode_memory_ex(buf, len, &px, &icc, &icc_size)) {
            err_line("stdin: not a decodable image");
            free(buf);
            if (j.e) eng_destroy(j.e);
            return 1;
        }
        free(buf);

        if (o.use_stdout) {
            void *png = NULL;
            size_t n = 0;
            src = px_to_rgb(&px);
            res.rgb = NULL;
            eng_image cache[3] = {{0,0,NULL},{0,0,NULL},{0,0,NULL}};
            int shared = 0;
            rc = run_method(&j, &src, cache, m, &res, &shared, err, sizeof err);
            free(src.rgb);
            for (int i = 0; i < 3; i++) eng_image_free(&cache[i]);
            if (rc != 0) {
                err_line("stdin: %s", rc == 1 ? "cancelled" : err);
                img_free(&px);
                img_free_icc(icc);
                if (j.e) eng_destroy(j.e);
                return rc == 1 ? 1 : 1;
            }
            if (!rgb_to_px(&res, &px, &outp)) {
                err_line("stdin: output packing failed");
                if (!shared) eng_image_free(&res);
                img_free(&px);
                img_free_icc(icc);
                if (j.e) eng_destroy(j.e);
                return 1;
            }
            if (!shared) eng_image_free(&res);
            if (!img_encode_png_memory_icc(&outp, icc, icc_size, &png, &n)) {
                err_line("stdin: PNG encode failed");
                img_free(&outp);
                img_free(&px);
                img_free_icc(icc);
                if (j.e) eng_destroy(j.e);
                return 1;
            }
            fwrite(png, 1, n, stdout);
            fflush(stdout);
            free(png);
            img_free(&outp);
            img_free(&px);
            img_free_icc(icc);
            if (j.e) eng_destroy(j.e);
            return 0;
        }

        /* stdin + file output: --out required, else ./stdin_denoise */
        if (o.out_dir) {
            snprintf(outdir, sizeof outdir, "%s/", o.out_dir);
        } else {
            snprintf(outdir, sizeof outdir, "stdin_denoise/");
        }
        mkdirs(outdir);
        rc = process_one(&j, "stdin", &px, icc, icc_size, outdir, stem);
        img_free(&px);
        img_free_icc(icc);
        if (j.e) eng_destroy(j.e);
        return rc == 0 ? 0 : 1;
    }

    /* ---------------- file / folder inputs ---------------- */
    for (int i = 0; i < o.n_inputs && !g_cancel; i++) {
        const char *input = o.inputs[i];
        char **files = NULL;
        int nfiles = 0;
        int owned = 0;   /* scan_dir allocated the path strings */

        if (is_dir(input)) {
            files = scan_dir(input, &nfiles);
            owned = 1;
            if (!files || !nfiles) {
                err_line("%s: no supported images in folder", input);
                rc = 1;
                continue;
            }
        } else {
            files = (char **)calloc(1, sizeof(char *));
            files[0] = (char *)input;
            nfiles = 1;
        }

        for (int f = 0; f < nfiles && !g_cancel; f++) {
            const char *path = files[f];
            img_pixels px = { NULL, 0, 0 };
            unsigned char *icc = NULL;
            size_t icc_size = 0;
            char dir[1100], stem[600], outdir[1800];

            if (!img_decode_file_ex(path, &px, &icc, &icc_size)) {
                err_line("%s: cannot decode", path);
                rc = 1;
                continue;
            }
            split_path(path, dir, sizeof dir, stem, sizeof stem);
            if (o.out_dir) {
                snprintf(outdir, sizeof outdir, "%s/", o.out_dir);
            } else {
                snprintf(outdir, sizeof outdir, "%s%s_denoise/", dir, stem);
            }
            if (!mkdirs(outdir)) {
                err_line("%s: cannot create %s", path, outdir);
                img_free(&px);
                img_free_icc(icc);
                rc = 1;
                continue;
            }
            if (process_one(&j, path, &px, icc, icc_size, outdir, stem) != 0)
                rc = 1;
            img_free(&px);
            img_free_icc(icc);
        }
        if (owned)
            for (int f = 0; f < nfiles; f++) free(files[f]);
        free(files);
    }

    if (j.e) eng_destroy(j.e);
    img_shutdown();
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
int wmain(int argc, wchar_t **wargv)
{
    char **argv;
    int rc, i;

    install_cancel_handler();
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    argv = (char **)calloc((size_t)argc, sizeof(char *));
    if (!argv) return 2;
    for (i = 0; i < argc; i++) argv[i] = w2u(wargv[i]);
    rc = cli_run(argc, argv);
    for (i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return rc;
}
#else
int main(int argc, char **argv)
{
    install_cancel_handler();
    /* POSIX stdio is binary by nature; nothing to _setmode. */
    return cli_run(argc, argv);
}
#endif

/* ------------------------------------------------------------------------- */
/* Download-helper hooks (GUI substitutes, see header comment)                */
/* ------------------------------------------------------------------------- */

bool upd__stop_requested(void)
{
    return g_cancel != 0;
}

const char *cfg_data_dir(void)
{
    return "";   /* the CLI never writes under the settings tree */
}

void log_write(log_level lvl, const char *fmt, ...)
{
    va_list ap;
    (void)lvl;
    fputs("log: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
