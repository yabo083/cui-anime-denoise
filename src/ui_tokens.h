/* Visual tokens -- the single source for sizes, radii and type scale.
 * Rules: docs/design.md §4. No component may hard-code a radius or a gap. */
#ifndef UI_TOKENS_H
#define UI_TOKENS_H

/* --- geometry (logical points) --- */
#define UI_TOPBAR      56.0f
#define UI_SIDEBAR    300.0f
#define UI_STRIP       92.0f
#define UI_TILE        62.0f   /* thumbnail and "add" tile share this */
#define UI_HEAD        46.0f   /* file name / actions row */
#define UI_PANE_PAD    14.0f
#define UI_PAD         12.0f
#define UI_GAP          8.0f
#define UI_GAP_SM       6.0f

/* --- corner radii --- */
#define UI_R_CONTROL    8.0f
#define UI_R_TILE      12.0f
#define UI_R_CARD      14.0f
#define UI_R_DIALOG    18.0f

/* --- type --- */
#define UI_FS_CAPTION  10.5f
#define UI_FS_BODY     12.0f
#define UI_FS_LABEL    12.5f
#define UI_FS_TITLE    14.0f
#define UI_FS_BRAND    16.0f

/* --- controls --- */
#define UI_CONTROL_H   30.0f
#define UI_BUTTON_H    30.0f
#define UI_ICON_SM     14.0f
#define UI_ICON_MD     16.0f
#define UI_ICON_LG     20.0f
#define UI_SLIDER_H     3.0f
#define UI_SLIDER_THUMB 12.0f

/* --- title bar interaction zones (must match src/chrome.c) --- */
#define UI_CAPTION_BUTTONS 316.0f
#define UI_RESIZE_BAND       8.0f

#endif /* UI_TOKENS_H */
