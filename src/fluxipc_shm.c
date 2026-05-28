/**
 * fluxipc_shm.c – POSIX shared-memory management
 *
 * Server creates with mode 0666 (world-readable/writable).
 * Clients MUST open O_RDWR — even pthread_rwlock_rdlock writes the futex
 * word, so a PROT_READ mapping will SIGSEGV on lock acquisition.
 */

#include "fluxipc_internal.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static void make_shm_name(const char *prog_name, char *out, size_t sz)
{
    snprintf(out, sz, FLUXIPC_SHM_PREFIX "%s", prog_name);
}

/* ─── Server: create ──────────────────────────────────────────────────────── */

int shm_open_create(fluxipc_ctx_t *ctx)
{
    make_shm_name(ctx->prog_name, ctx->shm_name, sizeof(ctx->shm_name));

    size_t sz = sizeof(fluxipc_shm_t);
    ctx->shm_size = sz;

    ctx->shm_fd = shm_open(ctx->shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (ctx->shm_fd < 0 && errno == EEXIST) {
        shm_unlink(ctx->shm_name);
        ctx->shm_fd = shm_open(ctx->shm_name, O_CREAT | O_EXCL | O_RDWR, 0666);
    }
    if (ctx->shm_fd < 0) { perror("fluxipc: shm_open"); return -errno; }
    fchmod(ctx->shm_fd, 0666); /* override umask */

    if (ftruncate(ctx->shm_fd, (off_t)sz) < 0) {
        perror("fluxipc: ftruncate shm");
        shm_unlink(ctx->shm_name);
        close(ctx->shm_fd);
        return -errno;
    }

    ctx->shm = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ctx->shm_fd, 0);
    if (ctx->shm == MAP_FAILED) {
        perror("fluxipc: mmap shm");
        shm_unlink(ctx->shm_name);
        close(ctx->shm_fd);
        return -errno;
    }

    memset(ctx->shm, 0, sz);
    ctx->shm->magic   = FLUXIPC_SHM_MAGIC;
    ctx->shm->version = FLUXIPC_SHM_VERSION;
    snprintf(ctx->shm->prog_name, FLUXIPC_NAME_MAX, "%s", ctx->prog_name);
    snprintf(ctx->shm->sock_path, FLUXIPC_PATH_MAX, "%s", ctx->sock_path);
    atomic_store(&ctx->shm->entry_count, 0);
    atomic_store(&ctx->shm->next_id, 1);

    /* Process-shared rwlock */
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(&ctx->shm->lock, &attr);
    pthread_rwlockattr_destroy(&attr);

    return 0;
}

/* ─── Client: open existing ───────────────────────────────────────────────── */

fluxipc_shm_t *shm_open_existing(const char *prog_name)
{
    char name[FLUXIPC_NAME_MAX + 16];
    make_shm_name(prog_name, name, sizeof(name));

    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return NULL;

    fluxipc_shm_t *shm = mmap(NULL, sizeof(fluxipc_shm_t),
                               PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) return NULL;
    if (shm->magic != FLUXIPC_SHM_MAGIC) { munmap(shm, sizeof(*shm)); return NULL; }
    return shm;
}

void shm_close_existing(fluxipc_shm_t *shm)
{
    if (shm) munmap(shm, sizeof(fluxipc_shm_t));
}

/* ─── Entry management ────────────────────────────────────────────────────── */

int shm_add_entry(fluxipc_ctx_t *ctx, fluxipc_node_t *node, int is_static)
{
    if (!ctx->shm) return -EINVAL;

    pthread_rwlock_wrlock(&ctx->shm->lock);

    unsigned count = atomic_load(&ctx->shm->entry_count);
    if (count >= FLUXIPC_SHM_MAX_ENTRIES) {
        pthread_rwlock_unlock(&ctx->shm->lock);
        return -ENOSPC;
    }

    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < FLUXIPC_SHM_MAX_ENTRIES; i++) {
        if (!(ctx->shm->entries[i].flags & FLUXIPC_FLAG_ACTIVE)) {
            idx = i; break;
        }
    }
    if (idx == UINT32_MAX) { pthread_rwlock_unlock(&ctx->shm->lock); return -ENOSPC; }

    uint32_t id = atomic_fetch_add(&ctx->shm->next_id, 1);
    node->id      = id;
    node->shm_idx = idx;

    fluxipc_shm_entry_t *e = &ctx->shm->entries[idx];
    memset(e, 0, sizeof(*e));
    snprintf(e->path,      FLUXIPC_PATH_MAX,  "%s", node->full_path);
    snprintf(e->usage,     FLUXIPC_USAGE_MAX, "%s", node->usage);
    snprintf(e->sock_path, FLUXIPC_PATH_MAX,  "%s", ctx->sock_path);
    e->id           = id;
    e->flags        = FLUXIPC_FLAG_ACTIVE | (is_static ? FLUXIPC_FLAG_STATIC : 0);
    e->slot_data_sz = node->slot_data_sz;
    e->owner_pid    = getpid();

    atomic_fetch_add(&ctx->shm->entry_count, 1);
    pthread_rwlock_unlock(&ctx->shm->lock);
    return 0;
}

void shm_remove_entry(fluxipc_ctx_t *ctx, uint32_t shm_idx)
{
    if (!ctx->shm || shm_idx >= FLUXIPC_SHM_MAX_ENTRIES) return;
    pthread_rwlock_wrlock(&ctx->shm->lock);
    fluxipc_shm_entry_t *e = &ctx->shm->entries[shm_idx];
    if (e->flags & FLUXIPC_FLAG_ACTIVE) {
        e->flags = 0;
        atomic_fetch_sub(&ctx->shm->entry_count, 1);
    }
    pthread_rwlock_unlock(&ctx->shm->lock);
}

void shm_close(fluxipc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->shm && ctx->shm != MAP_FAILED) {
        pthread_rwlock_destroy(&ctx->shm->lock);
        munmap(ctx->shm, ctx->shm_size);
        ctx->shm = NULL;
    }
    if (ctx->shm_fd >= 0) { close(ctx->shm_fd); ctx->shm_fd = -1; }
    if (ctx->shm_name[0]) shm_unlink(ctx->shm_name);
}
