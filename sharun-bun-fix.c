/*
 * sharun-bun-fix.c
 *
 * LD_PRELOAD shim that makes bun's single-file-executable binaries work
 * when packaged with sharun (https://github.com/VHSgunzo/sharun) inside an
 * AppImage.
 *
 * Background
 * ----------
 * sharun arranges its tree like this:
 *
 *     $SHARUN_DIR/bin/<name>           -> hard link to the sharun launcher
 *     $SHARUN_DIR/shared/bin/<name>    -> the real application binary
 *
 * When the user runs $SHARUN_DIR/bin/<name>, sharun performs a userland
 * execve into the real interpreter (ld-linux.so) with the correct
 * arguments. From the kernel's point of view, /proc/self/exe still points
 * at $SHARUN_DIR/bin/<name> (the sharun hard link), not at the real
 * binary.
 *
 * For most programs this is fine. bun-compiled single-file executables
 * are special: they are self-extracting archives that mmap or read
 * /proc/self/exe in order to find their embedded JavaScript payload.
 * When they do that against the sharun hard link they don't find the
 * payload and the application aborts.
 *
 * Naive fix: rewrite every readlink("/proc/self/exe") to return the
 * shared/bin path. That breaks re-execution: many tools (including bun
 * itself) spawn child copies of themselves by reading /proc/self/exe and
 * passing the result to execve. If we point them at shared/bin/<name>,
 * the kernel runs the real ELF directly, bypassing sharun, and the
 * bundled libraries can no longer be found.
 *
 * Correct fix (this file):
 *   * readlink / readlinkat / open / openat / stat family calls that
 *     refer to /proc/self/exe (or /proc/<pid>/exe for our own pid) are
 *     redirected to $SHARUN_DIR/shared/bin/<name>, so bun can read its
 *     own payload.
 *   * execve / execv*, posix_spawn* calls whose target is
 *     $SHARUN_DIR/shared/bin/<name> are rewritten to
 *     $SHARUN_DIR/bin/<name>, so child processes go through sharun
 *     again.
 *
 * Build:
 *     cc -O2 -fPIC -shared -Wall -Wextra -o sharun-bun-fix.so \
 *        sharun-bun-fix.c -ldl
 *
 * Usage:
 *     LD_PRELOAD=$SHARUN_DIR/sharun-bun-fix.so $SHARUN_DIR/bin/<name>
 *
 * Dependencies: only libc and libdl. No glibc-specific features are
 * required, the code is portable to musl as well.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <alloca.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Cached values, populated lazily and never freed (process-lifetime). */
static char    g_sharun_dir[PATH_MAX];   /* canonicalized */
static size_t  g_sharun_dir_len;
static char    g_bin_prefix[PATH_MAX];   /* $SHARUN_DIR/bin/    */
static size_t  g_bin_prefix_len;
static char    g_shared_prefix[PATH_MAX];/* $SHARUN_DIR/shared/bin/ */
static size_t  g_shared_prefix_len;
static int     g_initialized = 0;
static int     g_disabled    = 0;        /* set if SHARUN_DIR not usable */

static void init_paths(void)
{
    if (g_initialized) return;
    g_initialized = 1;

    const char *dir = getenv("SHARUN_DIR");
    if (!dir || !*dir) { g_disabled = 1; return; }

    /* Canonicalize via realpath; if that fails just take it verbatim. */
    char resolved[PATH_MAX];
    if (realpath(dir, resolved) != NULL) {
        dir = resolved;
    }

    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof(g_sharun_dir) - 16) {
        g_disabled = 1;
        return;
    }
    /* Strip trailing slashes (except a lone "/"). */
    while (n > 1 && dir[n - 1] == '/') n--;

    memcpy(g_sharun_dir, dir, n);
    g_sharun_dir[n] = '\0';
    g_sharun_dir_len = n;

    int r;
    r = snprintf(g_bin_prefix, sizeof(g_bin_prefix), "%s/bin/", g_sharun_dir);
    if (r <= 0 || (size_t)r >= sizeof(g_bin_prefix)) { g_disabled = 1; return; }
    g_bin_prefix_len = (size_t)r;

    r = snprintf(g_shared_prefix, sizeof(g_shared_prefix),
                 "%s/shared/bin/", g_sharun_dir);
    if (r <= 0 || (size_t)r >= sizeof(g_shared_prefix)) {
        g_disabled = 1; return;
    }
    g_shared_prefix_len = (size_t)r;
}

/*
 * Returns non-zero if `path` refers to /proc/self/exe or to
 * /proc/<our-pid>/exe (with optional duplicate slashes). The check is
 * purely textual; we deliberately do not follow symlinks.
 */
static int is_proc_self_exe(const char *path)
{
    if (!path) return 0;

    /* /proc/self/exe */
    if (strcmp(path, "/proc/self/exe") == 0) return 1;

    /* /proc/<pid>/exe for our own pid */
    static char own[64];
    static size_t own_len;
    if (own_len == 0) {
        int n = snprintf(own, sizeof(own), "/proc/%ld/exe", (long)getpid());
        if (n > 0 && (size_t)n < sizeof(own)) own_len = (size_t)n;
    }
    if (own_len && strcmp(path, own) == 0) return 1;

    /* /proc/<pid>/exe for thread-group leader == getpid() on Linux, but
     * just in case anyone constructs the path with the current tid: we
     * also accept /proc/self/task/<tid>/exe pointing to ourselves. */
    if (strncmp(path, "/proc/self/task/", 16) == 0) {
        const char *p = path + 16;
        while (*p >= '0' && *p <= '9') p++;
        if (strcmp(p, "/exe") == 0) return 1;
    }
    return 0;
}

/*
 * If `path` starts with $SHARUN_DIR/shared/bin/, copy a rewritten path
 * ($SHARUN_DIR/bin/<rest>) into `out` and return non-zero. Otherwise
 * return zero and leave `out` untouched.
 */
static int rewrite_shared_to_bin(const char *path, char *out, size_t out_sz)
{
    init_paths();
    if (g_disabled || !path) return 0;
    if (strncmp(path, g_shared_prefix, g_shared_prefix_len) != 0) return 0;

    const char *rest = path + g_shared_prefix_len;
    int n = snprintf(out, out_sz, "%s%s", g_bin_prefix, rest);
    return (n > 0 && (size_t)n < out_sz);
}

/*
 * Resolve /proc/self/exe ourselves (using the real readlink, so the
 * resolution is unaffected by this shim) and, if the target lives under
 * $SHARUN_DIR/bin/, return $SHARUN_DIR/shared/bin/<name> in `out`.
 *
 * Returns the length of the resolved path on success, or -1 on failure
 * (errno preserved from the real readlink call).
 */
typedef ssize_t (*readlink_fn)(const char *, char *, size_t);

static ssize_t resolve_self_exe_target(char *out, size_t out_sz)
{
    static readlink_fn real_readlink = NULL;
    if (!real_readlink) {
        real_readlink = (readlink_fn)dlsym(RTLD_NEXT, "readlink");
        if (!real_readlink) { errno = ENOSYS; return -1; }
    }

    char buf[PATH_MAX];
    ssize_t n = real_readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return -1;
    buf[n] = '\0';

    init_paths();
    if (g_disabled) {
        if ((size_t)n >= out_sz) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, buf, (size_t)n + 1);
        return n;
    }

    /* If it's already under shared/bin, return as-is. */
    if (strncmp(buf, g_shared_prefix, g_shared_prefix_len) == 0) {
        if ((size_t)n >= out_sz) { errno = ENAMETOOLONG; return -1; }
        memcpy(out, buf, (size_t)n + 1);
        return n;
    }
    /* If it's under bin/, rewrite to shared/bin/. */
    if (strncmp(buf, g_bin_prefix, g_bin_prefix_len) == 0) {
        const char *name = buf + g_bin_prefix_len;
        int w = snprintf(out, out_sz, "%s%s", g_shared_prefix, name);
        if (w <= 0 || (size_t)w >= out_sz) {
            errno = ENAMETOOLONG; return -1;
        }
        return w;
    }
    /* Anything else: pass through untouched. */
    if ((size_t)n >= out_sz) { errno = ENAMETOOLONG; return -1; }
    memcpy(out, buf, (size_t)n + 1);
    return n;
}

/* ------------------------------------------------------------------ */
/* readlink / readlinkat                                              */
/* ------------------------------------------------------------------ */

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    static readlink_fn real = NULL;
    if (!real) real = (readlink_fn)dlsym(RTLD_NEXT, "readlink");

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        ssize_t n = resolve_self_exe_target(resolved, sizeof(resolved));
        if (n < 0) return -1;
        size_t copy = (size_t)n < bufsiz ? (size_t)n : bufsiz;
        memcpy(buf, resolved, copy);
        return (ssize_t)copy;
    }
    if (!real) { errno = ENOSYS; return -1; }
    return real(path, buf, bufsiz);
}

typedef ssize_t (*readlinkat_fn)(int, const char *, char *, size_t);

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
    static readlinkat_fn real = NULL;
    if (!real) real = (readlinkat_fn)dlsym(RTLD_NEXT, "readlinkat");

    /* Only intercept absolute paths; relative paths through dirfd are
     * left to the kernel because resolving them ourselves is racy. */
    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        ssize_t n = resolve_self_exe_target(resolved, sizeof(resolved));
        if (n < 0) return -1;
        size_t copy = (size_t)n < bufsiz ? (size_t)n : bufsiz;
        memcpy(buf, resolved, copy);
        return (ssize_t)copy;
    }
    if (!real) { errno = ENOSYS; return -1; }
    return real(dirfd, path, buf, bufsiz);
}

/* ------------------------------------------------------------------ */
/* open / openat (and 64-bit variants)                                */
/* ------------------------------------------------------------------ */

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);

static int open_dispatch(open_fn real, const char *path, int flags, mode_t mode)
{
    return real(path, flags, mode);
}
static int openat_dispatch(openat_fn real, int dirfd, const char *path,
                           int flags, mode_t mode)
{
    return real(dirfd, path, flags, mode);
}

int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                 )) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return open_dispatch(real, resolved, flags, mode);
        }
    }
    return open_dispatch(real, path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                 )) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    static open_fn real = NULL;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open64");
    if (!real) {
        /* Fall back to open() on musl, which has no open64. */
        return open(path, flags, mode);
    }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return open_dispatch(real, resolved, flags, mode);
        }
    }
    return open_dispatch(real, path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                 )) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    static openat_fn real = NULL;
    if (!real) real = (openat_fn)dlsym(RTLD_NEXT, "openat");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return openat_dispatch(real, dirfd, resolved, flags, mode);
        }
    }
    return openat_dispatch(real, dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                 )) {
        va_list ap; va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }

    static openat_fn real = NULL;
    if (!real) real = (openat_fn)dlsym(RTLD_NEXT, "openat64");
    if (!real) {
        return openat(dirfd, path, flags, mode);
    }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return openat_dispatch(real, dirfd, resolved, flags, mode);
        }
    }
    return openat_dispatch(real, dirfd, path, flags, mode);
}

/* ------------------------------------------------------------------ */
/* fopen / fopen64                                                    */
/* ------------------------------------------------------------------ */

typedef FILE *(*fopen_fn)(const char *, const char *);

FILE *fopen(const char *path, const char *mode)
{
    static fopen_fn real = NULL;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    if (!real) { errno = ENOSYS; return NULL; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(resolved, mode);
        }
    }
    return real(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
    static fopen_fn real = NULL;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen64");
    if (!real) return fopen(path, mode);

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(resolved, mode);
        }
    }
    return real(path, mode);
}

/* ------------------------------------------------------------------ */
/* access / faccessat                                                 */
/* ------------------------------------------------------------------ */

typedef int (*access_fn)(const char *, int);
typedef int (*faccessat_fn)(int, const char *, int, int);

int access(const char *path, int mode)
{
    static access_fn real = NULL;
    if (!real) real = (access_fn)dlsym(RTLD_NEXT, "access");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(resolved, mode);
        }
    }
    return real(path, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags)
{
    static faccessat_fn real = NULL;
    if (!real) real = (faccessat_fn)dlsym(RTLD_NEXT, "faccessat");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(dirfd, resolved, mode, flags);
        }
    }
    return real(dirfd, path, mode, flags);
}

/* ------------------------------------------------------------------ */
/* stat / lstat / fstatat (and 64-bit variants)                       */
/*                                                                    */
/* On glibc these symbols are usually inlined wrappers around the     */
/* __xstat family. Intercepting both shapes keeps us compatible with  */
/* both glibc and musl.                                               */
/* ------------------------------------------------------------------ */

typedef int (*stat_fn)(const char *, struct stat *);
typedef int (*fstatat_fn)(int, const char *, struct stat *, int);
typedef int (*xstat_fn)(int, const char *, struct stat *);
typedef int (*fxstatat_fn)(int, int, const char *, struct stat *, int);

int stat(const char *path, struct stat *st)
{
    static stat_fn real = NULL;
    if (!real) real = (stat_fn)dlsym(RTLD_NEXT, "stat");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(resolved, st);
        }
    }
    return real(path, st);
}

int lstat(const char *path, struct stat *st)
{
    static stat_fn real = NULL;
    if (!real) real = (stat_fn)dlsym(RTLD_NEXT, "lstat");
    if (!real) { errno = ENOSYS; return -1; }
    /* We intentionally do NOT redirect lstat on /proc/self/exe: lstat
     * is supposed to describe the symlink itself, not its target. */
    return real(path, st);
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    static fstatat_fn real = NULL;
    if (!real) real = (fstatat_fn)dlsym(RTLD_NEXT, "fstatat");
    if (!real) { errno = ENOSYS; return -1; }

    if (!(flags & AT_SYMLINK_NOFOLLOW) && is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(dirfd, resolved, st, flags);
        }
    }
    return real(dirfd, path, st, flags);
}

/* glibc-internal __xstat shims (older glibc). */
int __xstat(int ver, const char *path, struct stat *st)
{
    static xstat_fn real = NULL;
    if (!real) real = (xstat_fn)dlsym(RTLD_NEXT, "__xstat");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(ver, resolved, st);
        }
    }
    return real(ver, path, st);
}

int __xstat64(int ver, const char *path, struct stat *st)
{
    static xstat_fn real = NULL;
    if (!real) real = (xstat_fn)dlsym(RTLD_NEXT, "__xstat64");
    if (!real) { errno = ENOSYS; return -1; }

    if (is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(ver, resolved, st);
        }
    }
    return real(ver, path, st);
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags)
{
    static fxstatat_fn real = NULL;
    if (!real) real = (fxstatat_fn)dlsym(RTLD_NEXT, "__fxstatat");
    if (!real) { errno = ENOSYS; return -1; }

    if (!(flags & AT_SYMLINK_NOFOLLOW) && is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(ver, dirfd, resolved, st, flags);
        }
    }
    return real(ver, dirfd, path, st, flags);
}

int __fxstatat64(int ver, int dirfd, const char *path, struct stat *st, int flags)
{
    static fxstatat_fn real = NULL;
    if (!real) real = (fxstatat_fn)dlsym(RTLD_NEXT, "__fxstatat64");
    if (!real) { errno = ENOSYS; return -1; }

    if (!(flags & AT_SYMLINK_NOFOLLOW) && is_proc_self_exe(path)) {
        char resolved[PATH_MAX];
        if (resolve_self_exe_target(resolved, sizeof(resolved)) >= 0) {
            return real(ver, dirfd, resolved, st, flags);
        }
    }
    return real(ver, dirfd, path, st, flags);
}

/* ------------------------------------------------------------------ */
/* exec* family                                                       */
/*                                                                    */
/* If a program asks the kernel to execute                            */
/*    $SHARUN_DIR/shared/bin/<name>                                   */
/* we rewrite the path to                                             */
/*    $SHARUN_DIR/bin/<name>                                          */
/* so the sharun hard link is used and the bundled libraries are      */
/* found correctly. argv is left untouched on purpose: many programs  */
/* (bun included) rely on argv[0] being the resolved /proc/self/exe   */
/* path, and the launcher does not care what argv[0] is.              */
/* ------------------------------------------------------------------ */

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*execv_fn)(const char *, char *const[]);
typedef int (*execveat_fn)(int, const char *, char *const[], char *const[], int);
typedef int (*posix_spawn_fn)(pid_t *, const char *,
                              const void *, const void *,
                              char *const[], char *const[]);

int execve(const char *path, char *const argv[], char *const envp[])
{
    static execve_fn real = NULL;
    if (!real) real = (execve_fn)dlsym(RTLD_NEXT, "execve");
    if (!real) { errno = ENOSYS; return -1; }

    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(path, fixed, sizeof(fixed))) {
        return real(fixed, argv, envp);
    }
    return real(path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    static execv_fn real = NULL;
    if (!real) real = (execv_fn)dlsym(RTLD_NEXT, "execv");
    if (!real) { errno = ENOSYS; return -1; }

    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(path, fixed, sizeof(fixed))) {
        return real(fixed, argv);
    }
    return real(path, argv);
}

int execvp(const char *file, char *const argv[])
{
    static execv_fn real = NULL;
    if (!real) real = (execv_fn)dlsym(RTLD_NEXT, "execvp");
    if (!real) { errno = ENOSYS; return -1; }

    /* rewrite_shared_to_bin matches only paths starting with
     * $SHARUN_DIR/shared/bin/, which always contains slashes, so this
     * is correctly a no-op for execvp's slashless PATH-search case. */
    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(file, fixed, sizeof(fixed))) {
        return real(fixed, argv);
    }
    return real(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    static execve_fn real = NULL;
    if (!real) real = (execve_fn)dlsym(RTLD_NEXT, "execvpe");
    if (!real) { errno = ENOSYS; return -1; }

    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(file, fixed, sizeof(fixed))) {
        return real(fixed, argv, envp);
    }
    return real(file, argv, envp);
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags)
{
    static execveat_fn real = NULL;
    if (!real) real = (execveat_fn)dlsym(RTLD_NEXT, "execveat");
    if (!real) { errno = ENOSYS; return -1; }

    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(path, fixed, sizeof(fixed))) {
        return real(dirfd, fixed, argv, envp, flags);
    }
    return real(dirfd, path, argv, envp, flags);
}

/*
 * The execl* family is implemented in libc as varargs wrappers around
 * execv / execvp / execve. Because we already intercept the v* variants
 * those calls usually funnel through us. However, libc internals may
 * call the underlying syscall directly, so we also override the l*
 * variants for completeness.
 */

#define COLLECT_ARGV(first_arg, ap_anchor, argv_out) do {                 \
    va_list _ap;                                                          \
    va_start(_ap, ap_anchor);                                             \
    int _n = 1;                                                           \
    va_list _ap2; va_copy(_ap2, _ap);                                     \
    while (va_arg(_ap2, char *) != NULL) _n++;                            \
    va_end(_ap2);                                                         \
    (argv_out) = (char **)alloca(((size_t)_n + 1) * sizeof(char *));      \
    (argv_out)[0] = (char *)(first_arg);                                  \
    for (int _i = 1; _i < _n; _i++)                                       \
        (argv_out)[_i] = va_arg(_ap, char *);                             \
    (argv_out)[_n] = NULL;                                                \
    va_end(_ap);                                                          \
} while (0)

int execl(const char *path, const char *arg0, ...)
{
    char **argv;
    COLLECT_ARGV(arg0, arg0, argv);
    return execv(path, argv);
}

int execlp(const char *file, const char *arg0, ...)
{
    char **argv;
    COLLECT_ARGV(arg0, arg0, argv);
    return execvp(file, argv);
}

int execle(const char *path, const char *arg0, ...)
{
    /* execle: arguments followed by NULL, followed by envp. */
    va_list ap;
    va_start(ap, arg0);
    int n = 1;
    va_list ap2; va_copy(ap2, ap);
    while (va_arg(ap2, char *) != NULL) n++;
    va_end(ap2);

    char **argv = (char **)alloca(((size_t)n + 1) * sizeof(char *));
    argv[0] = (char *)arg0;
    for (int i = 1; i < n; i++) argv[i] = va_arg(ap, char *);
    (void)va_arg(ap, char *); /* NULL */
    char *const *envp = va_arg(ap, char *const *);
    argv[n] = NULL;
    va_end(ap);
    return execve(path, argv, envp);
}

/* posix_spawn / posix_spawnp: same idea as execve. */
int posix_spawn(pid_t *pid, const char *path,
                const void *file_actions, const void *attrp,
                char *const argv[], char *const envp[])
{
    static posix_spawn_fn real = NULL;
    if (!real) real = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    if (!real) { errno = ENOSYS; return -1; }

    char fixed[PATH_MAX];
    if (rewrite_shared_to_bin(path, fixed, sizeof(fixed))) {
        return real(pid, fixed, file_actions, attrp, argv, envp);
    }
    return real(pid, path, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const void *file_actions, const void *attrp,
                 char *const argv[], char *const envp[])
{
    static posix_spawn_fn real = NULL;
    if (!real) real = (posix_spawn_fn)dlsym(RTLD_NEXT, "posix_spawnp");
    if (!real) { errno = ENOSYS; return -1; }

    if (file && strchr(file, '/')) {
        char fixed[PATH_MAX];
        if (rewrite_shared_to_bin(file, fixed, sizeof(fixed))) {
            return real(pid, fixed, file_actions, attrp, argv, envp);
        }
    }
    return real(pid, file, file_actions, attrp, argv, envp);
}
