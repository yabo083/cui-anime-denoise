/* Self-update: the state machine, the worker thread and the public upd_* API.
 * Everything that talks to the network, parses JSON, hashes bytes or touches
 * the filesystem lives in update_io.c; see update_priv.h for the seam.
 *
 * Everything that can block (DNS, proxy resolution, TLS, HTTP, disk) happens
 * on one worker thread. The UI only ever touches the protected snapshot in
 * upd_get() and the two non-blocking starters upd_check()/upd_download().
 *
 * State machine:
 *
 *   IDLE --upd_check--> CHECKING --+--> UP_TO_DATE
 *                                  +--> AVAILABLE --upd_download--> DOWNLOADING
 *                                  +--> FAILED                    --+--> READY
 *                                                                  +--> FAILED
 *
 * Downloaded bytes go to update.bin.tmp and are only renamed to update.bin
 * after the size and SHA-256 match the manifest. update.json (the metadata)
 * is written last, so a snapshot of the directory is always consistent.
 */
#include "update_priv.h"

#include <shellapi.h>
#include <process.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPD_MIN_INTERVAL_MS  (6ull * 60ull * 60ull * 1000ull)

#define UPD_OP_CHECK    1
#define UPD_OP_DOWNLOAD 2

/* ------------------------------------------------------------------------ */
/* Shared state                                                             */
/* ------------------------------------------------------------------------ */

static CRITICAL_SECTION g_lock;
static bool g_lock_ready;

static HANDLE g_thread;               /* current worker, NULL when none */
static volatile LONG g_stop;          /* set by upd_shutdown */

static upd_info g_info;               /* guarded by g_lock */
static char g_manifest_url[UPD_PATH_CAP];
static char g_download_url[1024];
static char g_sha256[80];             /* lower-case hex expected hash */
static unsigned long long g_expected_size;
static bool g_have_manifest;          /* g_download_url/g_sha256 are valid */
static ULONGLONG g_last_check;        /* tick of the last check start */
static bool g_inited;

bool upd__stop_requested(void) {
    return InterlockedCompareExchange(&g_stop, 0, 0) != 0;
}

/* ------------------------------------------------------------------------ */
/* Version compare (x.y.z numeric, missing segments count as 0)             */
/* ------------------------------------------------------------------------ */

static int version_segment(const char **p) {
    const char *s = *p;
    unsigned long v = 0;
    bool any = false;
    while (*s && !isdigit((unsigned char)*s)) s++;
    while (*s && isdigit((unsigned char)*s)) {
        any = true;
        if (v < 100000000ul) v = v * 10ul + (unsigned long)(*s - '0');
        s++;
    }
    *p = s;
    return any ? (int)v : 0;
}

/* -1 when a < b, 0 when equal, 1 when a > b. */
static int version_cmp(const char *a, const char *b) {
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";
    int i;
    for (i = 0; i < 4; i++) {
        int va = version_segment(&pa);
        int vb = version_segment(&pb);
        if (va != vb) return va < vb ? -1 : 1;
        if (!*pa && !*pb) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* State helpers                                                            */
/* ------------------------------------------------------------------------ */

static void set_state(upd_state state) {
    EnterCriticalSection(&g_lock);
    g_info.state = state;
    LeaveCriticalSection(&g_lock);
}

static void set_failed(const char *message) {
    EnterCriticalSection(&g_lock);
    g_info.state = UPD_FAILED;
    g_info.progress = 0.0f;
    snprintf(g_info.error, sizeof g_info.error, "%s", message ? message : "unknown error");
    LeaveCriticalSection(&g_lock);
    log_error("update: %s", message ? message : "unknown error");
}

/* ------------------------------------------------------------------------ */
/* Worker: check                                                            */
/* ------------------------------------------------------------------------ */

static void do_check(void) {
    upd_manifest m;
    char err[512];
    char current[32];
    bool ok;
    int cmp;

    err[0] = 0;
    EnterCriticalSection(&g_lock);
    snprintf(current, sizeof current, "%s", g_info.current);
    LeaveCriticalSection(&g_lock);

    ok = upd__fetch_manifest(g_manifest_url, &m, err, sizeof err);
    if (!ok) {
        if (!upd__stop_requested()) set_failed(err[0] ? err : "manifest download failed");
        return;
    }
    if (upd__stop_requested()) return;

    cmp = version_cmp(m.version, current);
    log_info("update: manifest version %s, current %s", m.version, current);

    if (cmp > 0) {
        if (m.url[0] == 0 || !upd__sha256_looks_valid(m.sha256)) {
            set_failed("manifest: newer version but missing url/sha256");
            return;
        }
        upd__sha256_lower(m.sha256);
        EnterCriticalSection(&g_lock);
        snprintf(g_info.latest, sizeof g_info.latest, "%s", m.version);
        snprintf(g_info.notes, sizeof g_info.notes, "%s", m.notes);
        snprintf(g_info.page, sizeof g_info.page, "%s", m.page);
        g_info.size = m.size;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        snprintf(g_download_url, sizeof g_download_url, "%s", m.url);
        snprintf(g_sha256, sizeof g_sha256, "%s", m.sha256);
        g_expected_size = m.size;
        g_have_manifest = true;
        g_info.state = UPD_AVAILABLE;
        LeaveCriticalSection(&g_lock);
        log_info("update: %s available (%llu bytes)", m.version, m.size);
    } else {
        EnterCriticalSection(&g_lock);
        snprintf(g_info.latest, sizeof g_info.latest, "%s", m.version);
        snprintf(g_info.notes, sizeof g_info.notes, "%s", m.notes);
        snprintf(g_info.page, sizeof g_info.page, "%s", m.page);
        g_info.size = m.size;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        g_download_url[0] = 0;
        g_sha256[0] = 0;
        g_expected_size = 0;
        g_have_manifest = false;
        g_info.state = UPD_UP_TO_DATE;
        LeaveCriticalSection(&g_lock);
        log_info("update: already up to date");
    }
}

/* ------------------------------------------------------------------------ */
/* Worker: download                                                         */
/* ------------------------------------------------------------------------ */

/* Progress sink handed to upd__download_to_file(): the transfer loop already
 * throttles to one call per 100 ms (plus the final chunk), so this only
 * clamps and publishes the snapshot. */
static void publish_progress(void *ctx, unsigned long long done,
                             unsigned long long total) {
    float p;

    (void)ctx;
    if (total == 0) return;
    p = (float)((double)done / (double)total);
    if (p < 0.0f) p = 0.0f;
    if (p > 0.995f) p = 0.995f;
    EnterCriticalSection(&g_lock);
    g_info.progress = p;
    LeaveCriticalSection(&g_lock);
}

static void do_download(void) {
    char url[1024];
    char sha[80];
    char version[32];
    char tmp_path[UPD_PATH_CAP];
    char bin_path[UPD_PATH_CAP];
    char json_path[UPD_PATH_CAP];
    wchar_t wtmp[UPD_WPATH_CAP];
    wchar_t wbin[UPD_WPATH_CAP];
    wchar_t wjson[UPD_WPATH_CAP];
    unsigned long long expected_size;
    unsigned long long total = 0;
    char computed[65];
    char errbuf[512];
    bool have_manifest;

    errbuf[0] = 0;
    computed[0] = 0;

    EnterCriticalSection(&g_lock);
    snprintf(url, sizeof url, "%s", g_download_url);
    snprintf(sha, sizeof sha, "%s", g_sha256);
    snprintf(version, sizeof version, "%s", g_info.latest);
    expected_size = g_expected_size;
    have_manifest = g_have_manifest;
    LeaveCriticalSection(&g_lock);

    if (!have_manifest || url[0] == 0 || sha[0] == 0) {
        set_failed("no download metadata");
        return;
    }
    if (!upd__ensure_updates_dir() ||
        !upd__updates_path(tmp_path, sizeof tmp_path, "update.bin.tmp") ||
        !upd__updates_path(bin_path, sizeof bin_path, "update.bin") ||
        !upd__updates_path(json_path, sizeof json_path, "update.json")) {
        set_failed("cannot resolve the updates directory");
        return;
    }
    if (!upd__to_wide(tmp_path, wtmp, UPD_WPATH_CAP) ||
        !upd__to_wide(bin_path, wbin, UPD_WPATH_CAP) ||
        !upd__to_wide(json_path, wjson, UPD_WPATH_CAP)) {
        set_failed("update path is too long");
        return;
    }

    /* Streams, hashes and verifies (size + SHA-256) into update.bin.tmp. A
     * rejected transfer leaves no file behind. */
    if (!upd__download_to_file(url, wtmp, expected_size, sha, publish_progress,
                               NULL, computed, &total, errbuf, sizeof errbuf)) {
        set_failed(errbuf);
        return;
    }
    if (!MoveFileExW(wtmp, wbin, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(errbuf, sizeof errbuf, "cannot finalize update.bin (error %lu)",
                 GetLastError());
        DeleteFileW(wtmp);
        set_failed(errbuf);
        return;
    }
    {
        char json[512];
        int n = snprintf(json, sizeof json,
                         "{\n  \"version\": \"%s\",\n  \"sha256\": \"%s\",\n"
                         "  \"size\": %llu\n}\n",
                         version, computed, total);
        if (n > 0 && (size_t)n < sizeof json) {
            if (!upd__write_text_w(wjson, json)) {
                log_warn("update: update.bin verified but update.json could not be written");
            }
        }
    }
    EnterCriticalSection(&g_lock);
    g_info.progress = 1.0f;
    g_info.size = total;
    g_info.state = UPD_READY;
    g_info.error[0] = 0;
    LeaveCriticalSection(&g_lock);
    log_info("update: %s downloaded and verified (%llu bytes)", version, total);
}

/* ------------------------------------------------------------------------ */
/* Thread plumbing                                                          */
/* ------------------------------------------------------------------------ */

static unsigned __stdcall update_thread(void *arg) {
    int op = (int)(intptr_t)arg;
    if (op == UPD_OP_CHECK) do_check();
    else if (op == UPD_OP_DOWNLOAD) do_download();
    return 0;
}

/* Closes the handle of a finished worker; true when no worker is running.
 * A worker that just published its final state may need a few more
 * microseconds to actually exit, so the caller can allow a short grace
 * period instead of failing a brand-new operation. */
static bool reap_thread(DWORD timeout_ms) {
    HANDLE h;

    EnterCriticalSection(&g_lock);
    h = g_thread;
    LeaveCriticalSection(&g_lock);
    if (!h) return true;
    if (WaitForSingleObject(h, timeout_ms) != WAIT_OBJECT_0) return false;

    EnterCriticalSection(&g_lock);
    if (g_thread == h) g_thread = NULL;
    LeaveCriticalSection(&g_lock);
    CloseHandle(h);
    return true;
}

static bool start_thread(int op) {
    uintptr_t handle;

    if (!reap_thread(250)) return false;
    InterlockedExchange(&g_stop, 0);
    handle = _beginthreadex(NULL, 0, update_thread, (void *)(intptr_t)op, 0, NULL);
    if (!handle) return false;
    EnterCriticalSection(&g_lock);
    g_thread = (HANDLE)handle;
    LeaveCriticalSection(&g_lock);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                               */
/* ------------------------------------------------------------------------ */

bool upd_init(const char *current, const char *manifest_url) {
    const char *data;

    if (g_inited) upd_shutdown();

    InitializeCriticalSection(&g_lock);
    g_lock_ready = true;

    memset(&g_info, 0, sizeof g_info);
    memset(g_manifest_url, 0, sizeof g_manifest_url);
    memset(g_download_url, 0, sizeof g_download_url);
    memset(g_sha256, 0, sizeof g_sha256);
    g_have_manifest = false;
    g_expected_size = 0;
    g_last_check = 0;
    g_thread = NULL;
    InterlockedExchange(&g_stop, 0);
    g_inited = true;

    snprintf(g_info.current, sizeof g_info.current, "%s",
             (current && current[0]) ? current : "0.0.0");
    if (manifest_url && manifest_url[0]) {
        size_t n = strlen(manifest_url);
        if (n >= sizeof g_manifest_url) {
            g_inited = false;
            DeleteCriticalSection(&g_lock);
            g_lock_ready = false;
            return false;
        }
        memcpy(g_manifest_url, manifest_url, n + 1);
        g_info.state = UPD_IDLE;
    } else {
        g_info.state = UPD_UP_TO_DATE;
    }

    data = cfg_data_dir();
    if (!data || !data[0]) {
        upd_shutdown();
        return false;
    }
    if (!upd__ensure_updates_dir()) {
        log_warn("update: cannot create the updates directory");
    }
    log_info("update: initialised, current %s%s%s", g_info.current,
             g_manifest_url[0] ? ", manifest " : "",
             g_manifest_url[0] ? g_manifest_url : "");
    return true;
}

void upd_shutdown(void) {
    HANDLE h;

    if (!g_inited) return;
    InterlockedExchange(&g_stop, 1);

    EnterCriticalSection(&g_lock);
    h = g_thread;
    g_thread = NULL;
    LeaveCriticalSection(&g_lock);
    if (h) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    if (g_lock_ready) {
        DeleteCriticalSection(&g_lock);
        g_lock_ready = false;
    }
    g_inited = false;
    memset(&g_info, 0, sizeof g_info);
    g_info.state = UPD_IDLE;
}

void upd_check(bool force) {
    bool disabled;
    bool busy;
    bool recent;
    bool can_start;
    ULONGLONG now;

    if (!g_inited) return;

    EnterCriticalSection(&g_lock);
    disabled = g_manifest_url[0] == 0;
    busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    now = GetTickCount64();
    recent = g_last_check != 0 && (now - g_last_check) < UPD_MIN_INTERVAL_MS;
    can_start = !busy && (force || !recent);
    if (can_start) {
        g_info.state = UPD_CHECKING;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        g_last_check = now;
    }
    LeaveCriticalSection(&g_lock);

    if (disabled) {
        set_state(UPD_UP_TO_DATE);
        return;
    }
    if (!can_start) return;
    if (!start_thread(UPD_OP_CHECK)) {
        set_failed("cannot start the update thread");
    }
}

void upd_download(void) {
    bool can_start = false;
    bool busy;

    if (!g_inited) return;

    EnterCriticalSection(&g_lock);
    busy = g_info.state == UPD_CHECKING || g_info.state == UPD_DOWNLOADING;
    if (!busy && g_info.state == UPD_AVAILABLE && g_have_manifest) {
        g_info.state = UPD_DOWNLOADING;
        g_info.progress = 0.0f;
        g_info.error[0] = 0;
        can_start = true;
    }
    LeaveCriticalSection(&g_lock);

    if (!can_start) return;
    if (!start_thread(UPD_OP_DOWNLOAD)) {
        /* start_thread() may have failed because another worker is still
         * alive; only report FAILED when nothing is running. */
        if (reap_thread(0)) set_failed("cannot start the update thread");
        else set_state(UPD_AVAILABLE);
    }
}

void upd_get(upd_info *out) {
    if (!out) return;
    if (!g_inited) {
        memset(out, 0, sizeof *out);
        out->state = UPD_IDLE;
        return;
    }
    EnterCriticalSection(&g_lock);
    *out = g_info;
    LeaveCriticalSection(&g_lock);
}

bool upd_run_installer(void) {
    char bin_path[UPD_PATH_CAP];
    char marker[UPD_PATH_CAP];
    wchar_t wbin[UPD_WPATH_CAP];
    wchar_t wmarker[UPD_WPATH_CAP];
    char actual[65];
    char expected[80];
    char version[32];
    unsigned long long size = 0;
    SHELLEXECUTEINFOW sei;
    bool have_expected = false;

    if (!g_inited) return false;
    if (!upd__updates_path(bin_path, sizeof bin_path, "update.bin") ||
        !upd__updates_path(marker, sizeof marker, "pending-version.txt")) {
        log_error("update: cannot resolve installer paths");
        return false;
    }
    if (!upd__to_wide(bin_path, wbin, UPD_WPATH_CAP) ||
        !upd__to_wide(marker, wmarker, UPD_WPATH_CAP)) {
        log_error("update: installer path is too long");
        return false;
    }
    if (GetFileAttributesW(wbin) == INVALID_FILE_ATTRIBUTES) {
        log_error("update: update.bin is missing");
        return false;
    }
    if (!upd__file_sha256_w(wbin, actual, &size)) {
        log_error("update: cannot hash update.bin");
        return false;
    }
    EnterCriticalSection(&g_lock);
    snprintf(expected, sizeof expected, "%s", g_sha256);
    snprintf(version, sizeof version, "%s", g_info.latest);
    have_expected = g_have_manifest && expected[0] != 0;
    LeaveCriticalSection(&g_lock);

    /* A previous session may have left a verified update.bin behind: trust
     * the metadata written last, not the payload, for the expected digest. */
    if (!have_expected) {
        char json_path[UPD_PATH_CAP];
        wchar_t wjson[UPD_WPATH_CAP];
        char *body = NULL;
        size_t len = 0;
        upd_manifest meta;
        char perr[128];
        if (upd__updates_path(json_path, sizeof json_path, "update.json") &&
            upd__to_wide(json_path, wjson, UPD_WPATH_CAP) &&
            upd__read_text_w(wjson, &body, &len, 65536)) {
            if (upd__parse_manifest(body, len, &meta, perr, sizeof perr) &&
                upd__sha256_looks_valid(meta.sha256)) {
                upd__sha256_lower(meta.sha256);
                snprintf(expected, sizeof expected, "%s", meta.sha256);
                snprintf(version, sizeof version, "%s", meta.version);
                have_expected = true;
            }
            free(body);
        }
    }

    if (!have_expected) {
        log_error("update: no expected hash for update.bin");
        return false;
    }
    if (!upd__hex_equal(expected, actual)) {
        log_error("update: update.bin hash mismatch, removing it");
        DeleteFileW(wbin);
        return false;
    }

    if (!upd__write_text_w(wmarker, version)) {
        log_error("update: cannot write pending-version.txt");
        return false;
    }
    ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = wbin;
    sei.lpParameters = L"/SILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS /NORESTART";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        log_error("update: ShellExecuteExW failed (error %lu)", GetLastError());
        DeleteFileW(wmarker);
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    log_info("update: installer launched for %s (%llu bytes)", version, size);
    return true;
}

bool upd_take_updated_flag(char *version, size_t cap) {
    char marker[UPD_PATH_CAP];
    wchar_t wmarker[UPD_WPATH_CAP];
    HANDLE f;
    char buf[128];
    DWORD got = 0;
    bool existed;
    size_t len;

    if (version && cap) version[0] = 0;
    if (!g_inited) return false;
    if (!upd__updates_path(marker, sizeof marker, "pending-version.txt") ||
        !upd__to_wide(marker, wmarker, UPD_WPATH_CAP)) {
        return false;
    }
    f = CreateFileW(wmarker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    existed = true;
    if (!ReadFile(f, buf, (DWORD)sizeof buf - 1, &got, NULL)) got = 0;
    CloseHandle(f);
    DeleteFileW(wmarker);
    if (got == 0) return existed;
    buf[got] = 0;
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
                       buf[len - 1] == ' ' || buf[len - 1] == '\t')) {
        buf[--len] = 0;
    }
    if (version && cap) snprintf(version, cap, "%s", buf);
    return existed;
}
