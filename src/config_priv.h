/* Internals shared by the two halves of the config module.
 *
 * config.c owns settings.json, the data directory tree and the cfg_* API;
 * log.c owns the rotating log, the last-chance crash handler and the
 * diagnostics pack. config.h is the only header other modules include -- this
 * one is private to those two files.
 *
 * Symbols that cross the split carry the cfg__ prefix (defined by config.c and
 * used by log.c) or the log__ prefix (defined by log.c); everything else stays
 * static in its own file.
 */
#ifndef CONFIG_PRIV_H
#define CONFIG_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include "config.h"

#define CFG_WCHAR_CAP 1024
#define LOG_ROTATE_BYTES (4u * 1024u * 1024u)
#define LOG_MSG_CAP 3600
#define LOG_LINE_CAP 4096
#define SETTINGS_MAX_BYTES (1024u * 1024u)
#define DIAG_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define DIAG_MAX_ENTRIES 64
#define CRASH_REPORT_CAP (96u * 1024u)
#define CRASH_LOG_TAIL_BYTES (64u * 1024u)

/* ---- state resolved by cfg_init and read by the log half ---- */
extern bool cfg__ready;
extern wchar_t cfg__log_dir_w[CFG_WCHAR_CAP];
extern wchar_t cfg__settings_path_w[CFG_WCHAR_CAP];
extern char cfg__data_u8[CFG_PATH_CAP];

/* ---- growable string buffer (settings JSON, system info) ---- */
typedef struct cfg__sb {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
} cfg__sb;

void cfg__sb_adds(cfg__sb *b, const char *s);
void cfg__sb_addf(cfg__sb *b, const char *fmt, ...);
void cfg__sb_free(cfg__sb *b);

/* ---- path and file helpers ---- */
bool cfg__to_utf8(const wchar_t *w, char *out, size_t cap);
bool cfg__path_join(wchar_t *out, size_t cap, const wchar_t *base,
                    const wchar_t *leaf);
bool cfg__read_file_w(const wchar_t *path, char **out, size_t *out_len,
                      size_t max_bytes);
bool cfg__write_file_w(const wchar_t *path, const void *data, size_t len);

/* Replaces the user profile prefix (resolved by cfg_init) with a neutral
 * marker. Returns a malloc'd copy, NULL on OOM. Used by the diagnostics pack. */
char *cfg__redact_alloc(const char *data, size_t len, size_t *out_len);

#endif /* CONFIG_PRIV_H */
