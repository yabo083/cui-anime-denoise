/* ui_models.c -- the settings sheet's "模型" section: one stock radio picker
 * row per engine model plus that model's lifecycle management (state caption,
 * download progress, download/delete actions).
 *
 * The registry view is uniform: every row reads eng_model_status() and the
 * dl_* slots in app_state, so a model is exactly one of {bundled-ready,
 * ready, not-downloaded, downloading, broken-half-state} per frame -- the
 * button cannot snap back to "下载" after a successful fetch because the row
 * no longer guesses; the downloaded pair is on disk or it is not.
 *
 * Selection semantics live in app_select_model / dn_resolve_model: only a
 * model whose pair is complete can be picked (the radio disables itself
 * otherwise); whatever the picker stores is a preference, and the denoiser
 * resolves it per job -- an unavailable pick falls back to the bundled
 * ESRGAN with a visible note instead of silently switching graphs.
 */

#include "app.h"
#include "ui_tokens.h"

#include <string.h>

/* Static per-model presentation: title on the radio, one-line trait under
 * it. State is dynamic and lives on the right, not in these strings. */
typedef struct {
    const char *title;
    const char *trait;
} ui_model_info;

static const ui_model_info UI_MODEL_INFO[3] = {
    { "ESRGAN", "内置模型 · 4× 放大重建" },
    { "CUGAN",  "降噪专用 · 2× · 仅在 CPU 运行" },
    { "APISR",  "真实感修复 · 2×" },
};

/* Selection/deletion are app-level actions (the bench drives them through
 * the same path the radio and buttons take). */

void app_select_model(app_state *st, eng_model m) {
    eng_model_state ms;
    if (m < ENG_M_ESRGAN || m > ENG_M_APISR) return;
    if (m != ENG_M_ESRGAN) {
        memset(&ms, 0, sizeof ms);
        dn_model_status(m, &ms);
        if (!ms.available) return;     /* the picker guards this too */
    }
    if (st->set.model == (int)m) return;
    st->set.model = (int)m;
    st->model_fb_noted = false;
    cfg_save(&st->set);
}

void app_delete_model(app_state *st, eng_model m) {
    if (m == ENG_M_ESRGAN || m > ENG_M_APISR) return;
    if (dn_delete_model(m)) {
        toast(st, "已删除下载的模型文件");
        /* deleting the selected model: the selection itself must move back
         * to the bundled default, not just its runtime resolution */
        if (st->set.model == (int)m) {
            st->set.model = ENG_M_ESRGAN;
            cfg_save(&st->set);
        }
        st->model_fb_noted = false;
    } else {
        toast(st, "没有可删除的文件");
    }
}

/* True when the picker selection cannot run as stored: the model section
 * shows the warning line and the device pill shows its warning tint. This
 * is derived, not stored -- delete the files and it appears, restore them
 * and it clears, no bookkeeping to go stale. */
bool app_model_sel_broken(app_state *st) {
    eng_model_state ms;
    if (st->set.model == ENG_M_ESRGAN ||
        st->set.model < ENG_M_ESRGAN || st->set.model > ENG_M_APISR) {
        return false;
    }
    if (st->dl_busy[st->set.model]) {
        return false;   /* on its way in: not broken, just not here yet */
    }
    memset(&ms, 0, sizeof ms);
    dn_model_status((eng_model)st->set.model, &ms);
    return !ms.available;
}

/* One registry row: [radio + trait] [status / action]. The per-model id
 * scope matters: two rows render a "下载"/"删除" button each, and same-label
 * buttons under one seed resolve to the SAME id -- they would share hot,
 * active, click and animation state (hovering one lit both; clicking one
 * fired both downloads). */
static void ui_model_row(cui_ctx *ui, app_state *st, eng_model m,
                         const eng_model_state *ms, int *sel) {
    bool busy = st->dl_busy[m];
    bool broken_half = !ms->available && (ms->weights_present || ms->skeleton_present);
    cui_push_id_int(ui, (int)m);
    cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_column(ui, .width = CUI_FILL, .gap = 0) {
            cui_radio(ui, UI_MODEL_INFO[m].title, sel, (int)m,
                      .disabled = !ms->available,
                      .description = UI_MODEL_INFO[m].trait);
        }
        if (busy) {
            float t = st->dl_total[m] ? (float)st->dl_done[m] / (float)st->dl_total[m]
                                    : 0.0f;
            cui_progress(ui, t, .width = CUI_PX(72), .height = 4);
        } else {
            if (m == ENG_M_ESRGAN) {
                cui_text(ui, ms->available ? "已就绪" : "内置文件缺失",
                         .size = UI_FS_CAPTION,
                         .color = ms->available ? CUI_COLOR_SUCCESS : CUI_COLOR_DANGER);
            } else if (ms->available) {
                cui_text(ui, "已就绪", .size = UI_FS_CAPTION, .color = CUI_COLOR_SUCCESS);
            } else if (broken_half) {
                cui_text(ui, "文件不完整", .size = UI_FS_CAPTION, .color = CUI_COLOR_WARNING);
            } else {
                cui_text(ui, "未下载", .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_MUTED);
            }
            if (m != ENG_M_ESRGAN) {
                if (!ms->available) {
                    /* the click also repairs a broken half-state for free:
                     * a finished .pth on disk skips the fetch and only the
                     * skeleton copy runs (see eng_model_download) */
                    if (cui_button(ui, "下载", .variant = CUI_BUTTON_OUTLINE,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_DOWNLOAD,
                                   .disabled = st->bench)) {
                        st->dl_busy[m] = true;
                        st->dl_done[m] = st->dl_total[m] = 0;
                        dn_download_model(m);
                    }
                }
                if (ms->weights_present) {
                    if (cui_button(ui, "删除", .variant = CUI_BUTTON_GHOST,
                                   .size = CUI_SIZE_SM, .icon = CUI_ICON_TRASH_2,
                                   .disabled = st->bench)) {
                        app_delete_model(st, m);
                    }
                }
            }
        }
    }
    cui_pop_id(ui);
}

void ui_models_section(cui_ctx *ui, app_state *st) {
    eng_model_state ms[3];
    int sel = st->set.model;
    for (int m = ENG_M_ESRGAN; m <= ENG_M_APISR; m++) {
        memset(&ms[m], 0, sizeof ms[m]);
        dn_model_status((eng_model)m, &ms[m]);
    }

    cui_text(ui, "模型", .font = CUI_FONT_MEDIUM, .size = 12.5f);
    cui_text(ui, "超分重建使用的模型，改动即时生效",
             .size = UI_FS_CAPTION, .color = CUI_COLOR_TEXT_SECONDARY);
    for (int m = ENG_M_ESRGAN; m <= ENG_M_APISR; m++) {
        ui_model_row(ui, st, (eng_model)m, &ms[m], &sel);
    }
    if (sel != st->set.model) {
        app_select_model(st, (eng_model)sel);
    }
    if (app_model_sel_broken(st)) {
        cui_text(ui, "所选模型不可用，处理时将自动改用内置模型。",
                 .size = UI_FS_CAPTION, .color = CUI_COLOR_WARNING);
    }
}
