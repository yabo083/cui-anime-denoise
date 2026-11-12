/* Visual tokens -- the single source for sizes, radii and type scale.
 * Rules: docs/fluent-redesign.md (authoritative) + docs/design.md §4.
 * No component may hard-code a radius, a gap or a control height.
 *
 * Colours do not live here: the Fluent palette is applied to CUI's theme
 * tokens by apply_fluent_theme() (ui_shell.c), so every CUI_* colour macro
 * keeps resolving per-theme. The few spec colours with no token role (mask
 * red, the acrylic surfaces, the split line) get named constants in
 * ui_fluent.h. */
#ifndef UI_TOKENS_H
#define UI_TOKENS_H

/* --- window chrome --- */
#define UI_TITLEBAR     40.0f    /* slim WinUI title bar */
#define UI_CAPTION_W    46.0f    /* one system caption button */
#define UI_CAPTION_BUTTONS (UI_CAPTION_W * 3.0f)  /* chrome.c's HTCLIENT strip */
#define UI_RESIZE_BAND   8.0f
#define UI_MIN_W       900.0f
#define UI_MIN_H       560.0f

/* --- floating layer 1 --- */
#define UI_PILL_H       44.0f    /* main control capsule */
#define UI_PILL_PAD      6.0f    /* inside the capsule */
#define UI_PILL_LIFT    24.0f    /* distance from the window bottom */
#define UI_TRAY_H       68.0f    /* filmstrip tray, 8px above the pill */
#define UI_TRAY_GAP      8.0f
#define UI_TILE         48.0f    /* tray thumbnail */
#define UI_TILE_PAD      4.0f
#define UI_TILE_GAP      6.0f
#define UI_PROTECT_H    44.0f    /* protection toolbar, slides in at the top */
#define UI_TUNE_W      260.0f    /* tune popover */
#define UI_SHEET_W     320.0f    /* settings right sheet */
#define UI_BANNER_H     36.0f    /* engine boot / error banner */

/* --- spacing scale (2/4/8/12/16/24/32) --- */
#define UI_S2    2.0f
#define UI_S4    4.0f
#define UI_S8    8.0f
#define UI_S12  12.0f
#define UI_S16  16.0f
#define UI_S24  24.0f
#define UI_S32  32.0f
#define UI_PAD  12.0f
#define UI_GAP   8.0f

/* --- corner radii --- */
#define UI_R_FULL    999.0f   /* capsule */
#define UI_R_LG       10.0f   /* floating layers */
#define UI_R_MD        6.0f   /* segmented thumb, icon buttons, tiles */
#define UI_R_SM        4.0f   /* slider thumb, tooltips */

/* --- type scale (Noto Sans SC) --- */
#define UI_FS_TITLE   18.0f   /* /24 bold */
#define UI_FS_SUB     14.0f   /* /20 medium */
#define UI_FS_BODY    13.0f   /* /18 */
#define UI_FS_CAPTION 11.0f   /* /14 */

/* --- controls --- */
#define UI_SEG_H      32.0f   /* segmented controls */
#define UI_ICONBTN    32.0f   /* icon buttons */
#define UI_ICONBTN_SM 28.0f   /* compact icon buttons */
#define UI_ICON_MD    16.0f
#define UI_ICON_SM    14.0f
#define UI_SLIDER_TRACK  4.0f
#define UI_SLIDER_THUMB 16.0f

/* --- motion --- */
#define UI_T_FLOAT   0.20f    /* layers in/out, seconds */
#define UI_T_RESULT  0.12f    /* result texture cross-fade */
#define UI_T_MASKFADE 0.7f    /* protection overlay fade after a stroke */

#endif /* UI_TOKENS_H */
