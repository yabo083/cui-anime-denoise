/* plat_win.c -- the Win32 half of plat.h. These are the calls the engine
 * and the CLI always made, kept verbatim so the Windows build behaves
 * exactly as before; only the names changed (plat_ prefix, eng_path_t).
 */
#include "plat.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Paths                                                                      */
/* ------------------------------------------------------------------------- */

eng_path_t *plat_join_path(const eng_path_t *dir, const eng_path_t *name)
{
    size_t dl, nl;
    eng_path_t *p;
    if (!dir || !name) return NULL;
    dl = wcslen(dir);
    nl = wcslen(name);
    p = (eng_path_t *)malloc((dl + nl + 2) * sizeof(eng_path_t));
    if (!p) return NULL;
    memcpy(p, dir, dl * sizeof(eng_path_t));
    if (dl && p[dl - 1] != L'\\' && p[dl - 1] != L'/') {
        p[dl++] = L'\\';
    }
    memcpy(p + dl, name, (nl + 1) * sizeof(eng_path_t));
    return p;
}

eng_path_t *plat_join_u8(const eng_path_t *dir, const char *name_u8)
{
    wchar_t wname[520];
    int n;
    if (!dir || !name_u8) return NULL;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name_u8, -1,
                          wname, (int)(sizeof wname / sizeof wname[0]));
    if (n <= 0) return NULL;
    return plat_join_path(dir, wname);
}

void plat_sncpy(eng_path_t *dst, const eng_path_t *src, size_t n)
{
    wcsncpy(dst, src, n);
}

size_t plat_plen(const eng_path_t *s)
{
    return wcslen(s);
}

bool plat_path_to_u8(const eng_path_t *p, char *out, size_t cap)
{
    int n;
    if (!out || !cap) return false;
    out[0] = 0;
    if (!p) return false;
    n = WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

eng_path_t *plat_u8_to_path(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */

bool plat_file_exists(const eng_path_t *p)
{
    return p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

bool plat_is_dir(const eng_path_t *p)
{
    DWORD a = p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool plat_file_size(const eng_path_t *p, unsigned long long *out)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!p || !GetFileAttributesExW(p, GetFileExInfoStandard, &fa)) return false;
    if (out) *out = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return true;
}

bool plat_read_head(const eng_path_t *p, void *buf, size_t n)
{
    HANDLE f;
    DWORD got = 0;
    int ok;
    if (!p) return false;
    f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    ok = n == 0 ||
        (ReadFile(f, buf, (DWORD)n, &got, NULL) && got == (DWORD)n);
    CloseHandle(f);
    return ok != 0;
}

unsigned char *plat_read_all(const eng_path_t *p, size_t *out_len)
{
    HANDLE f;
    LARGE_INTEGER sz;
    unsigned char *buf;
    DWORD got = 0, chunk;
    size_t off = 0;

    f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > (LONGLONG)0x7fffffff) {
        CloseHandle(f);
        return NULL;
    }
    buf = (unsigned char *)malloc((size_t)sz.QuadPart ? (size_t)sz.QuadPart : 1);
    if (!buf) {
        CloseHandle(f);
        return NULL;
    }
    while (off < (size_t)sz.QuadPart) {
        chunk = (DWORD)((sz.QuadPart - (LONGLONG)off > 1 << 24)
                            ? 1 << 24
                            : sz.QuadPart - (LONGLONG)off);
        if (!ReadFile(f, buf + off, chunk, &got, NULL) || got == 0) {
            free(buf);
            CloseHandle(f);
            return NULL;
        }
        off += got;
    }
    CloseHandle(f);
    *out_len = off;
    return buf;
}

bool plat_write_all(const eng_path_t *p, const void *data, size_t len)
{
    HANDLE f;
    size_t off = 0;
    int ok = 1;
    if (!p) return false;
    f = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    while (off < len) {
        DWORD chunk = (DWORD)(len - off > (size_t)(1 << 24) ? (size_t)(1 << 24)
                                                           : len - off);
        DWORD wrote = 0;
        if (!WriteFile(f, (const char *)data + off, chunk, &wrote, NULL) ||
            wrote != chunk) {
            ok = 0;
            break;
        }
        off += wrote;
    }
    CloseHandle(f);
    return ok != 0;
}

bool plat_copy_file(const eng_path_t *src, const eng_path_t *dst)
{
    return src && dst && CopyFileW(src, dst, FALSE) != 0;
}

bool plat_rename(const eng_path_t *src, const eng_path_t *dst)
{
    return src && dst &&
           MoveFileExW(src, dst,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
}

bool plat_unlink(const eng_path_t *p)
{
    return p && DeleteFileW(p) != 0;
}

bool plat_mkdir(const eng_path_t *p)
{
    DWORD a;
    if (!p) return false;
    if (CreateDirectoryW(p, NULL)) return true;
    a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

eng_path_t *plat_exe_dir(void)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    wchar_t *end;
    if (!n || n >= sizeof buf / sizeof buf[0]) return NULL;
    end = buf + n;
    while (end > buf && end[-1] != L'\\' && end[-1] != L'/') end--;
    *end = L'\0';
    return _wcsdup(buf);
}

/* ------------------------------------------------------------------------- */
/* Directory listing                                                          */
/* ------------------------------------------------------------------------- */

struct plat_dir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int primed;   /* first FindFirstFile result not yet consumed */
};

plat_dir *plat_dir_open(const eng_path_t *dir)
{
    plat_dir *d;
    eng_path_t *pat;
    if (!dir) return NULL;
    pat = plat_join_path(dir, L"*");
    if (!pat) return NULL;
    d = (plat_dir *)calloc(1, sizeof *d);
    if (!d) {
        free(pat);
        return NULL;
    }
    d->h = FindFirstFileW(pat, &d->fd);
    free(pat);
    if (d->h == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->primed = 1;
    return d;
}

eng_path_t *plat_dir_next_file(plat_dir *d)
{
    if (!d) return NULL;
    for (;;) {
        if (!d->primed && !FindNextFileW(d->h, &d->fd)) return NULL;
        d->primed = 0;
        if (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        return _wcsdup(d->fd.cFileName);
    }
}

void plat_dir_close(plat_dir *d)
{
    if (!d) return;
    FindClose(d->h);
    free(d);
}

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

int plat_stricmp(const char *a, const char *b)
{
    return _stricmp(a, b);
}

int plat_strnicmp(const char *a, const char *b, size_t n)
{
    return _strnicmp(a, b, n);
}

/* ------------------------------------------------------------------------- */
/* Atomics / time                                                             */
/* ------------------------------------------------------------------------- */

long plat_atomic_set(volatile long *p, long v)
{
    return InterlockedExchange(p, v);
}

long plat_atomic_get(volatile long *p)
{
    return InterlockedCompareExchange(p, 0, 0);
}

unsigned long long plat_tick_ms(void)
{
    return GetTickCount64();
}

void plat_sleep_ms(unsigned ms)
{
    Sleep(ms);
}

int plat_num_cpus(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors >= 1 ? (int)si.dwNumberOfProcessors : 1;
}

/* ------------------------------------------------------------------------- */
/* Threads                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    plat_thread_fn fn;
    void *arg;
} plat__start;

static DWORD WINAPI plat__entry(void *pv)
{
    plat__start *s = (plat__start *)pv;
    plat_thread_fn fn = s->fn;
    void *arg = s->arg;
    free(s);
    plat_thread_below_normal();
    fn(arg);
    return 0;
}

int plat_thread_spawn(plat_thread *out, plat_thread_fn fn, void *arg)
{
    plat__start *s;
    HANDLE h;
    if (!out || !fn) return -1;
    s = (plat__start *)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    h = CreateThread(NULL, 0, plat__entry, s, 0, NULL);
    if (!h) {
        free(s);
        return -1;
    }
    *out = (plat_thread)h;
    return 0;
}

void plat_thread_join(plat_thread *t)
{
    if (!t || !*t) return;
    WaitForSingleObject((HANDLE)*t, INFINITE);
    CloseHandle((HANDLE)*t);
    *t = NULL;
}

void plat_thread_below_normal(void)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

/* ------------------------------------------------------------------------- */
/* Dynamic loading                                                            */
/* ------------------------------------------------------------------------- */

void *plat_dlopen(const eng_path_t *path)
{
    /* Full path + DLL-load dir: never pick up the System32 onnxruntime.dll
     * and resolve onnxruntime_providers_shared.dll / DirectML.dll from the
     * same folder. */
    return (void *)LoadLibraryExW(
        path, NULL,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

void *plat_dlsym(void *lib, const char *name)
{
    return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL;
}

void plat_dlclose(void *lib)
{
    if (lib) FreeLibrary((HMODULE)lib);
}
