/* ui_fluent.c -- the Fluent palette applied onto CUI's theme tokens, and the
 * custom-drawn control primitives the floating layers share. Values come
 * straight from docs/fluent-redesign.md §4-§6; the CUI token roles get the
 * closest spec colour so stock components (sheets, dialogs, toasts) follow
 * the same palette as the hand-drawn surfaces. */

#include "app.h"
#include "ui_fluent.h"
#include "ui_tokens.h"

#include <math.h>
#include <string.h>

bool fl_dark(cui_ctx *ui) { return cui_get_theme_mode(ui) == CUI_THEME_DARK; }

cui_color fl_acrylic(cui_ctx *ui) {
    return fl_dark(ui) ? CUI_RGBA(0x2C2C2CD9) : CUI_RGBA(0xFFFFFFD1);
}
cui_color fl_card_bg(cui_ctx *ui) {
    return fl_dark(ui) ? CUI_RGBA(0x32323299) : CUI_RGBA(0xFFFFFFA6);
}
cui_color fl_border(cui_ctx *ui) {
    return fl_dark(ui) ? CUI_RGBA(0xFFFFFF17) : CUI_RGBA(0x00000014);
}
cui_color fl_hover(cui_ctx *ui) {          /* state layer, spec §6 */
    return fl_dark(ui) ? CUI_RGBA(0xFFFFFF0F) : CUI_RGBA(0x0000000A);
}
cui_color fl_press(cui_ctx *ui) {
    return fl_dark(ui) ? CUI_RGBA(0xFFFFFF1F) : CUI_RGBA(0x00000014);
}
cui_color fl_text2(cui_ctx *ui) { return cui_token_color(ui, CUI_TOKEN_TEXT_SECONDARY); }
cui_color fl_text3(cui_ctx *ui) { return cui_token_color(ui, CUI_TOKEN_TEXT_MUTED); }
cui_color fl_mask_rgb(cui_ctx *ui) {
    return fl_dark(ui) ? CUI_RGBA(0xFF4D4Dff) : CUI_RGBA(0xE81123ff);
}
uint8_t fl_mask_alpha(cui_ctx *ui) {       /* .38 light / .45 dark, spec §4 */
    return fl_dark(ui) ? 115 : 97;
}

void fl_card_draw(cui_ctx *ui, cui_rect r, float radius) {
    if (fl_dark(ui)) {
        cui_draw_shadow(ui, r, CUI_CORNERS(radius), 32.0f, 0.0f, (cui_vec2){ 0, 8 },
                        CUI_RGBA(0x00000073));
    } else {
        cui_draw_shadow(ui, r, CUI_CORNERS(radius), 24.0f, 0.0f, (cui_vec2){ 0, 8 },
                        CUI_RGBA(0x0000001f));
        cui_draw_shadow(ui, r, CUI_CORNERS(radius), 6.0f, 0.0f, (cui_vec2){ 0, 2 },
                        CUI_RGBA(0x0000000a));
    }
    cui_draw_rect_border(ui, r, CUI_CORNERS(radius), fl_acrylic(ui), fl_border(ui), 1.0f);
}

/* ------------------------------------------------------------------------ */
/* The Fluent palette onto the CUI token roles                               */
/* ------------------------------------------------------------------------ */

void apply_fluent_theme(cui_ctx *ui, app_state *st) {
    cui_theme *t = cui_theme_get(ui);
    bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
    cui_color *c = t->colors;

    c[CUI_TOKEN_BACKGROUND]      = dark ? CUI_RGBA(0x202020ff) : CUI_RGBA(0xF3F3F3ff);
    c[CUI_TOKEN_SURFACE]         = dark ? CUI_RGBA(0x2C2C2Cff) : CUI_RGBA(0xFFFFFFFF);
    c[CUI_TOKEN_SURFACE_RAISED]  = dark ? CUI_RGBA(0x323232ff) : CUI_RGBA(0xFFFFFFFF);
    c[CUI_TOKEN_SURFACE_MUTED]   = dark ? CUI_RGBA(0x181818ff) : CUI_RGBA(0xE8E8E8ff);
    c[CUI_TOKEN_FOREGROUND]      = dark ? CUI_RGBA(0xFFFFFFFF) : CUI_RGBA(0x1A1A1Aff);
    c[CUI_TOKEN_TEXT_SECONDARY]  = dark ? CUI_RGBA(0xCCCCCc7)  : CUI_RGBA(0x5D5D5Dff);
    c[CUI_TOKEN_TEXT_MUTED]      = dark ? CUI_RGBA(0xFFFFFF66) : CUI_RGBA(0x8C8C8Cff);
    c[CUI_TOKEN_BORDER]          = dark ? CUI_RGBA(0xFFFFFF17) : CUI_RGBA(0x00000014);
    c[CUI_TOKEN_BORDER_SUBTLE]   = dark ? CUI_RGBA(0xFFFFFF0F) : CUI_RGBA(0x0000000A);
    c[CUI_TOKEN_BORDER_STRONG]   = dark ? CUI_RGBA(0xFFFFFF2E) : CUI_RGBA(0x00000033);
    c[CUI_TOKEN_ACCENT]          = dark ? CUI_RGBA(0x4CC2FFff) : CUI_RGBA(0x0067C0ff);
    c[CUI_TOKEN_ACCENT_STRONG]   = dark ? CUI_RGBA(0x60CDFFff) : CUI_RGBA(0x1875C9ff);
    c[CUI_TOKEN_ACCENT_SUBTLE]   = dark ? CUI_RGBA(0x4CC2FF26) : CUI_RGBA(0x0067C01A);
    c[CUI_TOKEN_ACCENT_FOREGROUND] = dark ? CUI_RGBA(0x1A1A1Aff) : CUI_RGBA(0xFFFFFFFF);
    c[CUI_TOKEN_SUCCESS]         = dark ? CUI_RGBA(0x6CCB5Fff) : CUI_RGBA(0x107C10ff);
    c[CUI_TOKEN_WARNING]         = dark ? CUI_RGBA(0xF7630Cff) : CUI_RGBA(0xD83B01ff);
    c[CUI_TOKEN_DANGER]          = dark ? CUI_RGBA(0xFF4D4Dff) : CUI_RGBA(0xE81123ff);
    c[CUI_TOKEN_INFO]            = dark ? CUI_RGBA(0x4CC2FFff) : CUI_RGBA(0x0067C0ff);
    c[CUI_TOKEN_HOVER]           = dark ? CUI_RGBA(0xFFFFFF0F) : CUI_RGBA(0x0000000A);
    c[CUI_TOKEN_ACTIVE]          = dark ? CUI_RGBA(0xFFFFFF1F) : CUI_RGBA(0x00000014);
    c[CUI_TOKEN_SCRIM]           = dark ? CUI_RGBA(0x00000080) : CUI_RGBA(0x00000040);
    c[CUI_TOKEN_FOCUS_RING]      = dark ? CUI_RGBA(0x4CC2FF80) : CUI_RGBA(0x0067C080);

    t->radius_control = UI_R_MD;
    t->radius_panel = UI_R_LG;
    t->radius_surface = UI_R_LG;
    t->control_sm = UI_ICONBTN_SM;
    t->control_md = UI_ICONBTN;
    t->control_lg = 36.0f;
    t->text_xs = UI_FS_CAPTION;
    t->text_sm = 12.0f;
    t->text_body = UI_FS_BODY;
    t->text_lg = UI_FS_SUB;

    /* the spec's floating shadow for stock popovers/sheets */
    t->shadow_floating[0] = (cui_shadow){ 8.0f, dark ? 32.0f : 24.0f, 0.0f, dark ? 0.45f : 0.12f };
    t->shadow_floating[1] = (cui_shadow){ 2.0f, 6.0f, 0.0f, dark ? 0.0f : 0.04f };

    /* the mask overlay bakes its theme colour into a texture: rebuild it */
    if (st) {
        for (int i = 0; i < st->count; i++) st->jobs[i].mask_tex_dirty = true;
        st->theme_applied = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT;
    }
}

/* ------------------------------------------------------------------------ */
/* Primitives                                                                */
/* ------------------------------------------------------------------------ */

bool ui_hit(cui_rect r, cui_vec2 m) {
    return r.w > 0.0f && m.x >= r.x && m.x < r.x + r.w && m.y >= r.y && m.y < r.y + r.h;
}

/* Icon button: ghost, with the spec's state layers; `on` tints the accent. */
void ui_iconbtn_draw(cui_ctx *ui, cui_rect r, const char *icon, int state, bool on,
                     float radius) {
    if (on)
        cui_draw_rect(ui, r, CUI_CORNERS(radius), cui_alpha(cui_token_color(ui, CUI_TOKEN_ACCENT), 0.16f));
    if (state == 1) cui_draw_rect(ui, r, CUI_CORNERS(radius), fl_hover(ui));
    else if (state == 2) cui_draw_rect(ui, r, CUI_CORNERS(radius), fl_press(ui));
    cui_color c = on ? cui_token_color(ui, CUI_TOKEN_ACCENT) : fl_text2(ui);
    cui_draw_icon(ui, icon, r, r.h >= UI_ICONBTN ? UI_ICON_MD : UI_ICON_SM, c);
}

/* Text button; accent fills the accent role. The content (icon + label)
 * centres inside r; callers normally pass ui_button_w() so r already hugs. */
void ui_button_draw(cui_ctx *ui, cui_rect r, const char *icon, const char *text,
                    bool accent, bool disabled, int state, float radius) {
    cui_color fg = disabled ? fl_text3(ui)
                 : accent   ? cui_token_color(ui, CUI_TOKEN_ACCENT_FOREGROUND)
                            : cui_token_color(ui, CUI_TOKEN_FOREGROUND);
    cui_color bg = accent ? cui_token_color(ui, CUI_TOKEN_ACCENT) : CUI_TRANSPARENT;
    if (state == 1) bg = accent ? cui_token_color(ui, CUI_TOKEN_ACCENT_STRONG) : fl_hover(ui);
    else if (state == 2) bg = accent ? cui_alpha(cui_token_color(ui, CUI_TOKEN_ACCENT), 0.7f)
                                    : fl_press(ui);
    cui_draw_rect_border(ui, r, CUI_CORNERS(radius), bg,
                         accent || disabled ? CUI_TRANSPARENT : fl_border(ui), 1.0f);
    bool has_icon = icon && *icon, has_text = text && *text;
    cui_text_metrics tm = { 0 };
    if (has_text) tm = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, text, -1);
    float content = (has_icon ? 22.0f : 0.0f) + tm.width;
    float x = r.x + (r.w - content) * 0.5f;
    if (x < r.x + 10.0f) x = r.x + 10.0f;
    float cy = r.y + r.h * 0.5f;
    if (has_icon) {
        cui_draw_icon(ui, icon, (cui_rect){ x, cy - 8.0f, 16, 16 }, 15.0f, fg);
        x += 22.0f;
    }
    if (has_text)
        cui_draw_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY,
                      (cui_vec2){ x, cy - fl_font_h(tm) * 0.5f }, fg, text, -1);
}

float ui_button_w(cui_ctx *ui, const char *icon, const char *text) {
    float w = 20.0f;   /* 10px padding each side, matching the draw clamp */
    if (icon && *icon) w += 22.0f;
    if (text && *text)
        w += cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, text, -1).width;
    return w < 44.0f ? 44.0f : w;
}

float ui_seg_w(cui_ctx *ui, const char *const *items, int count) {
    float label = 0.0f;
    for (int i = 0; i < count; i++) {
        float tw = cui_measure_text(ui, CUI_FONT_MEDIUM, UI_FS_BODY, items[i], -1).width;
        if (tw > label) label = tw;
    }
    float seg = label + UI_S12 * 2.0f;
    if (seg < 48.0f) seg = 48.0f;
    return seg * (float)count;
}

/* Segmented control: card track, raised thumb under the selected segment.
 * The thumb springs between segments (spec §6 asks for stiffness 380 /
 * damping 28; CUI's spring is tuned to read the same at this travel). */
void ui_seg_draw(cui_ctx *ui, cui_rect r, const char *const *items, int count,
                 int index, cui_id anim_id, float radius) {
    cui_draw_rect_border(ui, r, CUI_CORNERS(radius), fl_card_bg(ui), fl_border(ui), 1.0f);
    float iw = r.w / (float)count;
    float thumb_t = cui_animate_spring(ui, anim_id, (float)index, 0.30f, 0.12f);
    if (thumb_t < 0.0f) thumb_t = 0.0f;
    if (thumb_t > (float)(count - 1)) thumb_t = (float)(count - 1);
    cui_rect thumb = { r.x + UI_S4 + (iw - UI_S4) * thumb_t, r.y + UI_S4,
                       iw - UI_S4 * 2.0f, r.h - UI_S4 * 2.0f };
    float thumb_r = radius - UI_S4;
    if (thumb_r > thumb.h * 0.5f) thumb_r = thumb.h * 0.5f;
    if (thumb_r < UI_R_SM) thumb_r = UI_R_SM;
    cui_draw_rect_border(ui, thumb, CUI_CORNERS(thumb_r),
                         cui_token_color(ui, CUI_TOKEN_SURFACE_RAISED), fl_border(ui), 1.0f);
    for (int i = 0; i < count; i++) {
        cui_font f = i == index ? CUI_FONT_MEDIUM : CUI_FONT_BODY;
        cui_text_metrics tm = cui_measure_text(ui, f, UI_FS_BODY, items[i], -1);
        cui_draw_text(ui, f, UI_FS_BODY,
                      (cui_vec2){ r.x + iw * i + (iw - tm.width) * 0.5f,
                                  r.y + (r.h - fl_font_h(tm)) * 0.5f },
                      i == index ? cui_token_color(ui, CUI_TOKEN_FOREGROUND) : fl_text2(ui),
                      items[i], -1);
    }
}

int ui_seg_hit(cui_rect r, int count, float mx) {
    if (r.w <= 0.0f || mx < r.x || mx >= r.x + r.w) return -1;
    int i = (int)((mx - r.x) / (r.w / (float)count));
    if (i < 0) i = 0;
    if (i >= count) i = count - 1;
    return i;
}

void ui_slider_draw(cui_ctx *ui, cui_rect r, float value, float lo, float hi,
                    bool active) {
    float t = (value - lo) / (hi - lo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float inset = UI_SLIDER_THUMB * 0.5f;
    float x0 = r.x + inset, x1 = r.x + r.w - inset;
    float cy = r.y + r.h * 0.5f;
    cui_draw_rect(ui, (cui_rect){ x0, cy - UI_SLIDER_TRACK * 0.5f, x1 - x0, UI_SLIDER_TRACK },
                  CUI_CORNERS(UI_SLIDER_TRACK * 0.5f), fl_border(ui));
    float hx = x0 + (x1 - x0) * t;
    cui_draw_rect(ui, (cui_rect){ x0, cy - UI_SLIDER_TRACK * 0.5f, hx - x0, UI_SLIDER_TRACK },
                  CUI_CORNERS(UI_SLIDER_TRACK * 0.5f), cui_token_color(ui, CUI_TOKEN_ACCENT));
    float tr = (active ? UI_SLIDER_THUMB + 2.0f : UI_SLIDER_THUMB) * 0.5f;
    cui_draw_circle(ui, (cui_vec2){ hx, cy }, tr, cui_token_color(ui, CUI_TOKEN_SURFACE_RAISED));
    cui_draw_rect_border(ui, (cui_rect){ hx - tr, cy - tr, tr * 2.0f, tr * 2.0f },
                         CUI_CORNERS(tr), CUI_TRANSPARENT,
                         cui_token_color(ui, CUI_TOKEN_ACCENT), 1.5f);
    if (active) cui_draw_circle(ui, (cui_vec2){ hx, cy }, 3.0f,
                              cui_token_color(ui, CUI_TOKEN_ACCENT));
}

float ui_slider_value(cui_rect r, float mx, float lo, float hi, float step) {
    float inset = UI_SLIDER_THUMB * 0.5f;
    float span = r.w - inset * 2.0f;
    float t = span > 0.0f ? (mx - (r.x + inset)) / span : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float v = lo + (hi - lo) * t;
    if (step > 0.0f) v = lo + floorf((v - lo) / step + 0.5f) * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

void fl_spinner(cui_ctx *ui, cui_vec2 center, float radius, float thick, cui_color c) {
    /* same sweep as CUI's own spinner: a faint track ring and a head arc whose
     * length breathes while it rotates */
    float t = (float)cui_time(ui);
    cui_draw_arc(ui, center, radius, 0, 6.28318530718f, thick, cui_alpha(c, 0.18f));
    float head = t * 5.2f;
    float len = 1.2f + 0.9f * sinf(t * 2.4f);
    cui_draw_arc(ui, center, radius, head, head + len, thick, c);
    cui_request_redraw(ui);
}
