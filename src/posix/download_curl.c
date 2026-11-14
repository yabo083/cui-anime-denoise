/* download_curl.c -- POSIX twin of update_io.c's upd__download_to_file.
 *
 * There is no WinHTTP here and the POSIX build carries no HTTP stack at
 * all: the weights fetch is delegated to the system `curl`, which honours
 * http_proxy/https_proxy/no_proxy environment variables, follows
 * redirects (-L) and retries transient failures (--retry 2). The parent
 * polls the destination file for progress (~100 ms cadence, the same
 * interval the WinHTTP loop published at) and kills the child when the
 * embedding executable's stop flag goes up.
 *
 * The tail mirrors the original: delete-on-failure, expected byte count
 * check, then a streaming SHA-256 compare (eng_sha256, shared with the
 * Windows build via models.c).
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "upd_posix.h"
#include "../plat.h"
#include "../sha256.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void set_error(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || !cap) return;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

bool upd__hex_equal(const char *a, const char *b)
{
    size_t i;
    if (!a || !b) return false;
    if (strlen(a) != strlen(b)) return false;
    for (i = 0; a[i]; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'F') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'F') cb += 'a' - 'A';
        if (ca != cb) return false;
    }
    return true;
}

/* Streaming SHA-256 of `path` -- same role as update_io.c's
 * upd__file_sha256_w, minus the wide name. */
static bool file_sha256(const eng_path_t *path, char out[65],
                        unsigned long long *size_out)
{
    FILE *f;
    unsigned char buf[65536];
    eng_sha256_ctx c;
    unsigned long long total = 0;
    int ok = 1;

    out[0] = 0;
    f = fopen(path, "rb");
    if (!f) return false;
    eng_sha256_init(&c);
    for (;;) {
        size_t got = fread(buf, 1, sizeof buf, f);
        if (got) {
            eng_sha256_update(&c, buf, got);
            total += got;
        }
        if (got < sizeof buf) {
            if (ferror(f)) ok = 0;
            break;
        }
    }
    fclose(f);
    if (ok) {
        unsigned char digest[32];
        int i;
        eng_sha256_final(&c, digest);
        for (i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
        out[64] = 0;
    }
    if (ok && size_out) *size_out = total;
    return ok != 0;
}

bool upd__download_to_file(const char *url, const eng_path_t *dest,
                           unsigned long long expected_size,
                           const char *expected_sha256,
                           upd__progress_fn on_progress, void *ctx,
                           char computed[65], unsigned long long *total_out,
                           char *err, size_t errcap)
{
    pid_t pid;
    int status = 0;
    int exited = 0;
    unsigned long long got = 0;

    computed[0] = 0;
    plat_unlink(dest);   /* stale tmp from an earlier crash must not win */

    pid = fork();
    if (pid < 0) {
        set_error(err, errcap, "fork failed (errno %d)", errno);
        return false;
    }
    if (pid == 0) {
        /* child: curl takes the wheel. -f fails on HTTP errors, -L follows
         * redirects, --retry 2 absorbs transient failures, -sS stays quiet
         * but still prints real errors to our stderr. Proxy env vars are
         * honoured by curl itself. */
        execlp("curl", "curl",
               "-fL", "--retry", "2", "--connect-timeout", "30",
               /* mid-transfer stall: die if under 1 KiB/s for 30 s --
                * mirrors the WinHTTP receive timeout in update_io.c */
               "--speed-limit", "1024", "--speed-time", "30",
               "-sS", "-o", dest, url, (char *)NULL);
        _exit(127);
    }

    /* parent: poll the growing file for progress; kill on cancel */
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exited = 1;
            break;
        }
        if (r < 0 && errno != EINTR) {
            set_error(err, errcap, "waitpid failed (errno %d)", errno);
            plat_unlink(dest);
            return false;
        }
        if (upd__stop_requested()) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            plat_unlink(dest);
            set_error(err, errcap, "cancelled");
            return false;
        }
        if (on_progress) {
            unsigned long long sz = 0;
            if (plat_file_size(dest, &sz)) {
                on_progress(ctx, sz, expected_size);
            }
        }
        plat_sleep_ms(100);
    }

    if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        plat_unlink(dest);
        if (exited && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            set_error(err, errcap, "curl not found: install curl and retry");
        } else if (exited && WIFEXITED(status)) {
            set_error(err, errcap, "curl failed (exit %d)",
                      WEXITSTATUS(status));
        } else {
            set_error(err, errcap, "curl terminated abnormally");
        }
        return false;
    }

    /* same post-conditions as the WinHTTP loop: byte count, then digest */
    if (expected_size != 0) {
        if (!plat_file_size(dest, &got) || got != expected_size) {
            unsigned long long sz = 0;
            plat_file_size(dest, &sz);
            plat_unlink(dest);
            set_error(err, errcap,
                      "size mismatch: got %llu bytes, expected %llu",
                      sz, expected_size);
            return false;
        }
    }
    if (!file_sha256(dest, computed, &got)) {
        plat_unlink(dest);
        set_error(err, errcap, "cannot hash the downloaded file");
        return false;
    }
    if (!upd__hex_equal(expected_sha256, computed)) {
        plat_unlink(dest);
        set_error(err, errcap, "SHA-256 mismatch");
        return false;
    }
    if (on_progress) on_progress(ctx, got, expected_size ? expected_size : got);
    if (total_out) *total_out = got;
    return true;
}
