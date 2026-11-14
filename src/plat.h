/* plat.h -- the platform seam between the engine/CLI and the OS.
 *
 * The engine and the console front end were born on Win32: paths are
 * wchar_t, threads are HANDLEs, cancel flags are Interlocked longs, the
 * clock is GetTickCount64. This header declares one small API so that
 * engine.c / l0.c / models.c / the CLI keep a single code path while the
 * implementation splits two ways:
 *
 *   src/plat_win.c        -- the existing Win32 semantics, verbatim
 *   src/posix/plat_posix.c -- pthreads, UTF-8 paths, dlopen-era POSIX
 *
 * `eng_path_t` (engine.h) is wchar_t on _WIN32 and char elsewhere, so the
 * same signature compiles on both sides. Everything is C11, no C++.
 *
 * Conventions:
 *   - paths passed to plat_* are already in the platform encoding
 *     (UTF-16 on Windows, UTF-8 on POSIX) -- the *_u8 helpers convert;
 *   - file functions return bool success like the Win32 calls they
 *     replaced (failure never throws, never aborts);
 *   - plat_rename REPLACES the destination (MoveFileEx semantics), which
 *     is what rename() does for free on POSIX;
 *   - cancel flags are `volatile long`: readers poll *p, writers use
 *     plat_atomic_set.
 */
#ifndef ANIME_PLAT_H
#define ANIME_PLAT_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/engine.h"

#ifdef _WIN32
#include <wchar.h>
#endif

/* String literal in the platform path type: L"x" on Windows, "x" on
 * POSIX. For model file names and other ASCII paths. */
#ifdef _WIN32
#define PLAT_LIT(x) L##x
#define PLAT_SEP L'\\'
#define PLAT_SEP_S L"\\"
#else
#define PLAT_LIT(x) x
#define PLAT_SEP '/'
#define PLAT_SEP_S "/"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Paths                                                                      */
/* ------------------------------------------------------------------------- */

/* malloc'ed dir + separator + name; NULL on alloc failure (or, on Windows,
 * when name_u8 is not convertible UTF-8). */
eng_path_t *plat_join_path(const eng_path_t *dir, const eng_path_t *name);
eng_path_t *plat_join_u8(const eng_path_t *dir, const char *name_u8);

/* wcsncpy/strncpy by another name: copies up to n chars; like the CRT
 * original it pads with zeros when src is shorter. Callers pre-zero or
 * pass cap-1, matching the code this replaced. */
void plat_sncpy(eng_path_t *dst, const eng_path_t *src, size_t n);
size_t plat_plen(const eng_path_t *s);

/* eng_path_t -> UTF-8 for error messages (%s). Returns false on
 * truncation/conversion failure; out is still NUL-terminated. */
bool plat_path_to_u8(const eng_path_t *p, char *out, size_t cap);

/* UTF-8 -> freshly malloc'ed eng_path_t (MultiByteToWideChar / strdup;
 * NULL on failure or unconvertible input). The CLI needs this for argv
 * and user-supplied --models paths, which arrive as UTF-8 bytes. */
eng_path_t *plat_u8_to_path(const char *s);

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */

bool plat_file_exists(const eng_path_t *p);
bool plat_is_dir(const eng_path_t *p);
bool plat_file_size(const eng_path_t *p, unsigned long long *out);

/* Reads exactly n bytes from the start of the file; false when the file
 * is missing or shorter than n. */
bool plat_read_head(const eng_path_t *p, void *buf, size_t n);

/* Whole file into a fresh malloc'ed buffer (caller frees). Refuses files
 * over 2 GiB like the Win32 reader it replaced. NULL on failure. */
unsigned char *plat_read_all(const eng_path_t *p, size_t *out_len);

/* Writes `len` bytes, truncating/creating the file. false on any error;
 * a partial file may remain (callers unlink on failure, as before). */
bool plat_write_all(const eng_path_t *p, const void *data, size_t len);

bool plat_copy_file(const eng_path_t *src, const eng_path_t *dst);
bool plat_rename(const eng_path_t *src, const eng_path_t *dst);
bool plat_unlink(const eng_path_t *p);   /* true when something was removed */

/* One directory level: true when the directory exists afterwards
 * (EEXIST/ERROR_ALREADY_EXISTS count as success, like the ignored
 * CreateDirectoryW call this replaces). */
bool plat_mkdir(const eng_path_t *p);

/* Directory of the running executable, with a trailing separator --
 * GetModuleFileNameW-stem on Windows, /proc/self/exe on POSIX. malloc'ed;
 * NULL on failure. */
eng_path_t *plat_exe_dir(void);

/* ------------------------------------------------------------------------- */
/* Directory listing (the CLI's folder-input scan)                            */
/* ------------------------------------------------------------------------- */

typedef struct plat_dir plat_dir;

/* opendir/FindFirstFile; NULL when the path is not a readable directory. */
plat_dir *plat_dir_open(const eng_path_t *dir);

/* Next regular file inside the directory (never "." / ".." / subdirs), as
 * a malloc'ed eng_path_t basename -- caller frees. NULL ends the walk. */
eng_path_t *plat_dir_next_file(plat_dir *d);

void plat_dir_close(plat_dir *d);

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

/* _stricmp / _strnicmp on Windows; strcasecmp/strncasecmp on POSIX. */
int plat_stricmp(const char *a, const char *b);
int plat_strnicmp(const char *a, const char *b, size_t n);

/* ------------------------------------------------------------------------- */
/* Atomics / time                                                             */
/* ------------------------------------------------------------------------- */

/* Store `v` into a `volatile long` cancel flag (InterlockedExchange /
 * __atomic_exchange_n) and read one back. */
long plat_atomic_set(volatile long *p, long v);
long plat_atomic_get(volatile long *p);

unsigned long long plat_tick_ms(void);   /* GetTickCount64 / CLOCK_MONOTONIC */
void plat_sleep_ms(unsigned ms);         /* Sleep / nanosleep */

/* Logical CPU count for default thread pools (>= 1). */
int plat_num_cpus(void);

/* ------------------------------------------------------------------------- */
/* Threads                                                                    */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
typedef void *plat_thread;   /* HANDLE */
#else
#include <pthread.h>
typedef pthread_t plat_thread;
#endif

typedef void (*plat_thread_fn)(void *arg);

/* Starts fn(arg) on a new thread. 0 on success; the thread runs
 * plat_thread_below_normal() itself first (matching the CreateThread
 * workers this replaces). */
int plat_thread_spawn(plat_thread *out, plat_thread_fn fn, void *arg);
void plat_thread_join(plat_thread *t);

/* Drops the calling thread to below-normal scheduling. Best effort:
 * Windows uses THREAD_PRIORITY_BELOW_NORMAL; POSIX raises its nice
 * level and quietly does nothing when that is not permitted. */
void plat_thread_below_normal(void);

/* ------------------------------------------------------------------------- */
/* Dynamic loading (engine.c's ORT probe)                                     */
/* ------------------------------------------------------------------------- */

void *plat_dlopen(const eng_path_t *path);   /* full path; NULL on failure */
void *plat_dlsym(void *lib, const char *name);
void plat_dlclose(void *lib);

#ifdef __cplusplus
}
#endif
#endif /* ANIME_PLAT_H */
