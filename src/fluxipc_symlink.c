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
 * client_resolve – given argv[0] under /run/<prog>-fluxipc/...,
 * extract prog_name, sock_path, shm_name.
 *
 * Directory name format: <prog>-fluxipc
 * So from /run/myprog-fluxipc/devices/stub we get prog = "myprog".
 */
int client_resolve(const char *argv0,
                   char *out_prog, size_t prog_sz,
                   char *out_sock, size_t sock_sz,
                   char *out_shm,  size_t shm_sz)
{
    if (!argv0) return -1;

    char abs0[PATH_MAX];
    to_abs_path(argv0, abs0, sizeof(abs0));

    /* Must be under /run/ */
    if (strncmp(abs0, "/run/", 5) != 0) return -1;

    /* Extract the directory component (first path element after /run/) */
    const char *after_run = abs0 + 5;  /* e.g. "myprog-fluxipc/devices/..." */
    const char *slash = strchr(after_run, '/');
    if (!slash) return -1;             /* no subpath → not an IPC symlink */

    /* dir_name = "myprog-fluxipc" */
    size_t dir_len = (size_t)(slash - after_run);
    char dir_name[FLUXIPC_NAME_MAX];
    if (dir_len >= sizeof(dir_name)) return -1;
    memcpy(dir_name, after_run, dir_len);
    dir_name[dir_len] = '\0';

    /* Must end with "-fluxipc" */
    const char *suffix = "-fluxipc";
    size_t slen = strlen(suffix);
    if (dir_len <= slen) return -1;
    if (strcmp(dir_name + dir_len - slen, suffix) != 0) return -1;

    /* prog_name = dir_name without the "-fluxipc" suffix */
    size_t prog_len = dir_len - slen;
    if (prog_len >= prog_sz) return -1;
    memcpy(out_prog, dir_name, prog_len);
    out_prog[prog_len] = '\0';

    /* sock and shm derived from prog_name */
    snprintf(out_sock, sock_sz, "/run/%s-fluxipc/%s.sock", out_prog, out_prog);
    snprintf(out_shm,  shm_sz,  FLUXIPC_SHM_PREFIX "%s", out_prog);
    return 0;
}

/*
 * client_ipc_path – extract the IPC path (suffix after run_dir in argv0).
 * e.g. argv0=/run/myprog-fluxipc/devices/stub/name  → /devices/stub/name
 */
int client_ipc_path(const char *argv0, const char *prog_name,
                    char *out_path, size_t path_sz)
{
    char abs0[PATH_MAX];
    to_abs_path(argv0, abs0, sizeof(abs0));

    char run_dir[PATH_MAX];
    snprintf(run_dir, sizeof(run_dir), "/run/%s-fluxipc", prog_name);
    size_t plen = strlen(run_dir);

    if (strncmp(abs0, run_dir, plen) == 0 && abs0[plen] == '/') {
        snprintf(out_path, path_sz, "%s", abs0 + plen);
        return 0;
    }
    return -1;
}
