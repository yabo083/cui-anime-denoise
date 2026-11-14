/* upd_posix.h -- the POSIX counterpart of the update_io.c internals that
 * models.c borrows. On Windows these come from update_priv.h (WinHTTP +
 * BCrypt + wide paths); the POSIX build has no updater, so the two helpers
 * models.c actually uses live here, implemented in download_curl.c.
 *
 * Same names (upd__ prefix) so models.c keeps one call site; the only
 * difference is eng_path_t (char) where update_priv.h had wchar_t.
 */
#ifndef ANIME_UPD_POSIX_H
#define ANIME_UPD_POSIX_H

#include "../engine/engine.h"

#include <stdbool.h>
#include <stddef.h>

/* Called from the transfer loop at most once per ~100 ms, plus once on
 * completion -- same contract as update_io.c. */
typedef void (*upd__progress_fn)(void *ctx, unsigned long long done,
                                 unsigned long long total);

/* Case-insensitive hex equality (digest vs digest). */
bool upd__hex_equal(const char *a, const char *b);

/* Provided by the embedding executable (the CLI's Ctrl-C flag). */
bool upd__stop_requested(void);

/* Downloads `url` into `dest` via a curl child process, then checks the
 * byte count and SHA-256 exactly like the WinHTTP original: on any
 * failure `dest` is deleted and false is returned with `err` set; the
 * verified file stays on success. */
bool upd__download_to_file(const char *url, const eng_path_t *dest,
                           unsigned long long expected_size,
                           const char *expected_sha256,
                           upd__progress_fn on_progress, void *ctx,
                           char computed[65], unsigned long long *total_out,
                           char *err, size_t errcap);

#endif /* ANIME_UPD_POSIX_H */
