/* update_priv.h -- internals of the updater, shared by update.c (state
 * machine, worker thread, public upd_* API) and update_io.c (WinHTTP, JSON,
 * SHA-256, file and path helpers).
 *
 * Nothing here is part of the public surface: callers outside these two
 * translation units keep including update.h only.
 *
 * Every cross-file internal symbol carries the upd__ prefix so a stray
 * non-static definition stands out next to the public upd_* names.
 */
#ifndef CUI_ANIME_UPDATE_PRIV_H
#define CUI_ANIME_UPDATE_PRIV_H

#include "config.h"
#include "update.h"

#include <stdbool.h>
#include <stddef.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/* ---------------------------------------------------------------- limits */

#define UPD_TIMEOUT_MS       20000u
#define UPD_MANIFEST_MAX     (1024u * 1024u)   /* 1 MiB */
#define UPD_IO_CHUNK         65536u
#define UPD_PATH_CAP         CFG_PATH_CAP
#define UPD_WPATH_CAP        (CFG_PATH_CAP * 2)

/* --------------------------------------------------------------- manifest */

typedef struct upd_manifest {
    char version[32];
    char url[1024];
    char sha256[80];
    char notes[4096];
    char page[512];
    unsigned long long size;
} upd_manifest;

/* --------------------------------------------- update.c: cancellation flag */

/* Set by upd_shutdown(); polled by the long IO loops so shutdown does not
 * have to wait for a stalled transfer. */
bool upd__stop_requested(void);

/* ---------------------------------- update_io.c: paths, files and digests */

bool upd__to_wide(const char *s, wchar_t *out, size_t cap);

/* Builds <data>\updates\<name>; with a NULL/empty `name`, the directory. */
bool upd__updates_path(char *out, size_t cap, const char *name);
bool upd__ensure_updates_dir(void);

bool upd__read_text_w(const wchar_t *path, char **out, size_t *out_len,
                      size_t max);
bool upd__write_text_w(const wchar_t *path, const char *utf8);

bool upd__file_sha256_w(const wchar_t *path, char out[65],
                        unsigned long long *size_out);
bool upd__hex_equal(const char *a, const char *b);
bool upd__sha256_looks_valid(const char *s);
void upd__sha256_lower(char *s);

/* --------------------------------------------------- update_io.c: JSON/HTTP */

/* Parses the six manifest fields out of a flat JSON object. Unknown keys are
 * skipped, so extra metadata never breaks an older client. */
bool upd__parse_manifest(const char *json, size_t len, upd_manifest *m,
                         char *err, size_t errcap);

/* GETs `url` and parses the body as a manifest. */
bool upd__fetch_manifest(const char *url, upd_manifest *m, char *err,
                         size_t errcap);

/* Called from the transfer loop at most once per 100 ms, plus once on the
 * final chunk; `ctx` is whatever the caller passed. */
typedef void (*upd__progress_fn)(void *ctx, unsigned long long done,
                                 unsigned long long total);

/* Streams `url` into `dest` (replacing it), hashing as it goes, then checks
 * the byte count and the digest. On any failure `dest` is deleted and false
 * is returned with `err` set; the verified file stays on success. */
bool upd__download_to_file(const char *url, const wchar_t *dest,
                           unsigned long long expected_size,
                           const char *expected_sha256,
                           upd__progress_fn on_progress, void *ctx,
                           char computed[65], unsigned long long *total_out,
                           char *err, size_t errcap);

#endif /* CUI_ANIME_UPDATE_PRIV_H */
