/* Tests for src/config.c.
 *
 * Covers: default/save/load round trip, tolerant JSON parsing, atomic save
 * failure (read-only target), log rotation, the crash handler (child
 * process), and the diagnostics zip (validated with Python's zipfile).
 *
 * Build and run commands: tests/README.md.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>

#include "../src/config.h"

static int g_fail;
static int g_checks;
static wchar_t g_test_root[CFG_PATH_CAP];

static void remove_tree(const wchar_t *path)
{
    wchar_t pattern[CFG_PATH_CAP + 8];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    StringCchPrintfW(pattern, CFG_PATH_CAP + 8, L"%s\\*", path);
    h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t full[CFG_PATH_CAP + 64];
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            StringCchPrintfW(full, CFG_PATH_CAP + 64, L"%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(full);
            } else {
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

/* Hermetic run: keep everything under <exe dir>\cfgtest so a parallel app
 * instance or test never shares settings/logs with this run. */
static void setup_test_root(void)
{
    wchar_t exe[CFG_PATH_CAP];
    wchar_t *slash;

    GetModuleFileNameW(NULL, exe, CFG_PATH_CAP);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        *slash = 0;
    }
    StringCchPrintfW(g_test_root, CFG_PATH_CAP, L"%s\\cfgtest", exe);
    remove_tree(g_test_root);
    SetEnvironmentVariableW(L"ANIME_DENOISE_DATA_DIR", g_test_root);
}

static void check(bool cond, const char *what)
{
    g_checks++;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        g_fail++;
    }
}

static void u8_to_w(const char *s, wchar_t *out, size_t cap)
{
    if (!s || !MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap)) {
        if (cap) {
            out[0] = 0;
        }
    }
}

static bool file_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

static unsigned long long file_size(const wchar_t *p)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fad)) {
        return 0;
    }
    return ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static bool write_bytes(const wchar_t *p, const void *data, size_t len)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = true;
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (len) {
        DWORD written = 0;
        if (!WriteFile(h, data, (DWORD)len, &written, NULL) || written != len) {
            ok = false;
        }
    }
    CloseHandle(h);
    return ok;
}

static size_t read_bytes(const wchar_t *p, char *buf, size_t cap)
{
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) {
        if (cap) {
            buf[0] = 0;
        }
        return 0;
    }
    if (!ReadFile(h, buf, (DWORD)(cap - 1), &got, NULL)) {
        got = 0;
    }
    CloseHandle(h);
    buf[got] = 0;
    return got;
}

static void logs_path(const wchar_t *leaf, wchar_t *out, size_t cap)
{
    wchar_t logs[CFG_PATH_CAP];
    u8_to_w(cfg_log_dir(), logs, CFG_PATH_CAP);
    StringCchPrintfW(out, cap, L"%s\\%s", logs, leaf);
}

static void settings_path(wchar_t *out, size_t cap)
{
    wchar_t data[CFG_PATH_CAP];
    u8_to_w(cfg_data_dir(), data, CFG_PATH_CAP);
    StringCchPrintfW(out, cap, L"%s\\settings.json", data);
}

static bool settings_equal(const app_settings *a, const app_settings *b)
{
    float dl0 = a->l0 - b->l0;
    float dbs = a->brush_size - b->brush_size;
    float dbm = a->brush_mix - b->brush_mix;
    float dfp = a->feather_px - b->feather_px;

    if (dl0 < 0) dl0 = -dl0;
    if (dbs < 0) dbs = -dbs;
    if (dbm < 0) dbm = -dbm;
    if (dfp < 0) dfp = -dfp;

    return a->preset == b->preset &&
           a->down_pct == b->down_pct &&
           dl0 < 1e-6f && dbs < 1e-5f && dbm < 1e-5f && dfp < 1e-5f &&
           a->theme_mode == b->theme_mode &&
           a->view_mode == b->view_mode &&
           a->auto_check_updates == b->auto_check_updates &&
           a->seen_welcome == b->seen_welcome &&
           a->window_w == b->window_w &&
           a->window_h == b->window_h &&
           a->window_maximized == b->window_maximized &&
           strcmp(a->output_dir, b->output_dir) == 0 &&
           strcmp(a->model_root, b->model_root) == 0 &&
           strcmp(a->last_open_dir, b->last_open_dir) == 0;
}

/* --------------------------------------------------------------- tests */

static void test_paths(void)
{
    wchar_t p[CFG_PATH_CAP];
    wchar_t data[CFG_PATH_CAP];
    const char *dirs[] = { "logs", "projects", "runtime", "updates" };
    size_t i;

    check(cfg_init(), "cfg_init succeeds");
    check(cfg_data_dir()[0] != 0, "cfg_data_dir is set");
    check(cfg_log_dir()[0] != 0, "cfg_log_dir is set");
    check(cfg_runtime_dir()[0] != 0, "cfg_runtime_dir is set");
    check(cfg_exe_dir()[0] != 0, "cfg_exe_dir is set");
    check(cfg_default_out_dir()[0] != 0, "cfg_default_out_dir is set");

    u8_to_w(cfg_data_dir(), data, CFG_PATH_CAP);
    for (i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        wchar_t sub[CFG_PATH_CAP];
        StringCchPrintfW(sub, CFG_PATH_CAP, L"%s\\%S", data, dirs[i]);
        check(file_exists(sub), dirs[i]);
    }
    u8_to_w(cfg_default_out_dir(), p, CFG_PATH_CAP);
    check(file_exists(p), "default output dir exists");
}

static void test_roundtrip(void)
{
    app_settings a;
    app_settings b;
    app_settings c;

    cfg_defaults(&a);
    check(a.preset == 1 && a.down_pct == 100 && a.auto_check_updates == 1,
          "defaults are sane");
    check(cfg_save(&a), "save defaults");

    memset(&b, 0xCD, sizeof b);
    check(cfg_load(&b), "load after saving defaults returns true");
    check(settings_equal(&a, &b), "load returns the defaults that were saved");

    a.preset = 3;
    a.down_pct = 42;
    a.l0 = 0.001953125f;
    a.brush_size = 33.25f;
    a.brush_mix = 87.0f;
    a.feather_px = 64.0f;
    a.theme_mode = 2;
    a.view_mode = 1;
    a.auto_check_updates = 0;
    a.seen_welcome = 1;
    a.window_w = 1600;
    a.window_h = 900;
    a.window_maximized = 1;
    StringCchCopyA(a.output_dir, CFG_PATH_CAP, "D:\\anime\\out");
    StringCchCopyA(a.model_root, CFG_PATH_CAP, "D:\\models");
    StringCchCopyA(a.last_open_dir, CFG_PATH_CAP, "D:\\anime\\in");

    check(cfg_save(&a), "save modified settings");
    memset(&c, 0, sizeof c);
    check(cfg_load(&c), "load modified settings");
    check(settings_equal(&a, &c), "modified settings survive save/load");
}

static const char k_messy_json[] =
    "\xEF\xBB\xBF{\r\n"
    "  // written by an older build\r\n"
    "  \"preset\": \"strong\",\r\n"
    "  \"down_pct\": 5,\r\n"
    "  \"brush_size\": 60.5,\r\n"
    "  \"theme_mode\": 9,\r\n"
    "  \"window_w\": \"wide\",\r\n"
    "  \"l0\": true,\r\n"
    "  \"mystery\": [1, 2, {\"a\": true}],\r\n"
    "  \"output_dir\": \"C:\\\\Users\\\\tester\\\\out\",\r\n"
    "  \"model_root\": 123,\r\n"
    "  \"last_open_dir\": \"C:\\\\work\", /* trailing comment */\r\n"
    "}\r\n";

static void test_tolerant_load(void)
{
    wchar_t path[CFG_PATH_CAP];
    app_settings s;
    app_settings d;

    cfg_defaults(&d);
    settings_path(path, CFG_PATH_CAP);
    check(write_bytes(path, k_messy_json, sizeof k_messy_json - 1),
          "write handcrafted settings.json");
    check(cfg_load(&s), "load messy settings returns true");

    check(s.preset == d.preset, "wrong-typed preset keeps the default");
    check(s.down_pct == 10, "out-of-range down_pct clamps to 10");
    check(s.brush_size > 60.4f && s.brush_size < 60.6f, "valid brush_size is kept");
    check(s.theme_mode == 2, "invalid theme enum clamps to 2");
    check(s.window_w == d.window_w, "wrong-typed window_w keeps the default");
    check(s.l0 == 0.0f, "wrong-typed l0 keeps the default");
    check(strcmp(s.output_dir, "C:\\Users\\tester\\out") == 0,
          "escaped string value is decoded");
    check(s.model_root[0] == 0, "wrong-typed model_root keeps the default");
    check(strcmp(s.last_open_dir, "C:\\work") == 0,
          "value followed by a block comment parses");
    check(s.seen_welcome == d.seen_welcome && s.window_h == d.window_h,
          "missing keys keep the defaults");

    check(write_bytes(path, "{\"preset\": ", 11), "write truncated json");
    check(cfg_load(&s), "load truncated json returns true");
    check(s.preset == d.preset, "truncated json falls back to defaults");
}

static void test_readonly_save(void)
{
    app_settings a;
    app_settings b;
    wchar_t path[CFG_PATH_CAP];
    DWORD attr;

    cfg_defaults(&a);
    a.down_pct = 42;
    check(cfg_save(&a), "save before read-only test");

    settings_path(path, CFG_PATH_CAP);
    attr = GetFileAttributesW(path);
    check(attr != INVALID_FILE_ATTRIBUTES, "settings.json exists");
    check(SetFileAttributesW(path, attr | FILE_ATTRIBUTE_READONLY) != 0,
          "make settings.json read-only");

    a.down_pct = 77;
    check(!cfg_save(&a), "save fails while settings.json is read-only");

    memset(&b, 0, sizeof b);
    check(cfg_load(&b), "load still works");
    check(b.down_pct == 42, "failed save left the old file intact");

    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
}

static void test_log_rotation(void)
{
    const size_t msg_len = 2600;
    const int lines_per_cycle = 1650;
    const unsigned long long limit = (4ull * 1024ull * 1024ull) + (64ull * 1024ull);
    char *msg = (char *)malloc(msg_len + 1);
    wchar_t app_log[CFG_PATH_CAP];
    wchar_t rotated[CFG_PATH_CAP];
    int cycle;
    int i;

    check(msg != NULL, "allocate log payload");
    if (!msg) {
        return;
    }
    memset(msg, 'x', msg_len);
    msg[msg_len] = 0;

    /* start from a clean set of log files */
    logs_path(L"app.log", app_log, CFG_PATH_CAP);
    log_close();
    DeleteFileW(app_log);
    for (i = 1; i <= 3; ++i) {
        wchar_t name[64];
        StringCchPrintfW(name, 64, L"app.log.%d", i);
        logs_path(name, rotated, CFG_PATH_CAP);
        DeleteFileW(rotated);
    }

    for (cycle = 0; cycle < 4; ++cycle) {
        log_open();
        for (i = 0; i < lines_per_cycle; ++i) {
            log_info("rotation test %d:%d %s", cycle, i, msg);
        }
        log_close();
    }
    log_open();

    for (i = 1; i <= 3; ++i) {
        wchar_t name[64];
        StringCchPrintfW(name, 64, L"app.log.%d", i);
        logs_path(name, rotated, CFG_PATH_CAP);
        check(file_exists(rotated), "rotated log segment exists");
        check(file_size(rotated) <= limit, "rotated log segment is within bounds");
    }
    check(file_exists(app_log), "fresh app.log exists");
    check(file_size(app_log) <= limit, "fresh app.log is within bounds");

    free(msg);
}

static DWORD spawn_crash_child(void)
{
    wchar_t exe[CFG_PATH_CAP];
    wchar_t cmd[CFG_PATH_CAP + 64];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    if (!GetModuleFileNameW(NULL, exe, CFG_PATH_CAP)) {
        return 0;
    }
    StringCchPrintfW(cmd, CFG_PATH_CAP + 64, L"\"%s\" --crash-child", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static bool find_new_crash(FILETIME since, wchar_t *out, size_t cap)
{
    wchar_t logs[CFG_PATH_CAP];
    wchar_t pattern[CFG_PATH_CAP + 32];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    bool found = false;

    u8_to_w(cfg_log_dir(), logs, CFG_PATH_CAP);
    StringCchPrintfW(pattern, CFG_PATH_CAP + 32, L"%s\\crash-*.txt", logs);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &since) >= 0) {
            StringCchPrintfW(out, cap, L"%s\\%s", logs, fd.cFileName);
            found = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void test_crash_handler(void)
{
    FILETIME start;
    wchar_t crash[CFG_PATH_CAP + 64];
    static char text[64 * 1024];
    DWORD code;

    GetSystemTimeAsFileTime(&start);
    code = spawn_crash_child();
    check(code == 0xC0000005u, "crash child exits with the access violation code");

    if (find_new_crash(start, crash, CFG_PATH_CAP + 64)) {
        check(true, "crash report file was created");
        read_bytes(crash, text, sizeof text);
        check(strstr(text, "0xC0000005") != NULL, "crash report names the exception code");
        check(strstr(text, "backtrace") != NULL, "crash report has a backtrace");
        check(strstr(text, "app.log") != NULL, "crash report includes the log tail");
        DeleteFileW(crash);
    } else {
        check(false, "crash report file was created");
    }
}

static bool contains_bytes(const char *hay, size_t hlen, const char *needle, size_t nlen)
{
    size_t i;
    if (nlen == 0 || nlen > hlen) {
        return false;
    }
    for (i = 0; i + nlen <= hlen; ++i) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

static bool validate_zip_python(const char *path)
{
    char cmd[4096];
    int rc;

    snprintf(cmd, sizeof cmd,
             "python -c \"import sys,zipfile;z=zipfile.ZipFile(sys.argv[1]);"
             "n=z.namelist();"
             "assert 'settings.json' in n, n;"
             "assert 'system-info.txt' in n, n;"
             "z.testzip()\" \"%s\"", path);
    rc = system(cmd);
    return rc == 0;
}

static void test_diagnostics(void)
{
    static const char mask[] = "<\xE7\x94\xA8\xE6\x88\xB7\xE7\x9B\xAE\xE5\xBD\x95>";
    app_settings a;
    char zip_path[CFG_PATH_CAP];
    wchar_t wzip[CFG_PATH_CAP];
    wchar_t profile_w[CFG_PATH_CAP];
    char profile_u8[CFG_PATH_CAP];
    unsigned long long size;
    char *bytes;
    size_t got;
    DWORD n;

    profile_w[0] = 0;
    profile_u8[0] = 0;
    n = GetEnvironmentVariableW(L"USERPROFILE", profile_w, CFG_PATH_CAP);
    if (n > 0 && n < CFG_PATH_CAP) {
        WideCharToMultiByte(CP_UTF8, 0, profile_w, -1, profile_u8,
                            CFG_PATH_CAP, NULL, NULL);
    }

    cfg_defaults(&a);
    if (profile_u8[0]) {
        snprintf(a.output_dir, CFG_PATH_CAP, "%s\\out", profile_u8);
    }
    check(cfg_save(&a), "save settings before diagnostics");

    check(log_export_diagnostics(zip_path, sizeof zip_path),
          "log_export_diagnostics succeeds");
    check(zip_path[0] != 0, "diagnostics path is returned");

    u8_to_w(zip_path, wzip, CFG_PATH_CAP);
    check(file_exists(wzip), "diagnostics zip exists on disk");

    check(validate_zip_python(zip_path),
          "python zipfile can open the zip with settings.json");

    /* store-only zip: text is raw in the file, so redaction is observable */
    size = file_size(wzip);
    bytes = (char *)malloc((size_t)size + 1);
    if (bytes) {
        got = read_bytes(wzip, bytes, (size_t)size + 1);
        if (profile_u8[0]) {
            check(!contains_bytes(bytes, got, profile_u8, strlen(profile_u8)),
                  "user profile path is redacted in the zip");
            check(contains_bytes(bytes, got, mask, sizeof mask - 1),
                  "redaction marker is present in the zip");
        }
        free(bytes);
    }
    DeleteFileW(wzip);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--crash-child") == 0) {
        if (!cfg_init()) {
            return 2;
        }
        log_install_crash_handler();
        log_info("test: crash child about to dereference NULL");
        {
            volatile int *p = (volatile int *)0;
            *p = 1;
        }
        return 0;
    }

    printf("config tests\n");
    setup_test_root();
    test_paths();
    test_roundtrip();
    test_tolerant_load();
    test_readonly_save();
    test_log_rotation();
    test_crash_handler();
    test_diagnostics();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_fail);
    cfg_shutdown();
    remove_tree(g_test_root);
    return g_fail == 0 ? 0 : 1;
}
