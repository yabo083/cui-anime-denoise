/* ui_fluent.h -- the Fluent "kit": the spec colours with no CUI token role
 * plus the custom-drawn control primitives shared by the floating layers
 * (ui_float.c) and the title bar (ui_shell.c). Everything resolves through
 * the live theme, so the same draw call is correct in light and dark. */
#ifndef UI_FLUENT_H
#define UI_FLUENT_H

#include <cui/cui.h>
#include <stdbool.h>
#include <stdint.h>

/* --- spec §4 colours without a token role ------------------------------- */
bool fl_dark(cui_ctx *ui);
cui_color fl_acrylic(cui_ctx *ui);    /* floating surfaces */
cui_color fl_card_bg(cui_ctx *ui);    /* secondary panels, seg tracks */
cui_color fl_border(cui_ctx *ui);     /* 1px layer border */
cui_color fl_hover(cui_ctx *ui);      /* state layer, spec §6 */
cui_color fl_press(cui_ctx *ui);
cui_color fl_text2(cui_ctx *ui);
cui_color fl_text3(cui_ctx *ui);
cui_color fl_mask_rgb(cui_ctx *ui);   /* MASK_RED, spec §4 */
uint8_t fl_mask_alpha(cui_ctx *ui);   /* overlay alpha scale (of 255) */

/* Floating card: acrylic fill + 1px border + the spec's floating shadow. */
void fl_card_draw(cui_ctx *ui, cui_rect r, float radius);

/* --- custom-drawn primitives -------------------------------------------- */
/* The floaters cannot be stock CUI widgets (no absolute-position layer), so
 * each is a hand-drawn rect published to app_state for next-frame hit tests. */

bool ui_hit(cui_rect r, cui_vec2 m);
/* Frame-edge press of the left button for the custom controls. One per file
 * that uses it; ui_float and ui_shell keep separate statics. */

/* Visible glyph height of a measured line: the font box (ascent + |descent|),
 * NOT metrics.height — that is the 1.45x paragraph line height meant for text
 * flow; centring on it parks every label ~0.2em above the control's middle.
 * CUI centres its own components on this same box (cui__font_line_height). */
static inline float fl_font_h(cui_text_metrics m) { return m.ascent - m.descent; }

/* state: 0 idle, 1 hot, 2 pressed. `on` draws the accent-tinted toggle state. */
void ui_iconbtn_draw(cui_ctx *ui, cui_rect r, const char *icon, int state, bool on);
void ui_button_draw(cui_ctx *ui, cui_rect r, const char *icon, const char *text,
                    bool accent, bool disabled, int state);
/* Natural widths — the floaters pass these so controls hug their label and
 * container padding stays symmetric; never hand-sum magic widths. The draw
 * calls still centre their content inside whatever rect they get, so a wider
 * rect looks right too. */
float ui_button_w(cui_ctx *ui, const char *icon, const char *text);
/* Equal segments sized to the widest label, measured in MEDIUM — the weight
 * the selected item draws in — with a 12px inset each side and a 48px
 * minimum per segment so the hit area stays usable. */
float ui_seg_w(cui_ctx *ui, const char *const *items, int count);
/* Segmented control: card track + raised thumb; the thumb springs between
 * segments through cui_animate_spring. Hit with ui_seg_hit(). */
void ui_seg_draw(cui_ctx *ui, cui_rect r, const char *const *items, int count,
                 int index, cui_id anim_id);
int ui_seg_hit(cui_rect r, int count, float mx);
/* Slim slider (4px track, 16px thumb, accent fill and grab dot). */
void ui_slider_draw(cui_ctx *ui, cui_rect r, float value, float lo, float hi,
                    bool active);
float ui_slider_value(cui_rect r, float mx, float lo, float hi, float step);
/* Indeterminate ring for the hand-drawn layers (stock cui_spinner only lives
 * inside a layout). Same shape as CUI's spinner, and it asks for the next
 * frame itself so it actually rotates under the event-driven frame pacing. */
void fl_spinner(cui_ctx *ui, cui_vec2 center, float radius, float thick, cui_color c);

#endif /* UI_FLUENT_H */
