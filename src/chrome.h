/* Custom window chrome.
 *
 * The app draws its own title bar, so the native caption is removed: the client
 * area covers the whole window and the top strip behaves like a caption
 * (drag, double-click to maximize, resize handles on the edges).
 *
 * Implemented by subclassing the window procedure CUI installs: WM_NCCALCSIZE
 * reports an empty non-client area (the work area when maximized), WM_NCHITTEST
 * claims the top strip and the resize borders in screen coordinates,
 * WM_SETCURSOR picks the cursor straight from that hit code (CUI would force
 * its own arrow), WM_MOUSEWHEEL hands notches to the UI instead of the sidebar,
 * and everything else is forwarded to the original procedure.
 */
#ifndef CHROME_H
#define CHROME_H

#include <stdbool.h>

/* `title_height` is in logical points: it is scaled by the window DPI here, so
 * the C declaration and the WM_NCHITTEST math agree at every scale factor. */
void chrome_attach(void *hwnd, float title_height_points);

/* Window commands, for the buttons the app draws. */
void chrome_minimize(void);
void chrome_toggle_maximize(void);
void chrome_close(void);
bool chrome_is_maximized(void);

/* Width the app must reserve at the right end of its title bar for the
 * min/max/close buttons, in logical points. Mirrors UI_CAPTION_BUTTONS. */
float chrome_buttons_width(void);

/* True when (x, y) is inside the right-hand title-bar strip that holds the
 * app's own buttons. Coordinates are logical points inside the client area --
 * the same space as cui_mouse_pos() -- so the UI can suppress its own hover
 * feedback where CUI already owns the pointer. */
bool chrome_button_at(int x, int y);

/* Dark caption for the few pixels DWM still draws (maximized border). */
void chrome_set_dark(bool dark);

/* True while the pointer is over the window (for hover styling). */
bool chrome_is_active(void);

/* Accumulated WM_MOUSEWHEEL notches since the last call; positive = up.
 * Returns and clears. The UI reads it once per frame, so a fast scroll is one
 * number rather than a burst of events: the whole wheel input goes to the app
 * (canvas zoom) and never to CUI, which would scroll the sidebar instead. */
float chrome_take_wheel(void);

/* Publishes the clickable rects inside the title band: x,y,w,h quads in
 * logical points, client space (the same space as cui_mouse_pos). The title
 * bar is HTCAPTION by default so it drags the window; inside a published zone
 * the hit test reports HTCLIENT and the app's own controls (filename link,
 * device pill, gear, help, caption buttons) receive their clicks. Replaces the
 * previous set; call again every frame the title bar paints. */
void chrome_set_zones(const float *xywh, int count);

#endif /* CHROME_H */
