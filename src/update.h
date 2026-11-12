/* Self-update: fetch a JSON manifest over HTTPS, compare versions, download
 * the Inno Setup installer, verify its SHA-256, then hand over to it and quit.
 *
 * Manifest (served from a static URL, or a GitHub release asset):
 *   { "version": "1.2.0",
 *     "url": "https://.../AnimeDenoise-1.2.0-setup.exe",
 *     "sha256": "9f2c...",
 *     "size": 24000000,
 *     "notes": "UTF-8 release notes",
 *     "page": "https://github.com/..." }
 *
 * Everything network related runs on a worker thread; the UI polls
 * upd_get(). Downloaded bytes land in <data>\updates\update.bin and the
 * metadata last (update.json), so a half-finished download is never mistaken
 * for a complete one.
 */
#ifndef UPDATE_H
#define UPDATE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum upd_state {
    UPD_IDLE = 0,
    UPD_CHECKING,
    UPD_UP_TO_DATE,
    UPD_AVAILABLE,
    UPD_DOWNLOADING,
    UPD_READY,        /* installer downloaded and verified */
    UPD_FAILED
} upd_state;

typedef struct upd_info {
    upd_state state;
    char current[32];
    char latest[32];
    char notes[4096];
    char page[512];
    char error[512];
    float progress;              /* 0..1 while downloading */
    unsigned long long size;     /* installer bytes */
} upd_info;

/* `manifest_url` may be empty, which disables the feature. Call once at
 * startup, after cfg_init. `current` is the running version string. */
bool upd_init(const char *current, const char *manifest_url);
void upd_shutdown(void);

/* Starts a check when idle; safe to call every frame. `force` bypasses the
 * 6-hour minimum interval. */
void upd_check(bool force);
void upd_download(void);

/* Snapshot of the state, for the UI. */
void upd_get(upd_info *out);

/* Runs the downloaded installer with Inno's silent flags and returns; the
 * caller must then quit the app. False when nothing usable is on disk. */
bool upd_run_installer(void);

/* True when a previous session launched an update: the app should show a
 * "updated to <version>" toast and clear the marker. */
bool upd_take_updated_flag(char *version, size_t cap);

#endif /* UPDATE_H */
