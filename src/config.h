/* Settings, paths and the log file.
 *
 * Everything lives under %LOCALAPPDATA%\AnimeDenoise:
 *   settings.json     user preferences (atomic write: tmp + MoveFileEx)
 *   logs\app.log      rotating log, 4 MiB x 3
 *   projects\         per-image sidecars (mask.png, meta.json)
 *   models\           user-downloaded .pth weights (CUGAN / APISR), optional
 *   updates\          downloaded installer (update.bin + update.json)
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define CFG_PATH_CAP 1024

typedef struct app_settings {
    /* denoise */
    int preset;              /* 0 soft, 1 balanced, 2 strong, 3 custom */
    int down_pct;            /* 10..100 */
    float l0;                /* 0..0.005 */
    int device;              /* 0 auto, 1 gpu, 2 cpu (eng_device) */

    /* brush */
    float brush_size;        /* 2..300 px */
    float brush_mix;         /* 0..100 % */
    float feather_px;        /* 0..150 */

    /* ui */
    int theme_mode;          /* 0 system, 1 light, 2 dark */
    int view_mode;           /* 0 compare, 1 original, 2 result */
    int auto_check_updates;  /* 1 on */
    int seen_welcome;        /* 1 after the first-run wizard */

    int window_w, window_h;
    int window_maximized;

    /* paths (empty = default) */
    char output_dir[CFG_PATH_CAP];
    char model_root[CFG_PATH_CAP];     /* legacy dev override (unused by the native engine) */
    char last_open_dir[CFG_PATH_CAP];
} app_settings;

void cfg_defaults(app_settings *s);

/* Creates the data directory tree and fills the paths below. Call once,
 * before anything else. Returns false if the data directory is unusable. */
bool cfg_init(void);
void cfg_shutdown(void);

const char *cfg_data_dir(void);       /* %LOCALAPPDATA%\AnimeDenoise */
const char *cfg_log_dir(void);
const char *cfg_models_dir(void);    /* <data>\models */
const char *cfg_default_out_dir(void);
const char *cfg_exe_dir(void);        /* where AnimeDenoise.exe lives */

bool cfg_load(app_settings *s);       /* missing/corrupt file -> defaults, true */
bool cfg_save(const app_settings *s);
bool cfg_reset(void);

/* ---- log ---- */
typedef enum log_level { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } log_level;

void log_open(void);                  /* called by cfg_init */
void log_close(void);
void log_write(log_level lvl, const char *fmt, ...);
#define log_debug(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __VA_ARGS__)

/* Last-chance handler: unhandled SEH exceptions, std::terminate-ish aborts and
 * pure-call crashes append a report to logs\crash-YYYYMMDD-HHMMSS.txt with the
 * module/offset, the exception code, a short stack backtrace and the tail of
 * app.log. Installed from main(). */
void log_install_crash_handler(void);

/* Writes a diagnostics zip (logs + settings + system info, paths redacted)
 * next to the log directory and returns its path. Returns false on failure. */
bool log_export_diagnostics(char *out_path, size_t cap);

#endif /* CONFIG_H */
