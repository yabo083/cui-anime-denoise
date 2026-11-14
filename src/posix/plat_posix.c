/* plat_posix.c -- the POSIX half of plat.h (Linux x86_64, gcc): UTF-8 char
 * paths, pthreads, dlopen, CLOCK_MONOTONIC. Behaviour mirrors the Win32
 * originals in plat_win.c; where POSIX has no exact analogue
 * (below-normal priority) the function degrades gracefully.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "../plat.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Paths                                                                      */
/* ------------------------------------------------------------------------- */

eng_path_t *plat_join_path(const eng_path_t *dir, const eng_path_t *name)
{
    size_t dl, nl;
    eng_path_t *p;
    if (!dir || !name) return NULL;
    dl = strlen(dir);
    nl = strlen(name);
    p = (eng_path_t *)malloc(dl + nl + 2);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    if (dl && p[dl - 1] != '/' && p[dl - 1] != '\\') {
        p[dl++] = '/';
    }
    memcpy(p + dl, name, nl + 1);
    return p;
}

eng_path_t *plat_join_u8(const eng_path_t *dir, const char *name_u8)
{
    /* POSIX paths are UTF-8 already -- no conversion hop. */
    return plat_join_path(dir, name_u8);
}

void plat_sncpy(eng_path_t *dst, const eng_path_t *src, size_t n)
{
    strncpy(dst, src, n);
}

size_t plat_plen(const eng_path_t *s)
{
    return strlen(s);
}

bool plat_path_to_u8(const eng_path_t *p, char *out, size_t cap)
{
    size_t n;
    if (!out || !cap) return false;
    out[0] = 0;
    if (!p) return false;
    n = strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return strlen(p) < cap;
}

eng_path_t *plat_u8_to_path(const char *s)
{
    /* POSIX paths are UTF-8 already -- no conversion hop. */
    return s ? strdup(s) : NULL;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */

bool plat_file_exists(const eng_path_t *p)
{
    struct stat st;
    return p && stat(p, &st) == 0;
}

bool plat_is_dir(const eng_path_t *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

bool plat_file_size(const eng_path_t *p, unsigned long long *out)
{
    struct stat st;
    if (!p || stat(p, &st) != 0) return false;
    if (out) *out = (unsigned long long)st.st_size;
    return true;
}

bool plat_read_head(const eng_path_t *p, void *buf, size_t n)
{
    int fd;
    size_t off = 0;
    if (!p) return false;
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    while (off < n) {
        ssize_t got = read(fd, (char *)buf + off, n - off);
        if (got <= 0) {
            close(fd);
            return false;
        }
        off += (size_t)got;
    }
    close(fd);
    return true;
}

unsigned char *plat_read_all(const eng_path_t *p, size_t *out_len)
{
    int fd;
    struct stat st;
    unsigned char *buf;
    size_t off = 0, len;

    if (!p) return NULL;
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size > (off_t)0x7fffffff) {
        close(fd);
        return NULL;
    }
    len = (size_t)st.st_size;
    buf = (unsigned char *)malloc(len ? len : 1);
    if (!buf) {
        close(fd);
        return NULL;
    }
    while (off < len) {
        ssize_t got = read(fd, buf + off, len - off);
        if (got <= 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        off += (size_t)got;
    }
    close(fd);
    *out_len = off;
    return buf;
}

bool plat_write_all(const eng_path_t *p, const void *data, size_t len)
{
    int fd;
    size_t off = 0;
    int ok = 1;
    if (!p) return false;
    fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    while (off < len) {
        ssize_t wrote = write(fd, (const char *)data + off, len - off);
        if (wrote <= 0) {
            ok = 0;
            break;
        }
        off += (size_t)wrote;
    }
    if (close(fd) != 0) ok = 0;
    return ok != 0;
}

bool plat_copy_file(const eng_path_t *src, const eng_path_t *dst)
{
    int in, out;
    unsigned char buf[1 << 16];
    int ok = 1;
    if (!src || !dst) return false;
    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (out < 0) {
        close(in);
        return false;
    }
    for (;;) {
        ssize_t got = read(in, buf, sizeof buf);
        size_t off = 0;
        if (got < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        if (got == 0) break;
        while (off < (size_t)got) {
            ssize_t wrote = write(out, buf + off, (size_t)got - off);
            if (wrote <= 0) {
                ok = 0;
                break;
            }
            off += (size_t)wrote;
        }
        if (!ok) break;
    }
    if (close(out) != 0) ok = 0;
    close(in);
    return ok != 0;
}

bool plat_rename(const eng_path_t *src, const eng_path_t *dst)
{
    /* rename(2) replaces dst atomically -- the MoveFileEx semantics the
     * callers were getting for free on Windows. */
    return src && dst && rename(src, dst) == 0;
}

bool plat_unlink(const eng_path_t *p)
{
    return p && unlink(p) == 0;
}

bool plat_mkdir(const eng_path_t *p)
{
    struct stat st;
    if (!p) return false;
    if (mkdir(p, 0755) == 0) return true;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

eng_path_t *plat_exe_dir(void)
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *end;
    if (n <= 0) return NULL;
    buf[n] = 0;
    end = buf + n;
    /* strip the basename, keep the trailing '/' (callers join directly) */
    while (end > buf && end[-1] != '/') end--;
    *end = 0;
    return strdup(buf);
}

/* ------------------------------------------------------------------------- */
/* Directory listing                                                          */
/* ------------------------------------------------------------------------- */

struct plat_dir {
    DIR *d;
    char *dir;   /* kept so d_type-less filesystems can stat */
};

plat_dir *plat_dir_open(const eng_path_t *dir)
{
    plat_dir *pd;
    if (!dir) return NULL;
    pd = (plat_dir *)calloc(1, sizeof *pd);
    if (!pd) return NULL;
    pd->d = opendir(dir);
    if (!pd->d) {
        free(pd);
        return NULL;
    }
    pd->dir = strdup(dir);
    if (!pd->dir) {
        closedir(pd->d);
        free(pd);
        return NULL;
    }
    return pd;
}

eng_path_t *plat_dir_next_file(plat_dir *pd)
{
    struct dirent *e;
    if (!pd || !pd->d) return NULL;
    while ((e = readdir(pd->d)) != NULL) {
        char *name;
        if (e->d_name[0] == '.' &&
            (e->d_name[1] == 0 ||
             (e->d_name[1] == '.' && e->d_name[2] == 0))) {
            continue;   /* "." and ".." */
        }
        name = strdup(e->d_name);
        if (!name) return NULL;
#ifdef DT_REG
        if (e->d_type == DT_REG) {
            return name;   /* fast path: regular file, no stat needed */
        }
        if (e->d_type == DT_DIR) {
            free(name);
            continue;
        }
#endif
        /* d_type unknown/unsupported: stat <dir>/<name> */
        {
            char *full = plat_join_path(pd->dir, name);
            struct stat st;
            int is_reg = full && stat(full, &st) == 0 &&
                         S_ISREG(st.st_mode);
            free(full);
            if (is_reg) return name;
            free(name);
        }
    }
    return NULL;
}

void plat_dir_close(plat_dir *pd)
{
    if (!pd) return;
    if (pd->d) closedir(pd->d);
    free(pd->dir);
    free(pd);
}

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

int plat_stricmp(const char *a, const char *b)
{
    return strcasecmp(a, b);
}

int plat_strnicmp(const char *a, const char *b, size_t n)
{
    return strncasecmp(a, b, n);
}

/* ------------------------------------------------------------------------- */
/* Atomics / time                                                             */
/* ------------------------------------------------------------------------- */

long plat_atomic_set(volatile long *p, long v)
{
    return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

long plat_atomic_get(volatile long *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

unsigned long long plat_tick_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}

void plat_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int plat_num_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n >= 1 ? (int)n : 1;
}

/* ------------------------------------------------------------------------- */
/* Threads                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    plat_thread_fn fn;
    void *arg;
} plat__start;

static void *plat__entry(void *pv)
{
    plat__start *s = (plat__start *)pv;
    plat_thread_fn fn = s->fn;
    void *arg = s->arg;
    free(s);
    plat_thread_below_normal();
    fn(arg);
    return NULL;
}

int plat_thread_spawn(plat_thread *out, plat_thread_fn fn, void *arg)
{
    plat__start *s;
    if (!out || !fn) return -1;
    s = (plat__start *)malloc(sizeof *s);
    if (!s) return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(out, NULL, plat__entry, s) != 0) {
        free(s);
        return -1;
    }
    return 0;
}

void plat_thread_join(plat_thread *t)
{
    if (!t || !*t) return;
    pthread_join(*t, NULL);
}

void plat_thread_below_normal(void)
{
    /* Best effort: raising the nice level needs no privilege. When it is
     * not permitted (some containers) the workers just run at default
     * priority -- the engine contract only asks for best effort. */
    (void)setpriority(PRIO_PROCESS, 0, 10);
}

/* ------------------------------------------------------------------------- */
/* Dynamic loading                                                            */
/* ------------------------------------------------------------------------- */

void *plat_dlopen(const eng_path_t *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

void *plat_dlsym(void *lib, const char *name)
{
    return lib ? dlsym(lib, name) : NULL;
}

void plat_dlclose(void *lib)
{
    if (lib) dlclose(lib);
}
