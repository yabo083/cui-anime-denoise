/* App state and the frame entry points.
 *
 * `main.c` only creates the window; everything the UI does lives in `app.c`
 * behind these three calls, so a harness can drive the same code without a
 * visible window (see lab/测试工程).
 *
 * Layout contract (docs/fluent-redesign.md): a 40px title bar and one canvas
 * that fills the window. Everything else -- the control pill, the filmstrip
 * tray, the protection toolbar, the tune card -- is a floating layer drawn
 * inside that canvas, with its hit rect published in app_state.
 */
#ifndef APP_H
#define APP_H

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdbool.h>
#include <stdint.h>

#include "chrome.h"
#include "config.h"
#include "denoiser.h"
#include "img.h"
#include "mask.h"
#include "update.h"

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

#define APP_TITLE      "Anime Denoise"
#define APP_ID         "AnimeDenoise"
#define MAX_JOBS       64
#define PATH_CAP       1024
#define IMAGE_FILTER   "png;jpg;jpeg;webp;bmp;gif;tif;tiff;avif;heic;heif"

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

/* Which floating-layer control currently owns the pressed pointer. The
 * overlays are custom-drawn inside the stage canvas, so capture is explicit:
 * the pointer down is claimed on the press frame and released on button up. */
typedef enum ui_ctl {
    CTL_NONE = 0,
    CTL_TUNE_DOWN,        /* tune card: downscale slider */
    CTL_TUNE_L0,          /* tune card: L0 slider */
    CTL_P_BRUSH,          /* protect bar: brush size */
    CTL_P_MIX,            /* protect bar: restore strength */
    CTL_P_FEATHER,        /* protect bar: feather */
    CTL_PILL_SEG          /* a segmented control inside the pill */
} ui_ctl;

/* zoom = screen points per image pixel; cx,cy = image point at the view centre */
typedef struct view2d {
    float zoom;
    float cx, cy;
} view2d;

typedef struct job {
    char path[PATH_CAP];       /* UTF-8 source path */
    char name[192];            /* basename */
    char stem[184];            /* basename without extension */
    char out[PATH_CAP];        /* denoised PNG, written by the host on DONE */
    char out_protected[PATH_CAP];

    int id;                    /* denoiser job id, 0 when idle */
    int src_key;               /* stable identity for the SR cache (path+file stamp) */
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
    unsigned char *icc;        /* source ICC profile (img_free_icc) */
    size_t icc_size;
    eng_image res;             /* full-res denoised RGB (eng_image_free) */
    bool has_alpha;
    cui_texture orig_tex;
    int disp_w, disp_h;
    cui_texture res_tex;       /* denoised / composited preview */
    int res_w, res_h;
    cui_texture fade_tex;      /* previous result texture, kept for the cross-fade */
    int fade_w, fade_h;
    double fade_at;            /* when res_tex was last swapped */
    cui_texture raw_tex;       /* unprotected result preview (lazy, protect mode) */
    int raw_w, raw_h;
    cui_texture mask_tex;      /* overlay of the feathered mask */
    int mask_tex_w, mask_tex_h;
    bool mask_tex_dirty;
    double mask_tex_at;        /* last overlay rebuild, to throttle strokes */
    cui_texture thumb_tex;
    int thumb_w, thumb_h;
    cui_texture bg_tex;        /* blurred result: the acrylic canvas floor */
    int bg_w, bg_h;
    bool bg_dirty;             /* set whenever res_tex is replaced */

    mask m;
    mask_hist *hist;
    mask_edges edges;
    bool edges_ready;
    bool mask_active;          /* any pixel > 0 */
    int feather_px;
    double mask_fade_from;     /* last mask edit: the overlay fades after 0.7 s */
    bool composite_dirty;      /* mask edited since the last composite */
    bool compositing;
    bool composite_pending;
    double dirty_at;           /* time of the last mask edit */
    double saved_at;           /* when the output PNG was (re)written */

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

    /* protection mode: the pill sinks away, the protect toolbar slides in */
    bool protect;
    bool mask_eye;             /* 显示范围: mask overlay stays on */
    bool view_unprotected;     /* protect bar: show the denoised result as-is */
    uint8_t *protect_backup;   /* mask snapshot taken on entry, restored by 取消 */

    /* floating layers */
    bool tune_open;            /* tune card above the pill */
    int drag_ctl;              /* ui_ctl that owns the current pointer capture */
    int tray_page;             /* filmstrip page when the tray overflows */

    /* denoiser */
    bool engine_ready;         /* a good READY arrived from the engine thread */
    bool start_wanted;
    bool start_shown;
    char device[160];
    char device_note[128];     /* READY message, e.g. the GPU->CPU fallback note */
    char engine_error[512];
    int next_id;
    bool booting;              /* engine bring-up or model download in progress */
    double boot_deadline;      /* when a silent engine counts as failed */
    float boot_value;
    char boot_stage[160];
    /* model downloads (settings sheet): indexed by eng_model */
    bool dl_busy[4];
    unsigned long long dl_done[4], dl_total[4];

    /* hit rects of the floating controls, filled by the paint pass and read
     * by the input pass one frame later (the test harness also uses them) */
    cui_rect r_pill;
    cui_rect r_preset, r_view;                    /* pill segmented controls */
    cui_rect r_pill_tune, r_pill_brush, r_pill_save, r_pill_add;
    cui_rect r_tray;                              /* whole tray, incl. paddings */
    cui_rect r_queue[MAX_JOBS];                   /* tray tiles */
    cui_rect r_tray_add, r_tray_prev, r_tray_next;
    cui_rect r_tray_x[MAX_JOBS];                  /* per-tile hover remove */
    cui_rect r_protect;                           /* protect toolbar */
    cui_rect r_tool[3];                           /* brush / eraser / lasso */
    cui_rect r_p_slider[3];                       /* brush / mix / feather */
    cui_rect r_undo, r_redo;
    cui_rect r_peye, r_pinvert, r_pclear, r_punprot, r_pdone, r_pcancel;
    cui_rect r_tune;                              /* tune card */
    cui_rect r_tune_slider[2];                    /* down, l0 */
    cui_rect r_tune_reset;
    cui_rect r_title_name, r_title_device, r_title_update, r_title_help, r_title_settings;
    cui_rect r_min, r_max, r_close;
    cui_rect r_banner, r_banner_retry;
    cui_rect r_empty;                             /* empty-state card */
    cui_rect r_stage_buttons;                     /* union of pill+tray+protect+tune: stage input skips it */

    /* stage */
    cui_rect stage;
    bool stage_valid;
    bool dragging_split;
    bool painting;
    bool stroke_open;
    float paint_x, paint_y;    /* last point in image pixels */
    bool lasso_active;
    float *lasso;              /* dense x,y polyline: every committed wire point */
    int lasso_n, lasso_cap;
    int *lasso_anchor;         /* vertex indices into lasso where each segment ends */
    int lasso_anchor_n, lasso_anchor_cap;
    float lasso_hover_x, lasso_hover_y;
    int *lasso_live;           /* wire from the last point to the cursor */
    int lasso_live_n;

    /* ui */
    bool want_browse, want_quit, want_clear, want_restart, want_paste;
    bool want_save, want_save_as;
    bool show_help, show_settings, show_about, show_update;
    bool auto_update;          /* live value of the settings checkbox */
    char toast[192];
    double toast_until;

    /* theme: re-apply the Fluent palette whenever the mode flips */
    int theme_applied;         /* -1 none, else cui_theme_mode last applied */

    /* settings land on disk when they stop changing, not per slider tick */
    bool settings_dirty;
    double settings_dirty_at;

    char **pending;            /* paths from the command line, added on the first frame */
    int pending_n;

    upd_info upd;
    bool bench;                /* ANIME_DENOISE_BENCH=1: no engine, no network */
    char upd_manifest[512];
    double upd_checked_at;
} app_state;


/* Denoise presets (shared with the pill). */
typedef struct preset_def {
    const char *name;
    const char *hint;
    int down_pct;
    float l0;
} preset_def;

extern const preset_def PRESETS[3];
#define PRESET_CUSTOM 3
#define DEFAULT_PRESET 1

/* Frame entry points. app_frame() polls the denoiser, handles input and draws;
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
/* Queue an image without the engine (returns false when it cannot be decoded). */
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

/* Job lifecycle, loader queue, denoiser submission (app_jobs.c). */
int job_find(const app_state *st, int dn_id);
void job_free(cui_ctx *ui, job *j);
void out_paths(app_state *st, job *j);
void job_focus(app_state *st, int index);
void job_remove(cui_ctx *ui, app_state *st, int index);
void job_clear_finished(cui_ctx *ui, app_state *st);
job *app_job_add_now(app_state *st, const char *path);   /* synchronous decode; tests/paste */
void enqueue_path(app_state *st, const char *path);
void pump_load_queue(app_state *st);
eng_device device_pref(const app_state *st);
void engine_start_now(app_state *st);
void engine_submit_next(app_state *st);
img_pixels *job_raw_result_px(job *j, img_pixels *out);  /* denoise RGB + source alpha, no mask */
void job_apply_done(cui_ctx *ui, app_state *st, job *j, dn_event *ev);
void request_composite(app_state *st, job *j);
void flush_composite(app_state *st);
/* Parameter edits re-run the selected job; a running job is cancelled first. */
void job_rerun(app_state *st, job *j);

/* Keys, drops, paste, save, file dialogs (app_input.c). */
void app_handle_keys(cui_ctx *ui, app_state *st);
void app_handle_drops(cui_ctx *ui, app_state *st);
void app_do_paste(app_state *st);
void app_do_save(app_state *st, bool save_as);
/* Expands the save_name settings template into a 另存为 default name. */
void app_save_default_name(const app_state *st, const job *j, char *out, size_t cap);
void app_browse_dialog(cui_app *app, app_state *st);
void app_reveal_in_explorer(const char *utf8_path);
void protect_enter(app_state *st);
void protect_done(app_state *st);
void protect_cancel(app_state *st);
void protect_exit(app_state *st);
void brush_step(app_state *st, float factor);

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
void lasso_reset(app_state *st);
/* Lasso interaction hooks, implemented in ui_lasso.c and driven by the key
 * bindings in app_input.c (Enter / Backspace / Alt). */
bool lasso_active(const app_state *st);
void lasso_close_now(app_state *st, job *j, bool subtract);
void lasso_drop_anchor(app_state *st);

/* Fluent palette: reapplied when the theme mode flips (ui_fluent.c). */
void apply_fluent_theme(cui_ctx *ui, app_state *st);

/* Drawing entry points. */
void ui_titlebar(cui_ctx *ui, app_state *st);
void draw_stage_area(cui_ctx *ui, app_state *st, float height);
void ui_dialogs(cui_ctx *ui, app_state *st);
void stage_handle_input(app_state *st, job *j);
/* Feeds the stage's press-edge trackers while a modal gates input. */
void stage_track_buttons(cui_ctx *ui);
void stage_fit(job *j, cui_rect r);
void stage_zoom_center(app_state *st, job *j, float factor);
void rebuild_mask_texture(app_state *st, job *j);
void ensure_edges(job *j);

/* Stage-space helpers shared by ui_stage.c and ui_lasso.c. */
float stage_image_w(const job *j);
float stage_image_h(const job *j);
void stage_img_to_screen(const job *j, cui_rect r, float ix, float iy, float *sx, float *sy);
void stage_screen_to_img(const job *j, cui_rect r, float sx, float sy, float *ix, float *iy);
void lasso_handle_input(app_state *st, job *j, float ix, float iy, bool left_press, bool right_press);
void lasso_paint(cui_ctx *ui, app_state *st, job *j, cui_rect r);

/* Floating layers (ui_float.c): empty state, pill, tray, protect bar, tune
 * card, engine banner, the quick-rerun bar. ui_overlay_input returns true
 * when the pointer is over a floating control, so the stage skips it. */
void ui_empty_paint(cui_ctx *ui, app_state *st, cui_rect r);
void ui_float_paint(cui_ctx *ui, app_state *st, cui_rect r);
bool ui_overlay_input(app_state *st, job *j);
/* Feeds the float layer's press-edge tracker while a modal gates input. */
void ui_overlay_track(cui_ctx *ui);
void ui_tray_page(app_state *st, int dir);

/* The custom-drawn Fluent primitives (ui_seg_draw, ui_button_draw,
 * ui_slider_draw, ui_hit, fl_*) live in ui_fluent.h. */
void ui_titlebar_input(app_state *st);   /* title-bar clicks, ui_shell.c */

#define BRUSH_STEP    0.85f   /* [ and ] scale the brush, same feel as the web UI */
#define BRUSH_KEY_MIN   2.0f
#define BRUSH_KEY_MAX 300.0f

#endif /* APP_H */
