/* The rotating log, the last-chance crash reports and the diagnostics zip.
 *
 * Layout (see config.h):
 *   %LOCALAPPDATA%\AnimeDenoise\logs\app.log(.1 .2 .3)
 *   %LOCALAPPDATA%\AnimeDenoise\logs\crash-YYYYMMDD-HHMMSS.txt
 *   %LOCALAPPDATA%\AnimeDenoise\logs\diagnostics-YYYYMMDD-HHMMSS.zip
 *
 * Settings and paths live in config.c; the two halves of the module share
 * config_priv.h. No third-party code: the CRC32 and the store-only zip writer
 * are local. dbghelp is loaded with LoadLibrary solely for optional symbol
 * names in crash reports, so it is never a link dependency.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <share.h>
#include <wchar.h>

#include "config.h"
#include "config_priv.h"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

/* -------------------------------------------------------------------- log */

static CRITICAL_SECTION g_log_lock;
static bool g_log_lock_ready;
static FILE *g_log_file;
static wchar_t g_log_path_w[CFG_WCHAR_CAP];

static const char *log_level_name(log_level lvl)
{
    switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO ";
    case LOG_WARN:  return "WARN ";
    case LOG_ERROR: return "ERROR";
    case LOG_FATAL: return "FATAL";
    default:        return "INFO ";
    }
}

static void log_rotate_if_needed(void)
{
    wchar_t p0[CFG_WCHAR_CAP];
    wchar_t p1[CFG_WCHAR_CAP];
    wchar_t p2[CFG_WCHAR_CAP];
    wchar_t p3[CFG_WCHAR_CAP];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    unsigned long long size;

    if (!cfg__log_dir_w[0]) {
        return;
    }
    if (FAILED(StringCchPrintfW(p0, CFG_WCHAR_CAP, L"%s\\app.log", cfg__log_dir_w)) ||
        FAILED(StringCchPrintfW(p1, CFG_WCHAR_CAP, L"%s\\app.log.1", cfg__log_dir_w)) ||
        FAILED(StringCchPrintfW(p2, CFG_WCHAR_CAP, L"%s\\app.log.2", cfg__log_dir_w)) ||
        FAILED(StringCchPrintfW(p3, CFG_WCHAR_CAP, L"%s\\app.log.3", cfg__log_dir_w))) {
        return;
    }
    if (!GetFileAttributesExW(p0, GetFileExInfoStandard, &fad)) {
        return;
    }
    size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size <= LOG_ROTATE_BYTES) {
        return;
    }
    DeleteFileW(p3);
    MoveFileExW(p2, p3, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(p1, p2, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(p0, p1, MOVEFILE_REPLACE_EXISTING);
}

/* Called with the log lock held, after the file grew past the threshold. */
static void log_reopen_locked(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    log_rotate_if_needed();
    if (g_log_path_w[0]) {
        g_log_file = _wfsopen(g_log_path_w, L"ab", _SH_DENYNO);
    }
}

void log_open(void)
{
    FILE *f = NULL;

    if (g_log_file) {
        return;
    }
    if (!g_log_lock_ready) {
        InitializeCriticalSection(&g_log_lock);
        g_log_lock_ready = true;
    }
    if (!cfg__log_dir_w[0]) {
        return;
    }
    log_rotate_if_needed();
    if (FAILED(StringCchPrintfW(g_log_path_w, CFG_WCHAR_CAP, L"%s\\app.log", cfg__log_dir_w))) {
        g_log_path_w[0] = 0;
        return;
    }
    /* _SH_DENYNO: the crash handler must be able to read app.log while the
     * process still has it open for append. */
    f = _wfsopen(g_log_path_w, L"ab", _SH_DENYNO);
    if (!f) {
        g_log_path_w[0] = 0;     /* degrade to OutputDebugString only */
        return;
    }
    g_log_file = f;
}

void log_close(void)
{
    if (!g_log_file) {
        return;
    }
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

void log_write(log_level lvl, const char *fmt, ...)
{
    char msg[LOG_MSG_CAP];
    char line[LOG_LINE_CAP];
    SYSTEMTIME st;
    va_list ap;
    int n;
    int len;

    if (!fmt) {
        fmt = "";
    }
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0) {
        msg[0] = 0;
    }
    GetLocalTime(&st);
    len = snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\n",
                   (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                   (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                   (unsigned)st.wMilliseconds, log_level_name(lvl), msg);
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof line) {
        size_t cut = sizeof line - 2;
        line[cut] = '\n';
        line[cut + 1] = 0;
        len = (int)(cut + 1);
    }
    OutputDebugStringA(line);
    if (!g_log_file) {
        return;
    }
    if (g_log_lock_ready) {
        EnterCriticalSection(&g_log_lock);
    }
    if (g_log_file) {
        fwrite(line, 1, (size_t)len, g_log_file);
        fflush(g_log_file);
        if (_ftelli64(g_log_file) > (long long)LOG_ROTATE_BYTES) {
            log_reopen_locked();
        }
    }
    if (g_log_lock_ready) {
        LeaveCriticalSection(&g_log_lock);
    }
}

/* ------------------------------------------------------------ crash report */

static volatile LONG g_crash_entered;
static volatile LONG g_veh_installed;
static volatile LONG g_veh_have;
static EXCEPTION_RECORD g_veh_record;

/* The CRT turns access violations into SIGSEGV before the unhandled filter
 * runs, which loses the exception code and fault address. This vectored
 * handler only records them for the signal path. */
static LONG CALLBACK log__veh_capture(EXCEPTION_POINTERS *ep)
{
    if (ep && ep->ExceptionRecord) {
        memcpy(&g_veh_record, ep->ExceptionRecord, sizeof g_veh_record);
        InterlockedExchange(&g_veh_have, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static EXCEPTION_POINTERS *log__signal_exception(int sig, EXCEPTION_POINTERS *storage)
{
    DWORD code;
    bool match = false;

    if (!g_veh_have) {
        return NULL;
    }
    code = g_veh_record.ExceptionCode;
    switch (sig) {
    case SIGSEGV:
        match = code == 0xC0000005u || code == 0xC0000006u;
        break;
    case SIGFPE:
        match = code == 0xC000008Eu || code == 0xC0000090u ||
                code == 0xC0000091u || code == 0xC0000094u;
        break;
    case SIGILL:
        match = code == 0xC000001Du;
        break;
    case SIGABRT:
        match = code == 0xC0000409u || code == 0xC0000602u;
        break;
    default:
        break;
    }
    if (!match) {
        return NULL;
    }
    memset(storage, 0, sizeof *storage);
    storage->ExceptionRecord = &g_veh_record;
    return storage;
}

typedef struct cb {
    char *d;
    size_t cap;
    size_t n;
} cb;

static void cb_init(cb *b, char *data, size_t cap)
{
    b->d = data;
    b->cap = cap;
    b->n = 0;
    if (cap) {
        data[0] = 0;
    }
}

static void cb_addn(cb *b, const char *s, size_t n)
{
    size_t room;
    if (b->n + 1 >= b->cap) {
        return;
    }
    room = b->cap - b->n - 1;
    if (n > room) {
        n = room;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
    b->d[b->n] = 0;
}

static void cb_s(cb *b, const char *s)
{
    if (s) {
        cb_addn(b, s, strlen(s));
    }
}

static void cb_c(cb *b, char c)
{
    cb_addn(b, &c, 1);
}

static void cb_u(cb *b, unsigned long long v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) {
        cb_c(b, '0');
        return;
    }
    while (v && i < (int)sizeof tmp) {
        tmp[i++] = (char)('0' + (int)(v % 10));
        v /= 10;
    }
    while (i > 0) {
        cb_c(b, tmp[--i]);
    }
}

static void cb_hex(cb *b, unsigned long long v)
{
    static const char hx[] = "0123456789ABCDEF";
    char tmp[20];
    int i = 0;
    do {
        tmp[i++] = hx[v & 0xF];
        v >>= 4;
    } while (v && i < (int)sizeof tmp);
    cb_s(b, "0x");
    while (i > 0) {
        cb_c(b, tmp[--i]);
    }
}

static void cb_put2(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 10) % 10));
    cb_c(b, (char)('0' + v % 10));
}

static void cb_put3(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 100) % 10));
    cb_put2(b, v);
}

static void cb_put4(cb *b, unsigned v)
{
    cb_c(b, (char)('0' + (v / 1000) % 10));
    cb_put3(b, v);
}

/* module+offset, and a symbol name when dbghelp is present */

typedef BOOL (WINAPI *sym_init_fn)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *sym_from_addr_fn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef USHORT (WINAPI *rtl_capture_fn)(ULONG, ULONG, PVOID *, PULONG);

static HMODULE g_dbghelp;
static sym_init_fn g_sym_init;
static sym_from_addr_fn g_sym_from_addr;
static rtl_capture_fn g_rtl_capture;
static char g_symbol_buf[sizeof(SYMBOL_INFO) + 1024];

static void crash_load_helpers(void)
{
    FARPROC fp;

    if (!g_dbghelp) {
        HMODULE m = LoadLibraryW(L"dbghelp.dll");
        if (m) {
            fp = GetProcAddress(m, "SymInitialize");
            memcpy(&g_sym_init, &fp, sizeof fp);
            fp = GetProcAddress(m, "SymFromAddr");
            memcpy(&g_sym_from_addr, &fp, sizeof fp);
            if (g_sym_init && g_sym_from_addr) {
                g_sym_init(GetCurrentProcess(), NULL, TRUE);
            }
            g_dbghelp = m;
        }
    }
    if (!g_rtl_capture) {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        fp = k ? GetProcAddress(k, "RtlCaptureStackBackTrace") : NULL;
        memcpy(&g_rtl_capture, &fp, sizeof fp);
    }
}

static void crash_module_name(void *addr, char *out, size_t cap)
{
    wchar_t path[CFG_WCHAR_CAP];
    wchar_t *base;
    HMODULE mod = NULL;

    if (cap) {
        out[0] = 0;
    }
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(ULONG_PTR)addr, &mod) || !mod) {
        strncpy_s(out, cap, "?", _TRUNCATE);
        return;
    }
    if (!GetModuleFileNameW(mod, path, CFG_WCHAR_CAP)) {
        strncpy_s(out, cap, "?", _TRUNCATE);
        return;
    }
    base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    if (!cfg__to_utf8(base, out, cap)) {
        strncpy_s(out, cap, "?", _TRUNCATE);
    }
}

static void crash_symbol_line(cb *b, void *addr)
{
    SYMBOL_INFO *si = (SYMBOL_INFO *)g_symbol_buf;
    DWORD64 disp = 0;
    char name[512];

    if (!g_sym_from_addr) {
        return;
    }
    memset(g_symbol_buf, 0, sizeof g_symbol_buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 480;
    if (!g_sym_from_addr(GetCurrentProcess(), (DWORD64)(ULONG_PTR)addr, &disp, si)) {
        return;
    }
    snprintf(name, sizeof name, "  symbol: %s+0x%llx\n",
             si->Name, (unsigned long long)disp);
    cb_s(b, name);
}

static void crash_frame_line(cb *b, void *addr, unsigned frame_no)
{
    char module[260];
    HMODULE mod = NULL;
    unsigned long long off = 0;

    crash_module_name(addr, module, sizeof module);
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(ULONG_PTR)addr, &mod) && mod) {
        off = (unsigned long long)((ULONG_PTR)addr - (ULONG_PTR)mod);
    }
    cb_s(b, "  #");
    cb_u(b, frame_no);
    cb_s(b, " ");
    cb_hex(b, (unsigned long long)(ULONG_PTR)addr);
    cb_s(b, " ");
    cb_s(b, module);
    cb_s(b, "+");
    cb_hex(b, off);
    cb_c(b, '\n');
    crash_symbol_line(b, addr);
}

static void crash_log_path(wchar_t *out, size_t cap)
{
    if (g_log_path_w[0]) {
        StringCchCopyW(out, cap, g_log_path_w);
        return;
    }
    if (cfg__log_dir_w[0]) {
        StringCchPrintfW(out, cap, L"%s\\app.log", cfg__log_dir_w);
        return;
    }
    if (cap) {
        out[0] = 0;
    }
}

static void crash_append_log_tail(cb *b)
{
    static char tail[CRASH_LOG_TAIL_BYTES];
    wchar_t path[CFG_WCHAR_CAP];
    HANDLE h;
    LARGE_INTEGER size;
    DWORD want;
    DWORD got = 0;
    size_t start = 0;
    int lines = 0;
    size_t i;

    cb_s(b, "\n---- app.log (tail) ----\n");
    crash_log_path(path, CFG_WCHAR_CAP);
    if (!path[0]) {
        cb_s(b, "<no log path>\n");
        return;
    }
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        cb_s(b, "<unavailable, win32 error ");
        cb_u(b, GetLastError());
        cb_s(b, ">\n");
        return;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        cb_s(b, "<empty>\n");
        return;
    }
    want = size.QuadPart > (LONGLONG)CRASH_LOG_TAIL_BYTES
         ? (DWORD)CRASH_LOG_TAIL_BYTES : (DWORD)size.QuadPart;
    {
        LARGE_INTEGER off;
        off.QuadPart = size.QuadPart - want;
        if (!SetFilePointerEx(h, off, NULL, FILE_BEGIN)) {
            CloseHandle(h);
            return;
        }
    }
    if (!ReadFile(h, tail, want, &got, NULL)) {
        got = 0;
    }
    CloseHandle(h);
    if (!got) {
        return;
    }
    for (i = got; i > 0; --i) {
        if (tail[i - 1] == '\n') {
            lines++;
            if (lines >= 100) {
                start = i;
                break;
            }
        }
    }
    cb_addn(b, tail + start, got - start);
    if (got == 0 || tail[got - 1] != '\n') {
        cb_c(b, '\n');
    }
}

/* RtlGetVersion instead of the deprecated GetVersionExW (lies under a
 * compatibility manifest). Declared locally; ntdll always exports it. */
typedef struct log__osvi {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} log__osvi;

typedef LONG (WINAPI *rtl_get_version_fn)(log__osvi *);

static bool log__os_version(log__osvi *os)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    FARPROC fp = ntdll ? GetProcAddress(ntdll, "RtlGetVersion") : NULL;
    rtl_get_version_fn fn = NULL;

    memcpy(&fn, &fp, sizeof fn);
    if (!fn) {
        return false;
    }
    memset(os, 0, sizeof *os);
    os->dwOSVersionInfoSize = sizeof *os;
    return fn(os) == 0;
}

static void crash_system_line(cb *b)
{
    MEMORYSTATUSEX ms;
    SYSTEM_INFO si;
    log__osvi os;
    char buf[256];

    if (log__os_version(&os)) {
        snprintf(buf, sizeof buf, "Windows %lu.%lu.%lu",
                 (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                 (unsigned long)os.dwBuildNumber);
    } else {
        strncpy_s(buf, sizeof buf, "Windows (unknown version)", _TRUNCATE);
    }
    GetNativeSystemInfo(&si);
    {
        const char *arch = "unknown";
        switch (si.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
        case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
        case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
        default: break;
        }
        cb_s(b, "system: ");
        cb_s(b, buf);
        cb_s(b, " ");
        cb_s(b, arch);
        cb_s(b, ", cpus: ");
        cb_u(b, si.dwNumberOfProcessors);
    }
    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        cb_s(b, ", memory: ");
        cb_u(b, ms.ullTotalPhys / (1024ull * 1024ull));
        cb_s(b, " MB");
    }
    cb_c(b, '\n');
}

static void crash_write_report(const char *reason, EXCEPTION_POINTERS *ep)
{
    static char report[CRASH_REPORT_CAP];
    wchar_t path[CFG_WCHAR_CAP];
    SYSTEMTIME st;
    cb b;
    DWORD code = 0;
    void *addr = NULL;
    void *frames[32];
    USHORT frame_count = 0;
    unsigned i;

    if (!cfg__log_dir_w[0]) {
        return;                 /* nowhere to write before cfg_init */
    }
    crash_load_helpers();

    GetLocalTime(&st);
    cb_init(&b, report, sizeof report);
    cb_s(&b, "AnimeDenoise crash report\n");
    cb_s(&b, "version: " APP_VERSION "\n");
    cb_s(&b, "time: ");
    cb_put4(&b, st.wYear);
    cb_c(&b, '-');
    cb_put2(&b, st.wMonth);
    cb_c(&b, '-');
    cb_put2(&b, st.wDay);
    cb_c(&b, ' ');
    cb_put2(&b, st.wHour);
    cb_c(&b, ':');
    cb_put2(&b, st.wMinute);
    cb_c(&b, ':');
    cb_put2(&b, st.wSecond);
    cb_c(&b, '.');
    cb_put3(&b, st.wMilliseconds);
    cb_s(&b, " (local)\n");
    cb_s(&b, "reason: ");
    cb_s(&b, reason ? reason : "unknown");
    cb_c(&b, '\n');

    if (ep && ep->ExceptionRecord) {
        code = ep->ExceptionRecord->ExceptionCode;
        addr = ep->ExceptionRecord->ExceptionAddress;
        cb_s(&b, "exception code: ");
        cb_hex(&b, code);
        if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            cb_s(&b, " ACCESS_VIOLATION (");
            cb_s(&b, op == 0 ? "read" : (op == 1 ? "write" : "execute"));
            cb_s(&b, " at ");
            cb_hex(&b, ep->ExceptionRecord->ExceptionInformation[1]);
            cb_s(&b, ")");
        } else if (code == 0xE06D7363) {
            cb_s(&b, " C++ exception");
        }
        cb_c(&b, '\n');
    } else {
        cb_s(&b, "exception code: n/a (signal/handler)\n");
    }
    cb_s(&b, "address: ");
    cb_hex(&b, (unsigned long long)(ULONG_PTR)addr);
    cb_c(&b, '\n');
    if (addr) {
        crash_frame_line(&b, addr, 0);
    }
    crash_system_line(&b);

    if (g_rtl_capture) {
        frame_count = g_rtl_capture(1, 32, frames, NULL);
    }
    cb_s(&b, "\nbacktrace (RtlCaptureStackBackTrace):\n");
    for (i = 0; i < frame_count; ++i) {
        crash_frame_line(&b, frames[i], i);
    }
    if (!frame_count) {
        cb_s(&b, "  <unavailable>\n");
    }
    crash_append_log_tail(&b);

    if (FAILED(StringCchPrintfW(path, CFG_WCHAR_CAP,
                                L"%s\\crash-%04u%02u%02u-%02u%02u%02u.txt",
                                cfg__log_dir_w, (unsigned)st.wYear, (unsigned)st.wMonth,
                                (unsigned)st.wDay, (unsigned)st.wHour,
                                (unsigned)st.wMinute, (unsigned)st.wSecond))) {
        return;
    }
    cfg__write_file_w(path, report, b.n);
}

static LONG WINAPI log__unhandled_filter(EXCEPTION_POINTERS *ep)
{
    DWORD code;

    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0xC0000005u;
    crash_write_report("unhandled exception", ep);
    TerminateProcess(GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

static int log__signal_code(int sig)
{
    switch (sig) {
    case SIGABRT: return 0xC0000409;   /* STATUS_STACK_BUFFER_OVERRUN-ish abort */
    case SIGSEGV: return 0xC0000005;
    case SIGFPE:  return 0xC0000094;
    case SIGILL:  return 0xC000001D;
    default:      return 0xC0000005;
    }
}

static void crash_signal_handler(int sig)
{
    EXCEPTION_POINTERS storage;
    EXCEPTION_POINTERS *ep = log__signal_exception(sig, &storage);
    int code;

    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    code = log__signal_code(sig);
    crash_write_report(sig == SIGABRT ? "abort (signal SIGABRT)" :
                       sig == SIGSEGV ? "segmentation fault (SIGSEGV)" :
                       sig == SIGFPE  ? "arithmetic fault (SIGFPE)" :
                       sig == SIGILL  ? "illegal instruction (SIGILL)" :
                                        "fatal signal", ep);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

static void crash_purecall_handler(void)
{
    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    crash_write_report("pure virtual function call", NULL);
    TerminateProcess(GetCurrentProcess(), 0xC0000409u);
}

static void crash_invalid_parameter_handler(const wchar_t *expression,
                                            const wchar_t *function,
                                            const wchar_t *file,
                                            unsigned int line,
                                            uintptr_t reserved)
{
    (void)expression;
    (void)function;
    (void)file;
    (void)line;
    (void)reserved;
    if (InterlockedCompareExchange(&g_crash_entered, 1, 0) != 0) {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    crash_write_report("invalid C runtime parameter", NULL);
    TerminateProcess(GetCurrentProcess(), 0xC000000D);
}

void log_install_crash_handler(void)
{
    if (InterlockedCompareExchange(&g_veh_installed, 1, 0) == 0) {
        AddVectoredExceptionHandler(1, log__veh_capture);
    }
    SetUnhandledExceptionFilter(log__unhandled_filter);
    _set_purecall_handler(crash_purecall_handler);
    _set_invalid_parameter_handler(crash_invalid_parameter_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGFPE, crash_signal_handler);
    signal(SIGILL, crash_signal_handler);
}

/* ----------------------------------------------------------- diagnostics */

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static uint32_t crc32_calc(const unsigned char *data, size_t len)
{
    uint32_t table[256];
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    unsigned j;

    for (i = 0; i < 256; ++i) {
        uint32_t c = (uint32_t)i;
        for (j = 0; j < 8; ++j) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    for (i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct zip_entry {
    char name[160];
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
    uint16_t dos_date;
    uint16_t dos_time;
} zip_entry;

typedef struct zip_state {
    HANDLE h;
    uint32_t offset;
    int count;
    zip_entry entries[DIAG_MAX_ENTRIES];
} zip_state;

static bool write_all(HANDLE h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    while (len) {
        DWORD chunk = (DWORD)(len > 0x10000000u ? 0x10000000u : len);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, NULL) || written != chunk) {
            return false;
        }
        p += chunk;
        len -= chunk;
    }
    return true;
}

static void dos_datetime(uint16_t *dos_date, uint16_t *dos_time)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *dos_date = (uint16_t)((((unsigned)st.wYear - 1980) << 9) |
                           ((unsigned)st.wMonth << 5) | (unsigned)st.wDay);
    *dos_time = (uint16_t)(((unsigned)st.wHour << 11) |
                           ((unsigned)st.wMinute << 5) | ((unsigned)st.wSecond / 2));
}

static bool zip_begin(zip_state *z, const wchar_t *path)
{
    memset(z, 0, sizeof *z);
    z->h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    return z->h != INVALID_HANDLE_VALUE;
}

static bool zip_add(zip_state *z, const char *name, const void *data, size_t len)
{
    unsigned char hdr[30];
    size_t nlen;
    zip_entry *e;

    if (!z->h || z->count >= DIAG_MAX_ENTRIES || len > 0x7FFFFFFFu) {
        return false;
    }
    nlen = strlen(name);
    if (nlen == 0 || nlen > sizeof z->entries[0].name - 1) {
        return false;
    }
    e = &z->entries[z->count];
    memset(e, 0, sizeof *e);
    memcpy(e->name, name, nlen + 1);
    e->crc = crc32_calc((const unsigned char *)data, len);
    e->size = (uint32_t)len;
    dos_datetime(&e->dos_date, &e->dos_time);
    e->offset = z->offset;

    put32(hdr, 0x04034b50u);
    put16(hdr + 4, 20);
    put16(hdr + 6, 0x0800);      /* UTF-8 names */
    put16(hdr + 8, 0);           /* store */
    put16(hdr + 10, e->dos_time);
    put16(hdr + 12, e->dos_date);
    put32(hdr + 14, e->crc);
    put32(hdr + 18, e->size);
    put32(hdr + 22, e->size);
    put16(hdr + 26, (unsigned)nlen);
    put16(hdr + 28, 0);
    if (!write_all(z->h, hdr, sizeof hdr) ||
        !write_all(z->h, name, nlen) ||
        !write_all(z->h, data, len)) {
        return false;
    }
    z->offset += (uint32_t)(sizeof hdr + nlen + len);
    z->count++;
    return true;
}

static bool zip_end(zip_state *z)
{
    unsigned char hdr[46];
    unsigned char eocd[22];
    uint32_t cd_start = z->offset;
    int i;

    for (i = 0; i < z->count; ++i) {
        const zip_entry *e = &z->entries[i];
        size_t nlen = strlen(e->name);
        put32(hdr, 0x02014b50u);
        put16(hdr + 4, 20);
        put16(hdr + 6, 20);
        put16(hdr + 8, 0x0800);
        put16(hdr + 10, 0);
        put16(hdr + 12, e->dos_time);
        put16(hdr + 14, e->dos_date);
        put32(hdr + 16, e->crc);
        put32(hdr + 20, e->size);
        put32(hdr + 24, e->size);
        put16(hdr + 28, (unsigned)nlen);
        put16(hdr + 30, 0);
        put16(hdr + 32, 0);
        put16(hdr + 34, 0);
        put16(hdr + 36, 0);
        put32(hdr + 38, 0);
        put32(hdr + 42, e->offset);
        if (!write_all(z->h, hdr, sizeof hdr) || !write_all(z->h, e->name, nlen)) {
            return false;
        }
        z->offset += (uint32_t)(sizeof hdr + nlen);
    }
    put32(eocd, 0x06054b50u);
    put16(eocd + 4, 0);
    put16(eocd + 6, 0);
    put16(eocd + 8, (unsigned)z->count);
    put16(eocd + 10, (unsigned)z->count);
    put32(eocd + 12, z->offset - cd_start);
    put32(eocd + 16, cd_start);
    put16(eocd + 20, 0);
    return write_all(z->h, eocd, sizeof eocd);
}

static void build_system_info(cfg__sb *b)
{
    SYSTEMTIME st;
    log__osvi os;
    SYSTEM_INFO si;
    MEMORYSTATUSEX ms;
    wchar_t cpu[256];
    DWORD cpu_len = (DWORD)sizeof cpu;
    char buf[512];
    const char *arch = "unknown";
    unsigned i;

    GetLocalTime(&st);
    cfg__sb_addf(b, "AnimeDenoise diagnostics\n");
    cfg__sb_addf(b, "version: %s\n", APP_VERSION);
    cfg__sb_addf(b, "time: %04u-%02u-%02u %02u:%02u:%02u (local)\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);

    if (log__os_version(&os)) {
        cfg__sb_addf(b, "os: Windows %lu.%lu build %lu\n",
                (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
                (unsigned long)os.dwBuildNumber);
    } else {
        cfg__sb_adds(b, "os: Windows (unknown version)\n");
    }

    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: arch = "x64"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: arch = "arm64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: arch = "x86"; break;
    default: break;
    }
    cfg__sb_addf(b, "arch: %s, logical cpus: %lu\n", arch,
            (unsigned long)si.dwNumberOfProcessors);

    cpu[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, NULL, cpu, &cpu_len) != ERROR_SUCCESS) {
        cpu[0] = 0;
    }
    if (cpu[0] && cfg__to_utf8(cpu, buf, sizeof buf)) {
        cfg__sb_addf(b, "cpu: %s\n", buf);
    }

    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        cfg__sb_addf(b, "memory: %llu MB total, %llu MB available\n",
                (unsigned long long)(ms.ullTotalPhys / (1024ull * 1024ull)),
                (unsigned long long)(ms.ullAvailPhys / (1024ull * 1024ull)));
    }

    cfg__sb_adds(b, "gpu:");
    for (i = 0; i < 8; ++i) {
        DISPLAY_DEVICEW dd;
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        if (!EnumDisplayDevicesW(NULL, i, &dd, 0)) {
            break;
        }
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            if (cfg__to_utf8(dd.DeviceString, buf, sizeof buf)) {
                cfg__sb_addf(b, " %s;", buf);
            }
        }
    }
    cfg__sb_adds(b, "\n");
    cfg__sb_addf(b, "data dir: %s\n", cfg__data_u8[0] ? "<data>" : "(unset)");
}

static bool diag_add_file(zip_state *z, const wchar_t *path, const char *name)
{
    char *buf = NULL;
    char *red;
    size_t len = 0;
    size_t rlen = 0;
    bool ok;

    if (!cfg__read_file_w(path, &buf, &len, DIAG_MAX_FILE_BYTES)) {
        return false;
    }
    red = cfg__redact_alloc(buf, len, &rlen);
    if (red) {
        ok = zip_add(z, name, red, rlen);
        free(red);
    } else {
        ok = zip_add(z, name, buf, len);
    }
    free(buf);
    return ok;
}

static bool has_zip_extension(const wchar_t *name)
{
    size_t n = wcslen(name);
    if (n < 5) {
        return false;
    }
    return _wcsicmp(name + n - 4, L".zip") == 0;
}

bool log_export_diagnostics(char *out_path, size_t cap)
{
    cfg__sb info;
    wchar_t info_path[CFG_WCHAR_CAP];
    wchar_t zip_path[CFG_WCHAR_CAP];
    wchar_t pattern[CFG_WCHAR_CAP];
    const wchar_t *zip_base;
    bool zip_ok = true;
    bool any = false;
    zip_state z;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    SYSTEMTIME st;

    if (!out_path || cap == 0) {
        return false;
    }
    out_path[0] = 0;
    if (!cfg__ready || !cfg__log_dir_w[0]) {
        return false;
    }

    memset(&info, 0, sizeof info);
    build_system_info(&info);
    if (info.oom) {
        cfg__sb_free(&info);
        return false;
    }
    if (FAILED(StringCchPrintfW(info_path, CFG_WCHAR_CAP, L"%s\\system-info.txt", cfg__log_dir_w))) {
        cfg__sb_free(&info);
        return false;
    }
    if (!cfg__write_file_w(info_path, info.p ? info.p : "", info.len)) {
        cfg__sb_free(&info);
        return false;
    }
    cfg__sb_free(&info);

    GetLocalTime(&st);
    if (FAILED(StringCchPrintfW(zip_path, CFG_WCHAR_CAP,
                                L"%s\\diagnostics-%04u%02u%02u-%02u%02u%02u.zip",
                                cfg__log_dir_w, (unsigned)st.wYear, (unsigned)st.wMonth,
                                (unsigned)st.wDay, (unsigned)st.wHour,
                                (unsigned)st.wMinute, (unsigned)st.wSecond))) {
        return false;
    }
    zip_base = wcsrchr(zip_path, L'\\');
    zip_base = zip_base ? zip_base + 1 : zip_path;

    if (!zip_begin(&z, zip_path)) {
        return false;
    }

    /* settings.json first so a zip that opened correctly always has it. */
    if (cfg__settings_path_w[0] && GetFileAttributesW(cfg__settings_path_w) != INVALID_FILE_ATTRIBUTES) {
        if (diag_add_file(&z, cfg__settings_path_w, "settings.json")) {
            any = true;
        }
    }
    if (diag_add_file(&z, info_path, "system-info.txt")) {
        any = true;
    }

    if (FAILED(StringCchPrintfW(pattern, CFG_WCHAR_CAP, L"%s\\*", cfg__log_dir_w))) {
        zip_ok = false;
    } else {
        find = FindFirstFileW(pattern, &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                wchar_t full[CFG_WCHAR_CAP];
                char name_u8[512];
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    continue;
                }
                if (_wcsicmp(fd.cFileName, zip_base) == 0) {
                    continue;
                }
                if (_wcsicmp(fd.cFileName, L"system-info.txt") == 0) {
                    continue;   /* already added above */
                }
                if (has_zip_extension(fd.cFileName)) {
                    continue;
                }
                if (!cfg__to_utf8(fd.cFileName, name_u8, sizeof name_u8)) {
                    continue;
                }
                if (!cfg__path_join(full, CFG_WCHAR_CAP, cfg__log_dir_w, fd.cFileName)) {
                    continue;
                }
                if (diag_add_file(&z, full, name_u8)) {
                    any = true;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    if (!zip_ok || !zip_end(&z) || !any) {
        CloseHandle(z.h);
        DeleteFileW(zip_path);
        return false;
    }
    CloseHandle(z.h);
    if (!cfg__to_utf8(zip_path, out_path, cap)) {
        out_path[0] = 0;
        return false;
    }
    return true;
}

