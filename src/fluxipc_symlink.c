/**
 * fluxipc_symlink.c – client-discovery symlinks
 *
 * Per-app layout:
 *   /run/<appname>-fluxipc/          (run_dir, mode 0755)
 *   /run/<appname>-fluxipc/<appname>.sock
 *   /run/<appname>-fluxipc/<ipc_path>  -> /proc/self/exe  (one per endpoint)
 *
 * A client invoked as /run/myprog-fluxipc/devices/stub/xxx/name derives:
 *   prog_name = "myprog"        (strip "-fluxipc" suffix from dir name)
 *   run_dir   = /run/myprog-fluxipc
 *   sock_path = /run/myprog-fluxipc/myprog.sock
 *   shm_name  = /fluxipc.myprog
 *   ipc_path  = /devices/stub/xxx/name   (suffix after run_dir)
 */

#include "fluxipc_internal.h"

#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>

/* ─── Helpers ─────────────────────────────────────────────────────────────── */

static int mkdirs(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -errno;
            *p = '/';
        }
    }
    return (mkdir(tmp, mode) < 0 && errno != EEXIST) ? -errno : 0;
}

static int get_binary_path(char *out, size_t sz)
{
    ssize_t n = readlink("/proc/self/exe", out, sz - 1);
    if (n < 0) return -errno;
    out[n] = '\0';
    return 0;
}

/** Collapse /./ and /../ in-place (does not follow symlinks). */
static void normalize_path(char *path)
{
    if (!path || path[0] != '/') return;
    char *dst = path, *src = path;
    while (*src) {
        while (*src == '/') src++;
        if (!*src) break;
        char *beg = src;
        while (*src && *src != '/') src++;
        size_t len = (size_t)(src - beg);
        if (len == 1 && beg[0] == '.') continue;
        if (len == 2 && beg[0] == '.' && beg[1] == '.') {
            if (dst > path + 1) { dst--; while (dst[-1] != '/') dst--; }
            continue;
        }
        *dst++ = '/';
        for (size_t i = 0; i < len; i++) dst[i] = beg[i];
        dst += len;
    }
    if (dst == path) *dst++ = '/';
    *dst = '\0';
}

/** Normalise argv0 to an absolute path (no readlink follow). */
static void to_abs_path(const char *argv0, char *out, size_t sz)
{
    if (argv0[0] == '/') {
        snprintf(out, sz, "%s", argv0);
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            int n = snprintf(out, sz, "%s/", cwd);
            if (n > 0 && (size_t)n < sz)
                snprintf(out + n, sz - (size_t)n, "%s", argv0);
        } else {
            snprintf(out, sz, "%s", argv0);
        }
    }
    normalize_path(out);
}

/* ─── Create ──────────────────────────────────────────────────────────────── */

/*
 * symlink_create – create /run/<prog>-fluxipc/<ipc_path> -> binary
 *
 * @param prog_name  application name, e.g. "example_server"
 * @param ipc_path   absolute IPC path, e.g. "/devices/stub/xxx/name"
 * @param run_dir    pre-computed run dir, e.g. "/run/example_server-fluxipc"
 */
int symlink_create(const char *prog_name, const char *ipc_path,
                   const char *run_dir)
{
    (void)prog_name;

    char binary[PATH_MAX];
    if (get_binary_path(binary, sizeof(binary)) < 0)
        snprintf(binary, sizeof(binary), "/proc/self/exe");

    /* link path: /run/<prog>-fluxipc<ipc_path>
     * ipc_path already starts with '/', so concatenate directly */
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s%s", run_dir, ipc_path);

    /* Create parent directories inside run_dir */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", link_path);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = '\0';
        mkdirs(parent, 0755);
    }

    unlink(link_path);
    if (symlink(binary, link_path) < 0) return -errno;
    return 0;
}

/* ─── Remove ──────────────────────────────────────────────────────────────── */

void symlink_remove(const char *prog_name, const char *ipc_path,
                    const char *run_dir)
{
    (void)prog_name;
    char link_path[PATH_MAX];
    snprintf(link_path, sizeof(link_path), "%s%s", run_dir, ipc_path);
    unlink(link_path);
}

static void rmdir_r(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { rmdir(path); return; }
    struct dirent *ent;
    char sub[PATH_MAX];
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(sub, &st) < 0) continue;
        if (S_ISDIR(st.st_mode)) rmdir_r(sub);
        else unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

/** Remove the entire /run/<prog>-fluxipc/ directory tree. */
void symlink_remove_all(const char *run_dir)
{
    rmdir_r(run_dir);
}

/* ─── Client-side resolution ──────────────────────────────────────────────── */

/*
 * fluxipc_client_resolve – scan argv[0] for a *-fluxipc* path component,
 * extract prog_name, sock_path, shm_name.
 *
 * Works with any run-dir layout:
 *   /run/user/0/myprog-fluxipc/devices/stub    (root)
 *   /run/user/1000/myprog-fluxipc/devices/stub (ordinary user)
 */
int fluxipc_client_resolve(const char *argv0,
                   char *out_prog, size_t prog_sz,
                   char *out_sock, size_t sock_sz,
                   char *out_shm,  size_t shm_sz)
{
    if (!argv0) return -1;

    char abs0[PATH_MAX];
    to_abs_path(argv0, abs0, sizeof(abs0));

    /* Find the "-fluxipc" marker anywhere in the path */
    const char *suffix = "-fluxipc";
    size_t slen = strlen(suffix);
    char *f = strstr(abs0, suffix);
    if (!f) return -1;

    /* Walk back to start of this path component */
    char *cs = f;
    while (cs > abs0 && cs[-1] != '/') cs--;

    /* prog_name = cs..f */
    size_t prog_len = (size_t)(f - cs);
    if (prog_len == 0 || prog_len >= prog_sz) return -1;
    memcpy(out_prog, cs, prog_len);
    out_prog[prog_len] = '\0';

    /* Walk forward to end of component (f + slen), might include "-<uid>" */
    char *ce = f + slen;
    while (*ce && *ce != '/') ce++;

    /* run_dir = abs0[0..ce-1] */
    size_t rlen = (size_t)(ce - abs0);
    if (rlen >= FLUXIPC_PATH_MAX) return -1;
    char run_dir[FLUXIPC_PATH_MAX];
    memcpy(run_dir, abs0, rlen);
    run_dir[rlen] = '\0';

    /* sock / shm from run_dir + prog_name */
    snprintf(out_sock, sock_sz, "%s/%s.sock", run_dir, out_prog);
    snprintf(out_shm,  shm_sz,  FLUXIPC_SHM_PREFIX "%s", out_prog);
    return 0;
}

/*
 * client_ipc_path – extract the IPC path (suffix after run_dir in argv0).
 * Uses strstr("-fluxipc") to locate the run_dir component.
 * e.g. argv0=/run/user/1000/myprog-fluxipc/devices/stub/name  → /devices/stub/name
 */
int client_ipc_path(const char *argv0, const char *prog_name,
                    char *out_path, size_t path_sz)
{
    (void)prog_name;
    char abs0[PATH_MAX];
    to_abs_path(argv0, abs0, sizeof(abs0));

    /* Find "-fluxipc" marker */
    const char *suffix = "-fluxipc";
    char *f = strstr(abs0, suffix);
    if (!f) return -1;

    /* Walk to end of the fluxipc component */
    char *ce = f + strlen(suffix);
    while (*ce && *ce != '/') ce++;

    /* IPC path is the suffix after the run_dir component */
    if (*ce == '/')
        snprintf(out_path, path_sz, "%s", ce);
    else
        snprintf(out_path, path_sz, "/");
    return 0;
}
