/* AI runtime bootstrap: uv -> python 3.12 -> torch -> worker deps.
 * See runtime.h for the directory layout and the why.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "runtime.h"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

#define UV_VERSION "0.13.7"

static const char UV_URL_REAL[] =
    "https://github.com/astral-sh/uv/releases/download/" UV_VERSION
    "/uv-x86_64-pc-windows-msvc.zip";

#define PATH_CAP 1024
#define PY_VERSION "3.12"

/* ------------------------------------------------------------------ state */

typedef struct rt_state_s {
    CRITICAL_SECTION lock;
    rt_info info;
    bool inited;
    volatile LONG cancel;
    volatile LONG busy;
    HANDLE thread;
    char root[PATH_CAP];
    char python[PATH_CAP];
    char bin[PATH_CAP];
    char uv[PATH_CAP];
} rt_state_s;

static rt_state_s g;

/* ------------------------------------------------------------------ utils */

static void u8_to_w(const char *s, wchar_t *out, int cap) {
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) <= 0) out[0] = 0;
}

static void w_to_u8(const wchar_t *w, char *out, int cap) {
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) <= 0) out[0] = 0;
}

static bool file_exists(const char *u8) {
    wchar_t w[PATH_CAP * 2];
    u8_to_w(u8, w, PATH_CAP * 2);
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

static void path_join(char *out, size_t cap, const char *dir, const char *leaf) {
    size_t n = dir ? strlen(dir) : 0;
    if (n && (dir[n - 1] == '\\' || dir[n - 1] == '/')) snprintf(out, cap, "%s%s", dir, leaf);
    else snprintf(out, cap, "%s\\%s", dir, leaf);
}

static void mkdirs(const char *u8) {
    wchar_t w[PATH_CAP * 2];
    u8_to_w(u8, w, PATH_CAP * 2);
    CreateDirectoryW(w, NULL);
}

static void set_stage(const char *text) {
    EnterCriticalSection(&g.lock);
    snprintf(g.info.stage, sizeof g.info.stage, "%s", text ? text : "");
    LeaveCriticalSection(&g.lock);
    log_info("runtime: %s", text ? text : "");
}

static void set_progress(float v) {
    EnterCriticalSection(&g.lock);
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g.info.progress = v;
    LeaveCriticalSection(&g.lock);
}

static void set_error(const char *msg) {
    EnterCriticalSection(&g.lock);
    g.info.state = RT_FAILED;
    snprintf(g.info.error, sizeof g.info.error, "%s", msg ? msg : "installation failed");
    LeaveCriticalSection(&g.lock);
    log_error("runtime: %s", msg ? msg : "installation failed");
}

static void append_log(const char *line) {
    EnterCriticalSection(&g.lock);
    size_t used = strlen(g.info.log_tail);
    size_t add = strlen(line);
    if (used + add + 2 >= sizeof g.info.log_tail) {
        /* keep the tail: drop the oldest half */
        memmove(g.info.log_tail, g.info.log_tail + used / 2, used - used / 2 + 1);
        used = strlen(g.info.log_tail);
    }
    snprintf(g.info.log_tail + used, sizeof g.info.log_tail - used, "%s\n", line);
    LeaveCriticalSection(&g.lock);
}

/* ------------------------------------------------------------- HTTP GET */

static bool http_get_file(const char *url, const char *dst, float weight_lo, float weight_hi) {
    wchar_t wurl[2048];
    u8_to_w(url, wurl, 2048);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) return false;

    wchar_t host[256] = {0};
    wcsncpy_s(host, 256, uc.lpszHostName, uc.dwHostNameLength);
    wchar_t path[1536] = {0};
    wcsncpy_s(path, 1536, uc.lpszUrlPath, uc.dwUrlPathLength);

    HINTERNET session = WinHttpOpen(L"AnimeDenoise/" L"" APP_VERSION,
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 20000, 20000, 30000, 60000);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { WinHttpCloseHandle(session); return false; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }

    /* follow up to 5 redirects (GitHub release assets redirect to a CDN) */
    for (int hop = 0; hop < 5; hop++) {
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, NULL)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
            return false;
        }
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            wchar_t loc[2048] = {0};
            DWORD loc_len = sizeof loc;
            if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                     loc, &loc_len, WINHTTP_NO_HEADER_INDEX))
                break;
            WinHttpCloseHandle(req);
            URL_COMPONENTS lc;
            memset(&lc, 0, sizeof lc);
            lc.dwStructSize = sizeof lc;
            lc.dwHostNameLength = (DWORD)-1;
            lc.dwUrlPathLength = (DWORD)-1;
            if (!WinHttpCrackUrl(loc, 0, 0, &lc)) break;
            wcsncpy_s(host, 256, lc.lpszHostName, lc.dwHostNameLength);
            wcsncpy_s(path, 1536, lc.lpszUrlPath, lc.dwUrlPathLength);
            WinHttpCloseHandle(conn);
            conn = WinHttpConnect(session, host, lc.nPort, 0);
            if (!conn) { WinHttpCloseHandle(session); return false; }
            req = WinHttpOpenRequest(conn, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     lc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }
            continue;
        }
        if (status < 200 || status >= 300) break;

        FILE *f = fopen(dst, "wb");
        if (!f) break;

        unsigned long long total = 0, got = 0;
        wchar_t clen[64] = {0};
        DWORD clen_sz = sizeof clen;
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                clen, &clen_sz, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(clen, NULL, 10);

        unsigned char buf[65536];
        bool ok = true;
        for (;;) {
            if (InterlockedCompareExchange(&g.cancel, 0, 0)) { ok = false; break; }
            DWORD got_now = 0;
            if (!WinHttpReadData(req, buf, sizeof buf, &got_now)) { ok = false; break; }
            if (got_now == 0) break;
            if (fwrite(buf, 1, got_now, f) != got_now) { ok = false; break; }
            got += got_now;
            if (total)
                set_progress(weight_lo + (weight_hi - weight_lo) * (float)((double)got / (double)total));
        }
        fclose(f);
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return ok;
    }

    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
}

/* --------------------------------------------------------- process runner */

/* Runs a command line, streaming its combined output line by line into
 * log_tail. Returns the exit code, or -1 when it could not start. */
static int run_stream(const wchar_t *cmdline, const char *cwd_u8) {
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = NULL;
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);

    wchar_t cwd[PATH_CAP * 2];
    u8_to_w(cwd_u8, cwd, PATH_CAP * 2);

    wchar_t *mutable_cmd = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, mutable_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, cwd[0] ? cwd : NULL, &si, &pi);
    free(mutable_cmd);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }

    char line[2048];
    size_t used = 0;
    for (;;) {
        if (InterlockedCompareExchange(&g.cancel, 0, 0)) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) {
            DWORD wait = WaitForSingleObject(pi.hProcess, 60);
            if (wait == WAIT_OBJECT_0) {
                /* drain what is left */
                DWORD got = 0;
                while (ReadFile(rd, line + used, (DWORD)(sizeof line - used - 1), &got, NULL) && got > 0) {
                    used += got;
                    char *nl;
                    while ((nl = memchr(line, '\n', used)) != NULL) {
                        *nl = 0;
                        size_t n = (size_t)(nl - line);
                        if (n && line[n - 1] == '\r') line[n - 1] = 0;
                        append_log(line);
                        memmove(line, nl + 1, used - n - 1);
                        used -= n + 1;
                    }
                }
                break;
            }
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(rd, line + used, (DWORD)(sizeof line - used - 1), &got, NULL) || got == 0) break;
        used += got;
        char *nl;
        while ((nl = memchr(line, '\n', used)) != NULL) {
            *nl = 0;
            size_t n = (size_t)(nl - line);
            if (n && line[n - 1] == '\r') line[n - 1] = 0;
            if (line[0]) append_log(line);
            memmove(line, nl + 1, used - n - 1);
            used -= n + 1;
        }
        if (used > sizeof line - 2) used = 0;   /* a single huge line: drop it */
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* -------------------------------------------------------------- probing */

static bool gpu_present(char *name, size_t cap) {
    /* nvidia-smi ships with the driver; its absence just means CPU */
    wchar_t cmd[] = L"cmd /c nvidia-smi --query-gpu=name --format=csv,noheader";
    FILE *p = _wpopen(cmd, L"rt");
    if (!p) return false;
    char buf[256] = {0};
    bool got = false;
    if (fgets(buf, sizeof buf, p)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        if (n > 4) { snprintf(name, cap, "%s", buf); got = true; }
    }
    _pclose(p);
    return got;
}

void rt_init(void) {
    if (g.inited) return;
    InitializeCriticalSection(&g.lock);
    g.inited = true;

    snprintf(g.root, sizeof g.root, "%s", cfg_runtime_dir());
    mkdirs(g.root);
    path_join(g.bin, sizeof g.bin, g.root, "bin");
    mkdirs(g.bin);
    path_join(g.uv, sizeof g.uv, g.bin, "uv.exe");
    path_join(g.python, sizeof g.python, g.root, "python\\Scripts\\python.exe");

    EnterCriticalSection(&g.lock);
    g.info.state = file_exists(g.python) ? RT_READY : RT_MISSING;
    g.info.progress = 0.0f;
    char gpu[160] = "";
    if (gpu_present(gpu, sizeof gpu)) {
        g.info.cuda = true;
        snprintf(g.info.device_note, sizeof g.info.device_note, "%s", gpu);
    } else {
        g.info.cuda = false;
        snprintf(g.info.device_note, sizeof g.info.device_note, "未检测到 NVIDIA 显卡，将安装 CPU 版");
    }
    LeaveCriticalSection(&g.lock);
}

bool rt_ready(void) {
    if (!g.inited) rt_init();
    return file_exists(g.python);
}

const char *rt_python(void) { return g.python; }
const char *rt_root(void) { return g.root; }

void rt_get(rt_info *out) {
    if (!out) return;
    if (!g.inited) rt_init();
    EnterCriticalSection(&g.lock);
    *out = g.info;
    LeaveCriticalSection(&g.lock);
}

/* -------------------------------------------------------------- install */

static bool step_uv(void) {
    if (file_exists(g.uv)) return true;
    set_stage("下载 uv（约 15 MB）");
    char zip[PATH_CAP];
    path_join(zip, sizeof zip, g.bin, "uv.zip");
    if (!http_get_file(UV_URL_REAL, zip, 0.0f, 0.06f)) {
        set_error("下载 uv 失败，请检查网络后重试");
        return false;
    }
    set_stage("解压 uv");
    wchar_t cmd[PATH_CAP * 3];
    wchar_t wz[PATH_CAP * 2], wb[PATH_CAP * 2];
    u8_to_w(zip, wz, PATH_CAP * 2);
    u8_to_w(g.bin, wb, PATH_CAP * 2);
    swprintf(cmd, PATH_CAP * 3, L"tar.exe -xf \"%s\" -C \"%s\"", wz, wb);
    if (run_stream(cmd, g.bin) != 0 || !file_exists(g.uv)) {
        set_error("解压 uv 失败");
        return false;
    }
    DeleteFileW(wz);
    return true;
}

static bool step_python(void) {
    set_stage("安装 Python " PY_VERSION);
    char pythons[PATH_CAP], cmd[PATH_CAP * 3];
    wchar_t wcmd[PATH_CAP * 3];
    path_join(pythons, sizeof pythons, g.root, "pythons");
    mkdirs(pythons);

    snprintf(cmd, sizeof cmd, "\"%s\" python install " PY_VERSION " --install-dir \"%s\" --no-bin",
             g.uv, pythons);
    u8_to_w(cmd, wcmd, PATH_CAP * 3);
    if (run_stream(wcmd, g.root) != 0) { set_error("安装 Python 失败"); return false; }

    set_stage("创建虚拟环境");
    char venv[PATH_CAP];
    path_join(venv, sizeof venv, g.root, "python");
    snprintf(cmd, sizeof cmd, "\"%s\" venv \"%s\" --python " PY_VERSION, g.uv, venv);
    u8_to_w(cmd, wcmd, PATH_CAP * 3);
    if (run_stream(wcmd, g.root) != 0 || !file_exists(g.python)) {
        set_error("创建虚拟环境失败");
        return false;
    }
    return true;
}

static bool pip_install(const char *args, float lo, float hi) {
    char cmd[4096];
    wchar_t wcmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" pip install --python \"%s\" %s", g.uv, g.python, args);
    u8_to_w(cmd, wcmd, 4096);
    if (!wcmd[0]) return false;
    set_progress(lo);
    int rc = run_stream(wcmd, g.root);
    if (rc == 0) set_progress(hi);
    return rc == 0;
}

static bool step_torch(void) {
    set_stage("安装 PyTorch（这一步最慢，约 2 GB）");
    char args[1024];
    if (g.info.cuda) {
        /* newer builds first; the driver decides what it can load */
        const char *indexes[] = { "cu130", "cu128", "cpu" };
        for (int i = 0; i < 3; i++) {
            snprintf(args, sizeof args,
                     "--index-url https://download.pytorch.org/whl/%s torch torchvision",
                     indexes[i]);
            if (pip_install(args, 0.10f, 0.72f)) {
                if (i == 2) {
                    EnterCriticalSection(&g.lock);
                    g.info.cuda = false;
                    snprintf(g.info.device_note, sizeof g.info.device_note, "只装上了 CPU 版 PyTorch");
                    LeaveCriticalSection(&g.lock);
                }
                return true;
            }
            if (InterlockedCompareExchange(&g.cancel, 0, 0)) return false;
        }
        set_error("PyTorch 安装失败");
        return false;
    }
    snprintf(args, sizeof args, "--index-url https://download.pytorch.org/whl/cpu torch torchvision");
    if (!pip_install(args, 0.10f, 0.72f)) { set_error("PyTorch 安装失败"); return false; }
    return true;
}

static bool step_deps(void) {
    set_stage("安装去噪依赖");
    if (!pip_install("spandrel numpy pillow psutil", 0.74f, 0.98f)) {
        set_error("依赖安装失败");
        return false;
    }
    return true;
}

static DWORD WINAPI install_thread(LPVOID param) {
    (void)param;
    EnterCriticalSection(&g.lock);
    g.info.state = RT_INSTALLING;
    g.info.progress = 0.0f;
    g.info.error[0] = 0;
    LeaveCriticalSection(&g.lock);

    bool ok = step_uv() && step_python() && step_torch() && step_deps();
    if (InterlockedCompareExchange(&g.cancel, 0, 0)) {
        set_error("安装已取消");
        ok = false;
    }

    EnterCriticalSection(&g.lock);
    if (ok) {
        g.info.state = RT_READY;
        g.info.progress = 1.0f;
        snprintf(g.info.stage, sizeof g.info.stage, "运行环境已就绪");
    } else if (g.info.state != RT_FAILED) {
        g.info.state = RT_FAILED;
    }
    LeaveCriticalSection(&g.lock);

    InterlockedExchange(&g.busy, 0);
    log_info("runtime install %s", ok ? "finished" : "failed");
    return 0;
}

void rt_install(void) {
    if (!g.inited) rt_init();
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) return;
    InterlockedExchange(&g.cancel, 0);
    g.thread = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (!g.thread) {
        InterlockedExchange(&g.busy, 0);
        set_error("无法启动安装线程");
    }
}

void rt_cancel(void) {
    InterlockedExchange(&g.cancel, 1);
}

void rt_shutdown(void) {
    if (!g.inited) return;
    InterlockedExchange(&g.cancel, 1);
    if (g.thread) {
        WaitForSingleObject(g.thread, 5000);
        CloseHandle(g.thread);
        g.thread = NULL;
    }
}
