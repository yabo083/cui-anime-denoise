/* Entry point: process setup, the window, the event loop. The UI itself is in
 * app.c so it can be driven headless by the benchmark harness. */

#define _CRT_SECURE_NO_WARNINGS 1

#include "app.h"
#include "ui_tokens.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#ifndef APP_VERSION
#define APP_VERSION "1.1.0"
#endif

static void parse_command_line(app_state *st) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        char u8[PATH_CAP * 4];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, u8, (int)sizeof u8, NULL, NULL);
        if (!img_supported_path(u8)) continue;
        st->pending = (char **)realloc(st->pending, sizeof(char *) * (size_t)(st->pending_n + 1));
        if (st->pending) st->pending[st->pending_n++] = _strdup(u8);
    }
    if (argv) LocalFree(argv);
}

int main(void) {
    if (!cfg_init()) {
        MessageBoxW(NULL, L"无法创建数据目录（%LOCALAPPDATA%\\AnimeDenoise）。", L"Anime Denoise", MB_ICONERROR);
        return 1;
    }
    log_install_crash_handler();
    if (!img_init()) log_warn("image subsystem init failed");

    /* one instance per user session: a second launch focuses the first window */
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\AnimeDenoise.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(NULL, L"Anime Denoise");
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    static app_state st;
    app_init(&st);
    parse_command_line(&st);

    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = APP_TITLE,
        .width = st.set.window_w > 640 ? st.set.window_w : 1280,
        .height = st.set.window_h > 480 ? st.set.window_h : 860,
        .min_width = (int)UI_MIN_W,     /* spec §1: 900 x 560 */
        .min_height = (int)UI_MIN_H,
        .accent = CUI_ACCENT_BLUE,      /* Fluent blue; tokens retune it to #0067C0/#4CC2FF */
        .follow_system_theme = st.set.theme_mode == 0,
        .theme = st.set.theme_mode == 2 ? CUI_THEME_DARK : CUI_THEME_LIGHT,
    });
    if (!app) {
        log_error("could not create the window");
        return 1;
    }
    chrome_attach(cui_app_native_window(app), UI_TITLEBAR);
    chrome_set_dark(cui_get_theme_mode(cui_app_ui(app)) == CUI_THEME_DARK);

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        app_frame(app, ui, &st);
        if (app_quit_requested(&st)) break;
    }

    app_shutdown(cui_app_ui(app), &st);
    cui_app_destroy(app);
    img_shutdown();
    log_close();
    if (mutex) CloseHandle(mutex);
    return 0;
}
