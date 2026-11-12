/* App state and the frame entry points.
 *
 * `main.c` only creates the window; everything the UI does lives in `app.c`
 * behind these three calls, so a harness can drive the same code without a
 * visible window (see lab/测试工程).
 */
#ifndef APP_H
#define APP_H

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdbool.h>
#include <stdint.h>

#include "backend.h"
#include "chrome.h"
#include "config.h"
#include "img.h"
#include "mask.h"
#include "runtime.h"
#include "update.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

#define APP_TITLE      "Anime Denoise"
#define APP_ID         "AnimeDenoise"
#define MAX_JOBS       64
#define PATH_CAP       1024
#define IMAGE_FILTER   "png;jpg;jpeg;webp;bmp;gif;tif;tiff;avif;heic;heif"

#define SIDEBAR_W      300.0f
#define TOPBAR_H       56.0f
#define STRIP_H        92.0f
#define HEAD_H         46.0f
#define PANE_PAD       14.0f
#define PANE_GAP       10.0f
#define DISPLAY_MAX    2560      /* textures above this are downscaled for display */
#define MASK_TEX_MAX   1400      /* the mask overlay texture */
#define COMPOSITE_DEBOUNCE 0.35  /* seconds of quiet before the mask is uploaded */


/* ------------------------------------------------------------------------ */
/* Types                                                                    */
/* ------------------------------------------------------------------------ */

typedef enum job_status {
    JOB_QUEUED = 0, JOB_RUNNING, JOB_DONE, JOB_ERROR, JOB_CANCELLED
} job_status;

typedef enum tool_mode {
    TOOL_VIEW = 0, TOOL_BRUSH, TOOL_ERASE, TOOL_LASSO
} tool_mode;

typedef enum view_mode {
    VIEW_COMPARE = 0, VIEW_ORIGINAL, VIEW_RESULT
} view_mode;

/* zoom = screen points per image pixel; cx,cy = image point at the view centre */
typedef struct view2d {
    float zoom;
    float cx, cy;
} view2d;

typedef struct job {
    char path[PATH_CAP];       /* UTF-8 source path */
    char name[192];            /* basename */
    char stem[184];            /* basename without extension */
    char out[PATH_CAP];        /* denoised PNG written by the worker */
    char out_protected[PATH_CAP];

    int id;                    /* worker job id, 0 when idle */
    int res_id;                /* id of the denoise whose result is cached in the worker */
    job_status status;
    bool cancel_sent;
    float progress;
    char phase[24];
    int ms;
    bool sr_cached;
    char error[512];

    int down_pct;              /* parameters captured when the job was queued */
    float l0;
    int res_down_pct;          /* parameters of the result currently shown */
    float res_l0;
    bool has_result;

    img_pixels orig;           /* full-res straight RGBA */
    bool has_alpha;
    cui_texture orig_tex;
    int disp_w, disp_h;
    cui_texture res_tex;       /* denoised / composited preview */
    int res_w, res_h;
    cui_texture mask_tex;      /* overlay of the feathered mask */
    int mask_tex_w, mask_tex_h;
    bool mask_tex_dirty;
    double mask_tex_at;        /* last overlay rebuild, to throttle strokes */
    cui_texture thumb_tex;
    int thumb_w, thumb_h;

    mask m;
    mask_hist *hist;
    mask_edges edges;
    bool edges_ready;
    bool mask_active;          /* any pixel > 0 */
    int feather_px;
    bool composite_dirty;      /* mask edited since the last composite */
    bool compositing;
    bool composite_pending;
    double dirty_at;           /* time of the last mask edit */

    view2d view;
    bool view_fitted;          /* the current zoom came from "fit", so a resize re-fits */
    float fit_w, fit_h;
    float split;
    char note[96];             /* "已完成 · 6.8 s" */
} job;

typedef struct app_state {
    cui_ctx *ui;

    job jobs[MAX_JOBS];
    int count;
    int selected;              /* -1 = nothing selected */

    app_settings set;

    /* live parameters; new images inherit these */
    int preset;                /* 0..2 preset, 3 custom */
    int down_pct;
    float l0;
    float brush_size, brush_mix, feather_px;

    tool_mode tool;
    view_mode view;
    bool peek_orig;            /* Z held */
    bool peek_mask;            /* M held */
    bool space_down;

    /* worker */
    bool worker_up;
    bool start_wanted;
    bool start_shown;
    char device[160];
    char worker_error[512];
    int next_id;
    bool booting;              /* init or model download in progress */
    double boot_deadline;      /* when a silent worker counts as failed */
    float boot_value;
    char boot_stage[160];

    /* rects of the controls the tests and the tour need (filled every frame) */
    cui_rect r_view, r_preset, r_undo, r_redo, r_clear_mask;
    cui_rect r_tool[3];
    cui_rect r_queue[MAX_JOBS];
    cui_rect r_strip[MAX_JOBS];

    /* stage */
    cui_rect stage;
    bool stage_valid;
    bool dragging_split;
    bool painting;
    bool stroke_open;
    float paint_x, paint_y;    /* last point in image pixels */
    bool lasso_active;
    float *lasso;              /* x,y pairs of committed vertices */
    int lasso_n, lasso_cap;
    float lasso_hover_x, lasso_hover_y;
    int *lasso_live;           /* path from the last vertex to the cursor */
    int lasso_live_n;

    /* ui */
    bool want_browse, want_quit, want_clear, want_restart, want_paste;
    bool want_save, want_save_as;
    bool show_help, show_settings, show_about, show_update;
    bool auto_update;          /* live value of the settings checkbox */
    char toast[192];
    double toast_until;

    /* settings land on disk when they stop changing, not per slider tick */
    bool settings_dirty;
    double settings_dirty_at;

    rt_info rt;                /* AI runtime state, polled every frame */
    char **pending;            /* paths from the command line, added on the first frame */
    int pending_n;

    upd_info upd;
    bool bench;                /* ANIME_DENOISE_BENCH=1: no worker, no network */
    char upd_manifest[512];
    double upd_checked_at;
} app_state;


/* Denoise presets (shared with the sidebar). */
typedef struct preset_def {
    const char *name;
    const char *hint;
    int down_pct;
    float l0;
} preset_def;

extern const preset_def PRESETS[3];
#define PRESET_CUSTOM 3
#define DEFAULT_PRESET 1

/* Frame entry points. app_frame() polls the worker, handles input and draws;
 * call it once per cui_app_frame(). */
void app_init(app_state *st);
void app_frame(cui_app *app, cui_ctx *ui, app_state *st);
void app_shutdown(cui_ctx *ui, app_state *st);

/* True when the app wants to quit (window close, Ctrl+Q, or the update flow). */
bool app_quit_requested(const app_state *st);

/* Test hooks: index of the selected job and a snapshot of the job list. */
int app_job_count(const app_state *st);
const job *app_job_at(const app_state *st, int index);
void app_test_select(app_state *st, int index);
/* Fills job `index` with a synthetic "done" result so the result UI can be
 * benchmarked without running the model. */
void app_test_fake_result(app_state *st, int index);
/* Queue an image without the worker (returns false when it cannot be decoded). */
bool app_test_add_image(app_state *st, const char *path);
void app_test_enqueue(app_state *st, const char *path);
int app_test_load_pending(void);
void app_test_set_preset(app_state *st, int index);
job *app_test_selected(app_state *st);


/* Strings, paths, time (app.c). */
void str_copy(char *dst, size_t cap, const char *src);
const char *path_basename(const char *path);
void path_join(char *out, size_t cap, const char *dir, const char *leaf);
void path_stem(char *out, size_t cap, const char *name);
void utf8_to_wide(const char *s, wchar_t *out, int cap);
void wide_to_utf8(const wchar_t *s, char *out, int cap);
bool path_exists(const char *utf8);
bool file_copy(const char *from, const char *to);
double now_seconds(void);
bool job_running(const app_state *st);
void worker_submit_next(app_state *st);
void enqueue_path(app_state *st, const char *path);
void pump_load_queue(app_state *st);

/* Shared helpers (defined in app.c; used by ui_shell.c and ui_stage.c). */
void toast(app_state *st, const char *msg);
void fmt_ms(int ms, char *buf, size_t cap);
void fmt_l0(float v, char *buf, size_t cap);
const char *status_label(job_status s);
cui_tone status_tone(job_status s);
job *job_selected(app_state *st);
void apply_preset(app_state *st, int index);
int preset_of(int down_pct, float l0);
void settings_from_state(app_state *st);
void mask_note(app_state *st, job *j);
void request_composite(app_state *st, job *j);
void job_focus(app_state *st, int index);
void lasso_reset(app_state *st);

/* Drawing entry points. */
void ui_topbar(cui_ctx *ui, app_state *st);
void ui_sidebar(cui_ctx *ui, app_state *st);
void ui_job_header(cui_ctx *ui, app_state *st, job *j);
void ui_strip(cui_ctx *ui, app_state *st);
void ui_status(cui_ctx *ui, app_state *st, job *j);
void ui_stage_toolbar(cui_ctx *ui, app_state *st, job *j);
void draw_stage_area(cui_ctx *ui, app_state *st, float height);
void ui_runtime_panel(cui_ctx *ui, app_state *st, float height);
void ui_dialogs(cui_ctx *ui, app_state *st);
void stage_handle_input(app_state *st, job *j);
void stage_fit(job *j, cui_rect r);
void rebuild_mask_texture(app_state *st, job *j);
void ensure_edges(job *j);

#endif /* APP_H */
