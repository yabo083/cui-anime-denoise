/* test_models.c -- the model-pair logic in src/engine/models.c, exercised
 * without a network. The download is only "done" when the bundled skeleton
 * sits next to the .pth in the same directory; these checks pin down the
 * states the settings surface renders:
 *
 *   - bundled ESRGAN is a plain file check on models_dir;
 *   - a downloadable model needs the .pth + .skel.onnx co-located -- either
 *     half alone (or both halves in different directories) is NOT available;
 *   - a "download" on weights that already verify skips the network and
 *     completes the pair in place (the interrupted-download repair);
 *   - a fresh download lands the .pth AND copies the skeleton;
 *   - pair completion refuses cleanly when the bundled skeleton is gone;
 *   - delete removes the pair from the user dir and flips status back.
 *
 * The four helpers models.c borrows from the updater / CUI are stubbed at
 * the bottom -- upd__download_to_file is a flagged fake so a check can prove
 * the network never ran.
 *
 * Build (from the repo root):
 *   pwsh -NoProfile -Command ". .\toolchain.ps1 -Quiet;
 *     New-Item -ItemType Directory -Force tmp\modelbuild | Out-Null;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /c tests\test_models.c /Fo:tmp\modelbuild\;
 *     cl /nologo /utf-8 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /I src /c src\engine\models.c /Fo:tmp\modelbuild\;
 *     link /nologo tmp\modelbuild\test_models.obj tmp\modelbuild\models.obj
 *       bcrypt.lib /OUT:tmp\modelbuild\test_models.exe"
 *   tmp\modelbuild\test_models.exe
 */
#include "engine/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int fails, checks;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            fails++;                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, msg);             \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ stubs */
/* models.c borrows these; the test provides them so nothing here is linked
 * against update_io.c, CUI or the network. */

bool upd__to_wide(const char *s, wchar_t *out, size_t cap) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out,
                                (int)cap);
    return n > 0;
}

bool upd__hex_equal(const char *a, const char *b) {
    return a && b && _stricmp(a, b) == 0;
}

size_t cui__inflate(const void *src, size_t src_len, void *dst,
                    size_t dst_cap) {
    (void)src; (void)src_len; (void)dst; (void)dst_cap;
    return 0;                       /* the zip path is not exercised here */
}

static int dl_calls;   /* how many times the fake network ran */
static int dl_fail;    /* when set, the fake network reports an error */

typedef void (*upd__progress_fn)(void *ctx, unsigned long long done,
                                 unsigned long long total);

/* Writes a fake download of `expected_size` bytes (PK zip magic, like a real
 * .pth), answers the pinned hash, and records that it ran. */
bool upd__download_to_file(const char *url, const wchar_t *dest,
                           unsigned long long expected_size,
                           const char *expected_sha256,
                           upd__progress_fn on_progress, void *ctx,
                           char computed[65], unsigned long long *total_out,
                           char *err, size_t errcap) {
    HANDLE f;
    DWORD wrote = 0;
    unsigned char *buf;
    (void)url;
    dl_calls++;
    if (dl_fail) {
        snprintf(err, errcap, "fake network failure");
        return false;
    }
    buf = (unsigned char *)calloc(1, (size_t)expected_size);
    if (!buf) return false;
    buf[0] = 'P'; buf[1] = 'K'; buf[2] = 3; buf[3] = 4;
    f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE ||
        !WriteFile(f, buf, (DWORD)expected_size, &wrote, NULL) ||
        wrote != expected_size) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        free(buf);
        snprintf(err, errcap, "fake download cannot write");
        return false;
    }
    CloseHandle(f);
    free(buf);
    strcpy(computed, expected_sha256);
    if (total_out) *total_out = expected_size;
    if (on_progress) on_progress(ctx, expected_size, expected_size);
    return true;
}

/* ---------------------------------------------------------------- helpers */

static wchar_t BUNDLED[260];   /* models_dir   */
static wchar_t USERDIR[260];   /* user_models_dir */
static wchar_t SCRATCH[260];   /* the tree both live under */

static void make_dirs(void) {
    wchar_t base[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, base, MAX_PATH);
    wchar_t *end;
    if (!n) exit(2);
    end = base + n;
    while (end > base && end[-1] != L'\\' && end[-1] != L'/') end--;
    *end = 0;
    swprintf(SCRATCH, 260, L"%smodeltest", base);
    swprintf(BUNDLED, 260, L"%s\\bundled", SCRATCH);
    swprintf(USERDIR, 260, L"%s\\user", SCRATCH);
    CreateDirectoryW(SCRATCH, NULL);
    CreateDirectoryW(BUNDLED, NULL);
    CreateDirectoryW(USERDIR, NULL);
}

static void wipe_dir(const wchar_t *dir) {
    wchar_t pat[300];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, 300, L"%s\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t p[300];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        swprintf(p, 300, L"%s\\%s", dir, fd.cFileName);
        DeleteFileW(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void reset_dirs(void) {
    wipe_dir(USERDIR);
    wipe_dir(BUNDLED);
}

static int write_file(const wchar_t *dir, const char *name,
                      unsigned long long size, int pk_magic) {
    wchar_t path[300];
    wchar_t wname[260];
    HANDLE f;
    unsigned char *buf;
    DWORD wrote = 0;
    int ok;
    if (!upd__to_wide(name, wname, 260)) return 0;
    swprintf(path, 300, L"%s\\%s", dir, wname);
    buf = (unsigned char *)calloc(1, (size_t)size ? (size_t)size : 1);
    if (!buf) return 0;
    if (pk_magic) { buf[0] = 'P'; buf[1] = 'K'; buf[2] = 3; buf[3] = 4; }
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    ok = f != INVALID_HANDLE_VALUE &&
         WriteFile(f, buf, (DWORD)size, &wrote, NULL) && wrote == size;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(buf);
    return ok;
}

static int exists_in(const wchar_t *dir, const char *name) {
    wchar_t path[300];
    wchar_t wname[260];
    if (!upd__to_wide(name, wname, 260)) return 0;
    swprintf(path, 300, L"%s\\%s", dir, wname);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static eng_config cfg(void) {
    eng_config c;
    memset(&c, 0, sizeof c);
    c.models_dir = BUNDLED;
    c.user_models_dir = USERDIR;
    return c;
}

/* the APISR registry values the test fixtures have to match */
#define APISR_PTH  "2x_APISR_RRDB_GAN_generator.pth"
#define APISR_SKEL "apisr_rrdb_x2.skel.onnx"
#define APISR_SIZE 17960938ull
#define CUGAN_PTH  "up2x-latest-denoise3x.pth"
#define CUGAN_SKEL "cugan_up2x_denoise3x.skel.onnx"
#define CUGAN_SIZE 5147249ull

/* ------------------------------------------------------------------ tests */

static void test_esrgan_status(void) {
    eng_model_state ms;
    eng_config c = cfg();

    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_ESRGAN, &ms);
    CHECK(!ms.available, "ESRGAN absent from models_dir is unavailable");
    CHECK(strstr(ms.missing, "esrgan_anime6b_x4.onnx") != NULL,
          "ESRGAN missing names the bundled file");

    CHECK(write_file(BUNDLED, "esrgan_anime6b_x4.onnx", 16, 0), "seed esrgan");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_ESRGAN, &ms);
    CHECK(ms.available, "ESRGAN present is available");
}

static void test_pair_states(void) {
    eng_model_state ms;
    eng_config c = cfg();
    char err[256];

    reset_dirs();
    CHECK(write_file(BUNDLED, APISR_SKEL, 16, 0), "seed bundled skel");

    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(!ms.available, "nothing downloaded -> unavailable");
    CHECK(!ms.weights_present, "weights absent reported");
    CHECK(ms.skeleton_present, "bundled skeleton reported present");

    /* the original bug state: weights downloaded, skeleton left behind in
     * the bundled dir -- both halves exist yet nothing is usable */
    CHECK(write_file(USERDIR, APISR_PTH, APISR_SIZE, 1), "seed weights only");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(!ms.available, "split pair is unavailable");
    CHECK(strstr(ms.missing, "co-located") != NULL,
          "split pair says so, not 'not downloaded'");

    /* the repair: pair_complete copies the bundled skeleton over */
    err[0] = 0;
    CHECK(eng_model_pair_complete(&c, ENG_M_APISR, err, sizeof err),
          "pair_complete repairs the split pair");
    CHECK(exists_in(USERDIR, APISR_SKEL), "skeleton landed next to weights");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(ms.available, "co-located pair is available");

    /* pair in the bundled dir itself also counts (dropped in by hand) */
    reset_dirs();
    CHECK(write_file(BUNDLED, APISR_SKEL, 16, 0), "seed bundled skel");
    CHECK(write_file(BUNDLED, APISR_PTH, APISR_SIZE, 1), "weights in bundled");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(ms.available, "pair inside models_dir is available");

    /* pair_complete with no weights refuses without touching anything */
    reset_dirs();
    err[0] = 0;
    CHECK(!eng_model_pair_complete(&c, ENG_M_APISR, err, sizeof err),
          "pair_complete without weights fails");
    CHECK(strstr(err, "weights not downloaded") != NULL,
          "pair_complete error says why");
}

static void test_download_paths(void) {
    eng_config c = cfg();
    eng_model_state ms;
    char err[256];

    /* a complete-on-disk .pth skips the fetch entirely: the network stub
     * must not run, and the pair must come out complete anyway */
    reset_dirs();
    CHECK(write_file(BUNDLED, APISR_SKEL, 16, 0), "seed bundled skel");
    CHECK(write_file(USERDIR, APISR_PTH, APISR_SIZE, 1), "seed finished .pth");
    dl_calls = 0;
    err[0] = 0;
    CHECK(eng_model_download(&c, ENG_M_APISR, NULL, NULL, NULL,
                             err, sizeof err) == 0,
          "download on finished weights succeeds");
    CHECK(dl_calls == 0, "finished weights skip the network");
    CHECK(exists_in(USERDIR, APISR_SKEL), "repair copied the skeleton");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(ms.available, "status flips to available after repair");

    /* a partial/corrupt .pth (right magic absent, wrong size) does NOT take
     * the shortcut: the fetch runs, lands, and the pair completes */
    reset_dirs();
    CHECK(write_file(BUNDLED, APISR_SKEL, 16, 0), "seed bundled skel");
    CHECK(write_file(USERDIR, APISR_PTH, 64, 0), "seed truncated .pth");
    dl_calls = 0;
    err[0] = 0;
    CHECK(eng_model_download(&c, ENG_M_APISR, NULL, NULL, NULL,
                             err, sizeof err) == 0,
          "download over a partial file succeeds");
    CHECK(dl_calls == 1, "partial weights go through the network");
    CHECK(exists_in(USERDIR, APISR_SKEL), "fresh download pairs the skeleton");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(ms.available, "status flips to available after download");

    /* the bundled skeleton gone: a fresh download must fail clearly and
     * roll the .pth back out -- no broken half-state left behind */
    reset_dirs();
    dl_calls = 0;
    err[0] = 0;
    CHECK(eng_model_download(&c, ENG_M_APISR, NULL, NULL, NULL,
                             err, sizeof err) != 0,
          "download without bundled skeleton fails");
    CHECK(strstr(err, "skeleton") != NULL, "error names the skeleton");
    CHECK(!exists_in(USERDIR, APISR_PTH), "weights rolled back on failure");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_APISR, &ms);
    CHECK(!ms.available, "status stays unavailable after the failure");

    /* cancel flag pre-set: nothing runs, answer is 1 */
    {
        volatile long cancel = 1;
        dl_calls = 0;
        CHECK(eng_model_download(&c, ENG_M_APISR, NULL, NULL, &cancel,
                                 err, sizeof err) == 1,
              "pre-set cancel answers 1");
        CHECK(dl_calls == 0, "cancel runs no network");
    }
}

static void test_delete(void) {
    eng_config c = cfg();
    eng_model_state ms;

    reset_dirs();
    CHECK(write_file(BUNDLED, CUGAN_SKEL, 16, 0), "seed bundled skel");
    CHECK(write_file(USERDIR, CUGAN_PTH, CUGAN_SIZE, 1), "seed weights");
    CHECK(eng_model_pair_complete(&c, ENG_M_CUGAN, NULL, 0),
          "cugan pair completed");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_CUGAN, &ms);
    CHECK(ms.available, "cugan pair is available");

    CHECK(eng_model_delete(&c, ENG_M_CUGAN), "delete removes the pair");
    CHECK(!exists_in(USERDIR, CUGAN_PTH), "weights gone");
    CHECK(!exists_in(USERDIR, CUGAN_SKEL), "copied skeleton gone");
    memset(&ms, 0, sizeof ms);
    eng_model_status(&c, ENG_M_CUGAN, &ms);
    CHECK(!ms.available, "status flips back to not-downloaded");
    CHECK(!eng_model_delete(&c, ENG_M_CUGAN),
          "deleting nothing reports it");
    /* the bundled skeleton must not be deleted -- only the user-dir copy */
    CHECK(exists_in(BUNDLED, CUGAN_SKEL), "bundled skeleton untouched");
}

int main(void) {
    make_dirs();
    test_esrgan_status();
    test_pair_states();
    test_download_paths();
    test_delete();
    wipe_dir(USERDIR);
    wipe_dir(BUNDLED);
    printf("%s: %d checks, %d failures\n",
           fails ? "FAIL" : "OK", checks, fails);
    return fails ? 1 : 0;
}
