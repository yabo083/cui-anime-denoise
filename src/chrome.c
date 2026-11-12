#define _CRT_SECURE_NO_WARNINGS 1

#include "chrome.h"
#include "ui_tokens.h"

#include <string.h>

#include <windows.h>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

/* DWM still paints one native pixel around a maximized window. Match it to the
 * CUI surface tone (light: white, dark: oklch 0.21 ~ #181818) so the seam does
 * not read as a border. These are DWM attributes, not UI painting, so they
 * cannot take a CUI theme token. */
#define CHROME_FRAME_LIGHT RGB(0xFF, 0xFF, 0xFF)
#define CHROME_FRAME_DARK  RGB(0x18, 0x18, 0x18)

static HWND g_hwnd;
static WNDPROC g_orig;
static float g_title_points = UI_TITLEBAR;
static int g_dark = -1; /* -1 = DWM attributes not applied yet */
/* last cursor we put up; while the window holds capture (a sizing or moving
 * loop owns the pointer) we keep showing it instead of re-deciding */
static LPCWSTR g_cursor;
/* WM_MOUSEWHEEL notches waiting for the next UI frame; see chrome_take_wheel */
static float g_wheel;
/* Clickable zones inside the title band, x,y,w,h quads in logical points.
 * The band is HTCAPTION by default (it drags the window); these rects are
 * where the app's own title-bar controls live, so they must hit-test HTCLIENT. */
#define CHROME_MAX_ZONES 24
static float g_zones[CHROME_MAX_ZONES * 4];
static int g_zones_n;

static float dpi_scale(HWND h) {
    typedef UINT(WINAPI * get_dpi_fn)(HWND);
    static get_dpi_fn get_dpi;
    static bool looked;
    if (!looked) {
        looked = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) get_dpi = (get_dpi_fn)(void *)GetProcAddress(user32, "GetDpiForWindow");
    }
    UINT dpi = 96;
    if (get_dpi) dpi = get_dpi(h);
    if (dpi == 0) dpi = 96;
    return (float)dpi / 96.0f;
}

/* Hit test in screen coordinates. WM_NCHITTEST hands us screen points, so the
 * frame comes from GetWindowRect and no client/window rectangle is mixed in:
 * every band is measured against the same box the pointer lives in. */
static LRESULT hit_test_screen(HWND h, POINT pt) {
    RECT wr;
    if (!GetWindowRect(h, &wr)) return HTCLIENT;

    const float s = dpi_scale(h);
    const LONG band = (LONG)(UI_RESIZE_BAND * s + 0.5f);
    const LONG title = (LONG)(g_title_points * s + 0.5f);
    const LONG buttons = (LONG)(UI_CAPTION_BUTTONS * s + 0.5f);
    const bool maximized = IsZoomed(h) != 0;

    if (maximized) {
        /* a maximized window rect overhangs the work area by the resize frame,
         * so its outer bands sit off-screen; the edges the user can touch are
         * the work area's, which is also the client area we report */
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) wr = mi.rcWork;
    }

    /* far outside the frame (possible while another window has capture): not a zone */
    if (pt.x < wr.left - band || pt.x >= wr.right + band ||
        pt.y < wr.top - band || pt.y >= wr.bottom + band) return HTCLIENT;

    const bool left = pt.x < wr.left + band;
    const bool right = pt.x >= wr.right - band;
    const bool top = pt.y < wr.top + band;
    const bool bottom = pt.y >= wr.bottom - band;

    if (maximized) {
        /* a maximized window has no resize handles; claiming one would show a
         * resize cursor the app cannot honour (design.md 3.1) */
        if (left || right || top || bottom) return HTCLIENT;
    } else {
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }

    if (pt.y < wr.top + title) {
        /* the app's title-bar controls live in published zones (the filename,
         * the device pill, the gear/help icons) and the window buttons in the
         * last UI_CAPTION_BUTTONS points: those stay HTCLIENT so CUI receives
         * the clicks, everything else drags the window */
        const float lx = (float)(pt.x - wr.left) / s;
        const float ly = (float)(pt.y - wr.top) / s;
        for (int i = 0; i < g_zones_n; i++) {
            const float *z = &g_zones[i * 4];
            if (lx >= z[0] && lx < z[0] + z[2] && ly >= z[1] && ly < z[1] + z[3])
                return HTCLIENT;
        }
        if (pt.x >= wr.right - buttons) return HTCLIENT;
        return HTCAPTION;
    }
    return HTCLIENT;
}

/* Cursor for a hit code. WM_SETCURSOR's LOWORD(lParam) is already the
 * WM_NCHITTEST result for the pointer's live position, so this is the whole
 * decision table: no second lookup, no re-test against a window that may have
 * moved since the system asked. Everything that is not a resize edge -- the
 * caption, the client area, the app's own buttons -- is a plain arrow. */
static LPCWSTR zone_cursor(LRESULT zone) {
    switch (zone) {
    case HTLEFT: case HTRIGHT: return IDC_SIZEWE;
    case HTTOP: case HTBOTTOM: return IDC_SIZENS;
    case HTTOPLEFT: case HTBOTTOMRIGHT: return IDC_SIZENWSE;
    case HTTOPRIGHT: case HTBOTTOMLEFT: return IDC_SIZENESW;
    default: return IDC_ARROW;
    }
}

static LRESULT CALLBACK chrome_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            if (!IsZoomed(h)) {
                /* the client area covers the whole window; the app draws the title bar */
                return 0;
            }
            /* maximized: the window rect overhangs the work area by the resize
             * frame, so report the work area as the client instead of letting
             * the app paint over the taskbar */
            NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
            MONITORINFO mi = { sizeof mi };
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) p->rgrc[0] = mi.rcWork;
            return 0;
        }
        break;

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        return hit_test_screen(h, pt);
    }

    case WM_SETCURSOR: {
        /* LOWORD(lParam) is the hit code the system just got from our
         * WM_NCHITTEST: the only trustworthy answer for where the pointer is in
         * the frame. Re-testing with GetCursorPos here used to disagree with it
         * while the window moved under the pointer, so the cursor flickered
         * between arrow and size arrow.
         *
         * HTCLIENT is left for CUI's own handler: it maps the cursor the
         * widgets asked for (hand over a link, I-beam in a field); answering
         * it here stomped those to the arrow on every move. We only own the
         * non-client codes hit_test_screen invented -- the resize bands and
         * the drag caption (design.md 3.2). */
        WORD hit = LOWORD(lp);
        if (hit == 0) break; /* forwarded by a child; not our frame */
        if (hit == HTCLIENT) break;

        if (GetCapture() != h || !g_cursor) {
            /* a sizing/moving loop owns the pointer while the window holds
             * capture: the window is dragged with the cursor, so the band under
             * it means nothing and re-deciding would flip the arrow mid-gesture
             * (worst at corners, where the code oscillates between two axes).
             * Keep the cursor the gesture started with. */
            g_cursor = zone_cursor(hit);
        }
        SetCursor(LoadCursorW(NULL, g_cursor));
        return TRUE;
    }

    case WM_MOUSEWHEEL: {
        /* keep wheel notches for the UI instead of letting CUI scroll whatever
         * sits under the pointer; the UI takes them once per frame with
         * chrome_take_wheel() and decides what they mean (zoom on the canvas) */
        g_wheel += (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        return 0;
    }

    case WM_GETMINMAXINFO: {
        /* let CUI set the minimum track size first, then clamp maximize to the
         * work area so snap/maximize never tuck the window under the taskbar */
        LRESULT r = CallWindowProcW(g_orig, h, msg, wp, lp);
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        MONITORINFO mi = { sizeof mi };
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
            mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mm->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mm->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return r;
    }

    case WM_NCACTIVATE:
        return TRUE;

    default:
        break;
    }
    return CallWindowProcW(g_orig, h, msg, wp, lp);
}

void chrome_attach(void *hwnd, float title_height_points) {
    if (!hwnd) return;
    g_hwnd = (HWND)hwnd;
    g_title_points = title_height_points > 0.0f ? title_height_points : UI_TITLEBAR;

    SetWindowLongPtrW(g_hwnd, GWL_STYLE,
                      GetWindowLongPtrW(g_hwnd, GWL_STYLE) | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    g_orig = (WNDPROC)(void *)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)chrome_proc);

    /* rounded corners and a 1px frame on Windows 11 */
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);

    SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void chrome_minimize(void) {
    if (g_hwnd) ShowWindow(g_hwnd, SW_MINIMIZE);
}

void chrome_toggle_maximize(void) {
    if (!g_hwnd) return;
    ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void chrome_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

bool chrome_is_maximized(void) { return g_hwnd && IsZoomed(g_hwnd); }

float chrome_buttons_width(void) { return UI_CAPTION_BUTTONS; }

void chrome_set_dark(bool dark) {
    if (!g_hwnd || g_dark == (dark ? 1 : 0)) return;
    g_dark = dark ? 1 : 0;

    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &value, sizeof value);

    /* attribute 35 exists from Windows 11 22000 on; older builds reject it and
     * keep their system caption colour, which is fine */
    COLORREF caption = dark ? CHROME_FRAME_DARK : CHROME_FRAME_LIGHT;
    DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);
}

bool chrome_is_active(void) { return g_hwnd && GetActiveWindow() == g_hwnd; }

float chrome_take_wheel(void) {
    const float v = g_wheel;
    g_wheel = 0.0f;
    return v;
}

void chrome_set_zones(const float *xywh, int count) {
    if (!xywh || count <= 0) { g_zones_n = 0; return; }
    if (count > CHROME_MAX_ZONES) count = CHROME_MAX_ZONES;
    memcpy(g_zones, xywh, (size_t)count * 4 * sizeof(float));
    g_zones_n = count;
}

bool chrome_button_at(int x, int y) {
    if (!g_hwnd) return false;
    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return false;
    const float s = dpi_scale(g_hwnd);
    if (s <= 0.0f) return false;
    /* the UI asks in logical points, so the client width shrinks by the same
     * factor the title bar is drawn at */
    const float w_points = (float)(rc.right - rc.left) / s;
    return (float)x >= w_points - UI_CAPTION_BUTTONS && (float)x < w_points &&
           (float)y >= 0.0f && (float)y < g_title_points;
}
