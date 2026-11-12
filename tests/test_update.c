/* Driver for src/update.c against a local HTTP server.
 *
 *   test_update.exe <manifest-url> [expected-sha256] [--expect-fail]
 *   test_update.exe -                                  (disabled: empty URL)
 *
 * `-` checks that an empty manifest URL keeps the state at UPD_UP_TO_DATE.
 *
 * Normal mode: fetch the manifest, wait for UPD_AVAILABLE, download, wait for
 * UPD_READY, then re-hash update.bin and compare it with the expected digest
 * (from the manifest, passed on the command line). Also exercises the
 * pending-version round trip and the "hash mismatch deletes the file" branch
 * of upd_run_installer() by corrupting the downloaded file afterwards.
 *
 * --expect-fail: the manifest points at a wrong sha256; the download must end
 * in UPD_FAILED and leave neither update.bin nor update.bin.tmp behind.
 *
 * Build (from the repo root, with MSVC active):
 *   cl /nologo /utf-8 /W4 /std:c11 /I src tests\test_update.c src\update.c
 *      src\update_io.c src\config.c src\log.c /Fe:tmp\test_update.exe
 *      /link winhttp.lib bcrypt.lib shell32.lib ole32.lib advapi32.lib user32.lib
 *
 * See tests/README.md for the full local-server recipe.
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/update.h"
#include "../src/config.h"

static int failures = 0;

static void check(int cond, const char *what) {
    printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) failures++;
}

static const char *state_name(upd_state s) {
    switch (s) {
    case UPD_IDLE: return "IDLE";
    case UPD_CHECKING: return "CHECKING";
    case UPD_UP_TO_DATE: return "UP_TO_DATE";
    case UPD_AVAILABLE: return "AVAILABLE";
    case UPD_DOWNLOADING: return "DOWNLOADING";
    case UPD_READY: return "READY";
    case UPD_FAILED: return "FAILED";
    default: return "?";
    }
}

static bool join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s\\%s", a, b);
    return n > 0 && (size_t)n < cap;
}

static bool to_wide(const char *s, wchar_t *out, size_t cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap);
    return n > 0;
}

static bool file_exists(const char *utf8) {
    wchar_t w[2048];
    return to_wide(utf8, w, 2048) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

static void delete_file(const char *utf8) {
    wchar_t w[2048];
    if (to_wide(utf8, w, 2048)) DeleteFileW(w);
}

static bool write_all(const char *utf8, const char *text) {
    wchar_t w[2048];
    HANDLE f;
    DWORD wrote = 0;
    DWORD len = (DWORD)strlen(text);
    if (!to_wide(utf8, w, 2048)) return false;
    f = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (!WriteFile(f, text, len, &wrote, NULL) || wrote != len) {
        CloseHandle(f);
        return false;
    }
    CloseHandle(f);
    return true;
}

/* Independent SHA-256 of a file, for verifying update.bin. */
static bool sha256_file(const char *utf8, char out[65], unsigned long long *size) {
    wchar_t w[2048];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_len = 0, got = 0;
    unsigned char digest[32];
    unsigned char buf[65536];
    HANDLE f;
    DWORD read = 0;
    unsigned long long total = 0;
    bool ok = true;
    static const char hex[] = "0123456789abcdef";
    int i;

    if (!to_wide(utf8, w, 2048)) return false;
    f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) {
        CloseHandle(f);
        return false;
    }
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_len,
                          sizeof object_len, &got, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);
        return false;
    }
    object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!object ||
        BCryptCreateHash(alg, &hash, object, object_len, NULL, 0, 0) < 0) {
        if (object) HeapFree(GetProcessHeap(), 0, object);
        BCryptCloseAlgorithmProvider(alg, 0);
        CloseHandle(f);
        return false;
    }
    for (;;) {
        if (!ReadFile(f, buf, (DWORD)sizeof buf, &read, NULL) || read == 0) break;
        if (BCryptHashData(hash, buf, read, 0) < 0) {
            ok = false;
            break;
        }
        total += read;
    }
    if (ok && BCryptFinishHash(hash, digest, 32, 0) < 0) ok = false;
    for (i = 0; ok && i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = 0;
    if (ok && size) *size = total;
    BCryptDestroyHash(hash);
    HeapFree(GetProcessHeap(), 0, object);
    BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

/* Polls upd_get() until the state is `want`. Fails fast on UPD_FAILED unless
 * `want` is UPD_FAILED. */
static bool wait_for_state(upd_state want, unsigned timeout_ms) {
    ULONGLONG start = GetTickCount64();
    upd_state last = (upd_state)-1;
    upd_info info;
    for (;;) {
        upd_get(&info);
        if (info.state != last) {
            printf("  state -> %-11s", state_name(info.state));
            if (info.state == UPD_DOWNLOADING) printf(" progress %.0f%%", info.progress * 100.0);
            if (info.state == UPD_FAILED) printf(" (%s)", info.error);
            if (info.state == UPD_AVAILABLE) {
                printf(" latest %s, %llu bytes", info.latest, info.size);
            }
            printf("\n");
            last = info.state;
        }
        if (info.state == want) return true;
        if (info.state == UPD_FAILED && want != UPD_FAILED) return false;
        if (GetTickCount64() - start > timeout_ms) {
            printf("  timeout waiting for %s after %u ms\n", state_name(want), timeout_ms);
            return false;
        }
        Sleep(50);
    }
}

int main(int argc, char **argv) {
    const char *url = NULL;
    const char *expect_sha = NULL;
    bool expect_fail = false;
    char data[1024];
    char bin[1100];
    char tmp[1100];
    char json[1100];
    char marker[1100];
    int i;
    upd_info info;
    const char *cfg_data;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--expect-fail") == 0) expect_fail = true;
        else if (!url) url = argv[i];
        else if (!expect_sha) expect_sha = argv[i];
        else {
            fprintf(stderr, "unexpected argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!url) {
        fprintf(stderr,
                "usage: %s <manifest-url|-> [expected-sha256] [--expect-fail]\n",
                argv[0]);
        return 2;
    }

    if (!cfg_init()) {
        fprintf(stderr, "cfg_init failed\n");
        return 1;
    }
    cfg_data = cfg_data_dir();
    if (!cfg_data || !cfg_data[0] ||
        !join(data, sizeof data, cfg_data, "updates") ||
        !join(bin, sizeof bin, data, "update.bin") ||
        !join(tmp, sizeof tmp, data, "update.bin.tmp") ||
        !join(json, sizeof json, data, "update.json") ||
        !join(marker, sizeof marker, data, "pending-version.txt")) {
        fprintf(stderr, "cannot resolve the data directory\n");
        return 1;
    }
    printf("data dir: %s\n", data);
    printf("manifest: %s\n", strcmp(url, "-") == 0 ? "(disabled)" : url);

    /* Clean slate so "no update.bin after failure" is a real check. */
    delete_file(bin);
    delete_file(tmp);
    delete_file(json);
    delete_file(marker);

    if (!upd_init("1.0.0", strcmp(url, "-") == 0 ? "" : url)) {
        fprintf(stderr, "upd_init failed\n");
        return 1;
    }

    if (strcmp(url, "-") == 0) {
        upd_get(&info);
        check(info.state == UPD_UP_TO_DATE, "empty manifest URL starts as UP_TO_DATE");
        upd_check(true);
        Sleep(300);
        upd_get(&info);
        check(info.state == UPD_UP_TO_DATE, "empty manifest URL stays UP_TO_DATE");
        upd_shutdown();
        printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
    }

    upd_check(true);
    if (!wait_for_state(UPD_AVAILABLE, 90000)) {
        printf("  FAIL: no AVAILABLE state\n");
        failures++;
        upd_shutdown();
        return 1;
    }
    upd_get(&info);
    printf("  latest: %s, notes: %.60s\n", info.latest, info.notes);
    check(strcmp(info.latest, "9.9.9") == 0, "manifest version is 9.9.9");

    /* A non-forced check within six hours must not start another request. */
    upd_check(false);
    Sleep(200);
    upd_get(&info);
    check(info.state == UPD_AVAILABLE, "non-forced re-check is throttled");

    upd_download();
    if (expect_fail) {
        check(wait_for_state(UPD_FAILED, 300000), "download ends in UPD_FAILED");
        check(!file_exists(bin), "update.bin was not left behind");
        check(!file_exists(tmp), "update.bin.tmp was cleaned up");
    } else {
        check(wait_for_state(UPD_READY, 300000), "download ends in UPD_READY");
        check(file_exists(bin), "update.bin exists");
        check(file_exists(json), "update.json exists");
        {
            char actual[65];
            unsigned long long size = 0;
            bool hashed = sha256_file(bin, actual, &size);
            check(hashed, "update.bin can be hashed");
            printf("  sha256: %s (%llu bytes)\n", actual, size);
            if (expect_sha) {
                check(_stricmp(actual, expect_sha) == 0,
                      "downloaded file matches the manifest sha256");
            }
            if (hashed && info.size != 0) {
                check(size == info.size, "downloaded size matches the manifest");
            }
        }
        {
            FILE *f = fopen(json, "rb");
            char text[1024] = {0};
            size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
            if (f) fclose(f);
            check(n > 0, "update.json is readable");
            check(strstr(text, "\"version\"") != NULL, "update.json has a version");
            if (expect_sha) {
                char needle[96];
                snprintf(needle, sizeof needle, "\"%s\"", expect_sha);
                check(strstr(text, needle) != NULL, "update.json has the sha256");
            }
        }
        {
            /* Re-init clears the in-memory manifest: upd_run_installer() must
             * fall back to the update.json metadata written next to the file. */
            char got[64] = {0};
            upd_shutdown();
            check(upd_init("1.0.0", url), "re-init without manifest metadata");
            check(write_all(marker, "9.9.9\n"), "pending-version.txt written");
            check(upd_take_updated_flag(got, sizeof got), "pending flag reported");
            check(strcmp(got, "9.9.9") == 0, "pending flag carries the version");
            check(!file_exists(marker), "pending flag was deleted");
        }
        {
            /* Corrupting update.bin must make upd_run_installer() refuse and
             * delete the file instead of launching a bad installer. */
            check(write_all(bin, "not an installer"), "update.bin corrupted");
            check(!upd_run_installer(), "upd_run_installer rejects a bad hash (update.json fallback)");
            check(!file_exists(bin), "corrupt update.bin was deleted");
        }
    }

    upd_shutdown();
    printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
