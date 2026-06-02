/**
 * fluxipc_interactive.c – REPL with cd navigation and tab-completion
 *
 * Path management uses a tree (trie over path components) instead of a flat
 * array, so lookups are O(depth) and completion just enumerates node children.
 *
 * cd behaviour:
 *   cd /module/sensor  → changes cwd to /module/sensor
 *                         prompt becomes  fluxipc@/module/sensor>
 *   cd /               → returns to root, prompt is fluxipc>
 *   cd ..              → goes up one level
 *   cd (no arg)        → goes to root
 *
 * Completion behaviour (relative to current cwd):
 *   In cwd /module/sensor, typing Tab shows:
 *     calibrate  status  config   (the children of the current node)
 *   Typing /m Tab still does absolute path completion.
 *
 * ls behaviour:
 *   Shows all leaf entries recursively under current cwd.
 *
 * Dispatch behaviour:
 *   Bare name "calibrate" → resolved relative to cwd node
 *   Absolute "/module/sensor/calibrate" always works.
 *
 * Discovery: shm regions are read from /run/&lt;name&gt;-fluxipc/ directories.
 */

#include "fluxipc_internal.h"
#include "fluxipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <readline/readline.h>
#include <readline/history.h>

/* ─── Path tree node ──────────────────────────────────────────────────────── */

typedef struct path_node {
    char     name[FLUXIPC_NAME_MAX];      /* single component; root.name = "" */
    char     full_path[FLUXIPC_PATH_MAX]; /* cached absolute path */
    char     usage[FLUXIPC_USAGE_MAX];    /* leaf only */
    char     sock_path[FLUXIPC_PATH_MAX]; /* leaf only */
    uint32_t id;                          /* leaf only */
    int      is_leaf;                     /* 1 = endpoint, 0 = namespace */

    struct path_node *parent;
    struct path_node *first_child;
    struct path_node *next_sibling;
    int      child_count;
} path_node_t;

static path_node_t *g_root     = NULL;
static path_node_t *g_cwd_node = NULL;

/* ─── Tree helpers ────────────────────────────────────────────────────────── */

static void tree_destroy_subtree(path_node_t *n)
{
    if (!n) return;
    path_node_t *c = n->first_child;
    while (c) {
        path_node_t *next = c->next_sibling;
        tree_destroy_subtree(c);
        c = next;
    }
    free(n);
}

static void tree_clear(void)
{
    if (!g_root) return;
    path_node_t *c = g_root->first_child;
    while (c) {
        path_node_t *next = c->next_sibling;
        tree_destroy_subtree(c);
        c = next;
    }
    g_root->first_child = NULL;
    g_root->child_count  = 0;
    g_cwd_node           = g_root;
}

static void tree_init(void)
{
    if (!g_root) {
        g_root = calloc(1, sizeof(path_node_t));
        if (!g_root) return;
        snprintf(g_root->full_path, sizeof(g_root->full_path), "/");
    }
    g_cwd_node = g_root;
}

/* Find or create a child of parent.  Name must be a single component. */
static path_node_t *node_get_child(path_node_t *parent, const char *name)
{
    for (path_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (strcmp(c->name, name) == 0)
            return c;
    }

    path_node_t *n = calloc(1, sizeof(path_node_t));
    if (!n) return NULL;

    snprintf(n->name, sizeof(n->name), "%s", name);
    n->parent       = parent;
    n->next_sibling = parent->first_child;
    parent->first_child = n;
    parent->child_count++;

    if (parent == g_root) {
        n->full_path[0] = '/';
        strncpy(n->full_path + 1, name, sizeof(n->full_path) - 2);
        n->full_path[sizeof(n->full_path) - 1] = '\0';
    } else {
        size_t plen = strlen(parent->full_path);
        if (plen < sizeof(n->full_path) - 1) {
            memcpy(n->full_path, parent->full_path, plen);
            n->full_path[plen] = '/';
            size_t remain = sizeof(n->full_path) - plen - 2;
            strncpy(n->full_path + plen + 1, name, remain);
        }
        n->full_path[sizeof(n->full_path) - 1] = '\0';
    }
    return n;
}

/* Walk from root by slash-separated components.  Returns NULL if not found. */
static path_node_t *node_lookup(const char *abs_path)
{
    if (!abs_path || abs_path[0] != '/') return NULL;
    if (strcmp(abs_path, "/") == 0) return g_root;

    path_node_t *cur = g_root;
    char buf[FLUXIPC_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", abs_path);

    char *saveptr;
    char *tok = strtok_r(buf + 1, "/", &saveptr);
    while (tok) {
        path_node_t *found = NULL;
        for (path_node_t *c = cur->first_child; c; c = c->next_sibling) {
            if (strcmp(c->name, tok) == 0) { found = c; break; }
        }
        if (!found) return NULL;
        cur = found;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return cur;
}

/* Like node_lookup but relative to g_cwd_node.  Handles ".", "..". */
static path_node_t *node_lookup_rel(const char *path)
{
    if (!path || path[0] == '\0') return g_cwd_node;
    if (path[0] == '/') return node_lookup(path);

    path_node_t *cur = g_cwd_node;
    char buf[FLUXIPC_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);

    char *saveptr;
    char *tok = strtok_r(buf, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
            tok = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        if (strcmp(tok, "..") == 0) {
            if (cur->parent) cur = cur->parent;
            tok = strtok_r(NULL, "/", &saveptr);
            continue;
        }
        path_node_t *found = NULL;
        for (path_node_t *c = cur->first_child; c; c = c->next_sibling) {
            if (strcmp(c->name, tok) == 0) { found = c; break; }
        }
        if (!found) return NULL;
        cur = found;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return cur;
}

/* Insert a path into the tree, creating intermediate nodes as needed.
   Returns the leaf-most node. */
static path_node_t *tree_insert_path(const char *abs_path)
{
    if (!abs_path || abs_path[0] != '/') return NULL;
    if (strcmp(abs_path, "/") == 0) return g_root;

    path_node_t *cur = g_root;
    char buf[FLUXIPC_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", abs_path);

    char *saveptr;
    char *tok = strtok_r(buf + 1, "/", &saveptr);
    while (tok) {
        cur = node_get_child(cur, tok);
        if (!cur) return NULL;
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return cur;
}

/* ─── Display helper ──────────────────────────────────────────────────────── */

/* Return a pointer to the relative display form of node.
   The returned pointer is into node->full_path (valid as long as node lives). */
static const char *rel_path(const path_node_t *node)
{
    if (g_cwd_node == g_root)
        return node->full_path + 1;          /* strip leading "/" */

    size_t clen = strlen(g_cwd_node->full_path);
    if (strncmp(node->full_path, g_cwd_node->full_path, clen) == 0) {
        if (node->full_path[clen] == '/')
            return node->full_path + clen + 1;
        if (node->full_path[clen] == '\0')
            return node->full_path + clen;
    }
    return node->full_path;                  /* fallback */
}

/* ─── Prompt helpers ──────────────────────────────────────────────────────── */

static char g_prompt[FLUXIPC_PATH_MAX + 16];

static void update_prompt(void)
{
    if (g_cwd_node == g_root)
        snprintf(g_prompt, sizeof(g_prompt), "fluxipc> ");
    else
        snprintf(g_prompt, sizeof(g_prompt), "fluxipc@%s> ",
                 g_cwd_node->full_path);
}

/* ─── SHM loading ─────────────────────────────────────────────────────────── */

static void tree_add_entry(const fluxipc_shm_entry_t *e)
{
    path_node_t *node = tree_insert_path(e->path);
    if (!node) return;

    node->is_leaf = 1;
    snprintf(node->usage,     sizeof(node->usage),     "%s", e->usage);
    snprintf(node->sock_path, sizeof(node->sock_path), "%s", e->sock_path);
    node->id = e->id;
}

static void load_all_shm(void)
{
    char saved_cwd[FLUXIPC_PATH_MAX] = "";
    if (g_cwd_node)
        snprintf(saved_cwd, sizeof(saved_cwd), "%s", g_cwd_node->full_path);

    tree_clear();

    /* Scan /run/user/<uid>/ for *-fluxipc server dirs */
    char scan_dir[PATH_MAX];
    snprintf(scan_dir, sizeof(scan_dir), "/run/user/%d", getuid());

    DIR *d = opendir(scan_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_name[0] == '.') continue;
            const char *suffix = "-fluxipc";
            size_t nlen = strlen(ent->d_name);
            size_t slen = strlen(suffix);
            if (nlen <= slen) continue;
            if (strcmp(ent->d_name + nlen - slen, suffix) != 0) continue;

            char sub[PATH_MAX];
            /* scan_dir ≤ 21 chars, d_name ≤ 255 → total ≤ 277, well under 4096 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(sub, sizeof(sub), "%s/%s", scan_dir, ent->d_name);
#pragma GCC diagnostic pop
            struct stat st;
            if (stat(sub, &st) < 0 || !S_ISDIR(st.st_mode)) continue;

            char prog[FLUXIPC_NAME_MAX];
            size_t prog_len = nlen - slen;
            if (prog_len >= sizeof(prog)) continue;
            memcpy(prog, ent->d_name, prog_len);
            prog[prog_len] = '\0';

            fluxipc_shm_t *shm = shm_open_existing(prog);
            if (!shm) {
                fprintf(stderr, "fluxipc: cannot open shm for '%s', skipping\n",
                        prog);
                continue;
            }

            pthread_rwlock_rdlock((pthread_rwlock_t *)&shm->lock);
            for (int i = 0; i < FLUXIPC_SHM_MAX_ENTRIES; i++) {
                if (shm->entries[i].flags & FLUXIPC_FLAG_ACTIVE)
                    tree_add_entry(&shm->entries[i]);
            }
            pthread_rwlock_unlock((pthread_rwlock_t *)&shm->lock);
            shm_close_existing(shm);
        }
        closedir(d);
    }

    /* In-process server entries (dedup via tree lookup) */
    if (g_ctx && g_ctx->shm) {
        pthread_rwlock_rdlock(&g_ctx->shm->lock);
        for (int i = 0; i < FLUXIPC_SHM_MAX_ENTRIES; i++) {
            if (!(g_ctx->shm->entries[i].flags & FLUXIPC_FLAG_ACTIVE))
                continue;
            if (node_lookup(g_ctx->shm->entries[i].path))
                continue;
            tree_add_entry(&g_ctx->shm->entries[i]);
        }
        pthread_rwlock_unlock(&g_ctx->shm->lock);
    }

    /* Restore cwd if still valid */
    if (saved_cwd[0]) {
        path_node_t *r = node_lookup(saved_cwd);
        g_cwd_node = (r && !r->is_leaf) ? r : g_root;
    } else {
        g_cwd_node = g_root;
    }
    update_prompt();
}

/* ─── Recursive leaf collector (for ls / namespace listing) ──────────────── */

static void collect_leaves(path_node_t *node, path_node_t **list,
                           int *count, int max)
{
    for (path_node_t *c = node->first_child; c && *count < max;
         c = c->next_sibling) {
        if (c->is_leaf)
            list[(*count)++] = c;
        collect_leaves(c, list, count, max);
    }
}

/* ─── cd command ──────────────────────────────────────────────────────────── */

static void cmd_cd(const char *arg)
{
    if (!arg || arg[0] == '\0' || strcmp(arg, "/") == 0) {
        g_cwd_node = g_root;
        update_prompt();
        return;
    }

    if (strcmp(arg, "..") == 0) {
        if (g_cwd_node->parent)
            g_cwd_node = g_cwd_node->parent;
        update_prompt();
        return;
    }

    path_node_t *target = node_lookup_rel(arg);
    if (!target) {
        printf("  No such namespace: %s\n\n", arg);
        return;
    }
    if (target->is_leaf) {
        printf("  '%s' is a leaf endpoint, not a namespace\n\n",
               target->full_path);
        return;
    }

    g_cwd_node = target;
    update_prompt();
}

/* ─── Tab completion ──────────────────────────────────────────────────────── */

static char **g_candidates     = NULL;
static int    g_cand_count     = 0;
static int    g_cand_idx       = 0;
static int    g_cand_relative  = 0;
static int    g_cand_dot_slash = 0;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void build_candidates(const char *text)
{
    /* Free previous */
    if (g_candidates) {
        for (int i = 0; i < g_cand_count; i++) free(g_candidates[i]);
        free(g_candidates);
        g_candidates = NULL;
    }
    g_cand_count     = 0;
    g_cand_idx       = 0;
    g_cand_relative  = 0;
    g_cand_dot_slash = 0;

    /* Handle ./ prefix */
    const char *effective = text;
    if (text[0] == '.' && text[1] == '/') {
        g_cand_dot_slash = 1;
        effective = text + 2;
    }

    int          is_absolute = (effective[0] == '/');
    path_node_t *parent      = NULL;
    const char  *partial     = "";
    char         cand_prefix[FLUXIPC_PATH_MAX] = "";  /* prepended to every candidate */

    if (is_absolute) {
        /* Split at last / to find parent node and partial component */
        if (strcmp(effective, "/") == 0) {
            parent  = g_root;
            partial = "";
        } else {
            char buf[FLUXIPC_PATH_MAX];
            snprintf(buf, sizeof(buf), "%s", effective);
            char *last_slash = strrchr(buf, '/');

            if (last_slash == buf) {
                /* "/foo" or "/foo/bar" */
                partial = last_slash + 1;
                char *inner = strchr(partial, '/');
                if (inner) {
                    *inner = '\0';
                    parent = node_lookup(partial);
                    partial = inner + 1;
                } else {
                    parent = g_root;
                }
            } else {
                /* "/a/b/partial" – parent is everything before last / */
                *last_slash = '\0';
                parent = node_lookup(buf);
                partial = last_slash + 1;
            }
        }
        g_cand_relative = 0;
    } else {
        /* Relative completion – may contain / for nested paths */
        char buf[FLUXIPC_PATH_MAX];
        snprintf(buf, sizeof(buf), "%s", effective);
        char *slash = strchr(buf, '/');

        if (slash) {
            /* Split at last /: walk consumed path, remainder is partial */
            char *last_slash = strrchr(buf, '/');
            *last_slash = '\0';
            partial = last_slash + 1;

            path_node_t *walked = node_lookup_rel(buf);
            if (walked) {
                parent = walked;
                /* cand_prefix = ["./"] + consumed_path + "/" */
                size_t off = 0;
                if (g_cand_dot_slash) {
                    cand_prefix[0] = '.'; cand_prefix[1] = '/'; off = 2;
                }
                size_t blen = strlen(buf);
                if (off + blen < sizeof(cand_prefix) - 1) {
                    memcpy(cand_prefix + off, buf, blen);
                    off += blen;
                    cand_prefix[off++] = '/';
                    cand_prefix[off] = '\0';
                } else {
                    cand_prefix[sizeof(cand_prefix) - 1] = '\0';
                }
            }
            /* if walked == NULL, parent stays NULL → no candidates */
        } else {
            parent  = g_cwd_node;
            partial = effective;
            if (g_cand_dot_slash)
                snprintf(cand_prefix, sizeof(cand_prefix), "./");
        }
        g_cand_relative = 1;
    }

    if (!parent) {
        g_candidates = calloc(1, sizeof(char *));
        return;
    }

    size_t plen     = strlen(partial);
    int    max_cand = parent->child_count + 1;
    g_candidates = calloc((size_t)max_cand, sizeof(char *));
    if (!g_candidates) return;

    for (path_node_t *c = parent->first_child; c; c = c->next_sibling) {
        if (plen > 0 && strncmp(c->name, partial, plen) != 0)
            continue;

        size_t name_len = strlen(c->name);
        char *cand;
        if (cand_prefix[0]) {
            /* Prepend consumed path prefix (with or without ./) */
            size_t clen = strlen(cand_prefix) + name_len + 1;
            cand = malloc(clen);
            if (cand)
                snprintf(cand, clen, "%s%s", cand_prefix, c->name);
        } else if (is_absolute) {
            if (parent == g_root)
                cand = malloc(name_len + 2);
            else
                cand = malloc(strlen(parent->full_path) + 1 + name_len + 1);
            if (!cand) continue;
            if (parent == g_root)
                snprintf(cand, name_len + 2, "/%s", c->name);
            else
                snprintf(cand, strlen(parent->full_path) + 1 + name_len + 1,
                         "%s/%s", parent->full_path, c->name);
        } else {
            cand = strdup(c->name);
        }
        if (cand)
            g_candidates[g_cand_count++] = cand;
    }

    if (g_cand_count > 1)
        qsort(g_candidates, (size_t)g_cand_count, sizeof(char *), cmp_str);
}

static char *path_generator(const char *text, int state)
{
    if (!state) {
        build_candidates(text);
        g_cand_idx = 0;
    }
    while (g_cand_idx < g_cand_count) {
        char *c = g_candidates[g_cand_idx++];
        if (c) return strdup(c);
    }
    return NULL;
}

/* ─── Built-in command completion ─────────────────────────────────────────── */

static const char *g_builtins[] = {
    "ls", "help", "exit", "reload", "watch", "cd", NULL
};

static char *builtin_generator(const char *text, int state)
{
    static int    idx;
    static size_t tlen;
    if (!state) { idx = 0; tlen = strlen(text); }
    while (g_builtins[idx]) {
        const char *cmd = g_builtins[idx++];
        if (strncmp(cmd, text, tlen) == 0)
            return strdup(cmd);
    }
    return NULL;
}

/* ─── Filesystem path completion (for ! prefix) ──────────────────────────── */

static char *fs_path_generator(const char *text, int state)
{
    static const char *stripped;
    if (!state) {
        stripped = text + 1;
        if (*stripped == '\0') stripped = "";
    }
    char *m = rl_filename_completion_function(stripped, state);
    if (!m) return NULL;
    size_t mlen = strlen(m);
    char *r = malloc(mlen + 2);
    if (!r) { free(m); return NULL; }
    r[0] = '!';
    memcpy(r + 1, m, mlen + 1);
    free(m);
    return r;
}

static int token_index_at(int pos)
{
    const char *line = rl_line_buffer;
    int tok = 0, i = 0;
    while (i < pos) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (i >= pos) break;
        while (line[i] != ' ' && line[i] != '\t' && line[i] != '\0') i++;
        if (i <= pos) tok++;
    }
    return tok;
}

static int first_word_is(const char *cmd)
{
    const char *line = rl_line_buffer;
    while (*line == ' ' || *line == '\t') line++;
    size_t clen = strlen(cmd);
    if (strncmp(line, cmd, clen) != 0) return 0;
    return line[clen] == ' ' || line[clen] == '\t' || line[clen] == '\0';
}

static char **fluxipc_completer(const char *text, int start, int end)
{
    (void)end;
    rl_attempted_completion_over = 1;

    /* ! prefix: filesystem path completion for any token */
    if (text[0] == '!') {
        char **matches = rl_completion_matches(text, fs_path_generator);
        if (matches && matches[0]) {
            const char *path = matches[0];
            if (path[0] == '!') path++;
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                rl_completion_append_character = '/';
            else
                rl_completion_append_character = ' ';
        }
        return matches;
    }

    /* Second+ token: path completion for commands that take a path arg */
    if (start > 0) {
        int tok = token_index_at(start);
        if ((first_word_is("help")  && tok >= 1) ||
            (first_word_is("watch") && tok >= 1) ||
            (first_word_is("cd")    && tok >= 1))
            goto do_path;

        /* Show usage when tabbing after a completed leaf path */
        if (text[0] == '\0') {
            const char *p = rl_line_buffer;
            while (*p == ' ' || *p == '\t') p++;
            const char *e = p;
            while (*e != ' ' && *e != '\t' && *e != '\0') e++;
            char first_tok[FLUXIPC_PATH_MAX];
            size_t flen = (size_t)(e - p);
            if (flen < sizeof(first_tok)) {
                memcpy(first_tok, p, flen);
                first_tok[flen] = '\0';
                path_node_t *n = node_lookup_rel(first_tok);
                if (n && n->is_leaf && n->usage[0]) {
                    printf("\n  \033[2m%s\033[0m\n", n->usage);
                    rl_on_new_line();
                }
            }
        }

        return NULL;
    }

    /* First token: builtins first, then relative path */
    if (text[0] != '/') {
        char **matches = rl_completion_matches(text, builtin_generator);
        if (matches && matches[0]) {
            rl_completion_append_character = ' ';
            return matches;
        }
        goto do_path;
    }

do_path:
    {
        char **matches = rl_completion_matches(text, path_generator);
        path_node_t *node = NULL;
        if (matches && matches[0]) {
            const char *cname = matches[0];
            if (g_cand_dot_slash && cname[0] == '.' && cname[1] == '/')
                cname += 2;

            if (g_cand_relative || g_cand_dot_slash) {
                node = (strchr(cname, '/'))
                     ? node_lookup_rel(cname)
                     : NULL;
                if (!node) {
                    for (path_node_t *c = g_cwd_node->first_child;
                         c; c = c->next_sibling) {
                        if (strcmp(c->name, cname) == 0) { node = c; break; }
                    }
                }
            } else {
                node = node_lookup(cname);
            }
            rl_completion_append_character = (node && node->is_leaf) ? ' ' : '/';
        }

        /* Show usage when tabbing on an exact leaf path */
        if (node && node->is_leaf && node->usage[0]) {
            printf("\n  \033[2m%s\033[0m\n", node->usage);
            rl_on_new_line();
        }

        return matches;
    }
}

/* ─── Built-in commands ───────────────────────────────────────────────────── */

static void cmd_ls(void)
{
    printf("\n%-42s  %-6s  %s\n", "PATH", "ID", "USAGE");
    printf("%-42s  %-6s  %s\n",
           "------------------------------------------",
           "------", "-----------------------------");

    path_node_t *leaves[4096];
    int count = 0;
    collect_leaves(g_cwd_node, leaves, &count, 4096);

    if (count == 0) {
        printf("  (no entries under %s)\n\n",
               g_cwd_node == g_root ? "/" : g_cwd_node->full_path);
        return;
    }

    for (int i = 0; i < count; i++) {
        path_node_t *n = leaves[i];
        printf("%-42s  %-6u  %s\n",
               rel_path(n), n->id,
               n->usage[0] ? n->usage : "-");
    }
    printf("\n");
}

static void cmd_help(const char *arg)
{
    if (!arg || !arg[0]) {
        printf("\n  Built-in commands:\n\n");
        printf("  %-12s – list endpoints under current namespace\n", "ls");
        printf("  %-12s – show this help\n", "help");
        printf("  %-12s – change namespace (cd / to return to root)\n",
               "cd <path>");
        printf("  %-12s – refresh registry from shared memory\n", "reload");
        printf("  %-12s – call endpoint every s seconds (default 1)\n",
               "watch [s] <p>");
        printf("  %-12s – quit the shell\n\n", "exit");
        return;
    }

    path_node_t *node = node_lookup_rel(arg);
    if (node && node->is_leaf)
        printf("\n  %s\n\n",
               node->usage[0] ? node->usage : "(no description)");
    else
        printf("  No entry for '%s'\n\n", arg);
}

/* ─── watch ───────────────────────────────────────────────────────────────── */

static volatile int g_watch_quit = 0;
static void watch_sig_handler(int s) { (void)s; g_watch_quit = 1; }

static void cmd_watch(int ntok, char **tokens)
{
    if (ntok < 2) {
        printf("  Usage: watch [<seconds>] <path> [args...]\n"
               "         seconds defaults to 1 if omitted\n\n");
        return;
    }

    int interval;
    int path_idx;

    char *endp;
    long maybe_interval = strtol(tokens[1], &endp, 10);
    if (*endp == '\0' && maybe_interval > 0) {
        if (ntok < 3) {
            printf("  Usage: watch [<seconds>] <path> [args...]\n\n");
            return;
        }
        interval = (int)maybe_interval;
        path_idx = 2;
    } else {
        interval = 1;
        path_idx = 1;
    }

    path_node_t *node = node_lookup_rel(tokens[path_idx]);
    if (!node || !node->is_leaf) {
        printf("  Unknown path: %s\n\n", tokens[path_idx]);
        return;
    }

    int   call_argc = ntok - path_idx;
    char *watch_argv[64];
    watch_argv[0] = node->full_path;
    for (int i = path_idx + 1; i < ntok; i++)
        watch_argv[i - path_idx] = tokens[i];

    void (*old_sig)(int) = signal(SIGINT, watch_sig_handler);
    printf("  Watching %s every %ds (Ctrl+C to stop)\n\n",
           node->full_path, interval);
    g_watch_quit = 0;

    int count = 0;
    while (!g_watch_quit) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        printf("\033[1m[%02d:%02d:%02d] #%d\033[0m ",
               tm->tm_hour, tm->tm_min, tm->tm_sec, ++count);

        char out[64 * 1024];
        size_t out_len = 0;
        int rc = fluxipc_call(node->sock_path, node->full_path,
                              call_argc, watch_argv,
                              out, sizeof(out), &out_len);
        if (rc < 0)
            printf("Error %d: %s\n", rc, strerror(-rc));
        else if (out_len > 0)
            fwrite(out, 1, out_len, stdout);
        else
            printf("(empty)");

        if (g_watch_quit) break;
        fflush(stdout);
        sleep((unsigned)interval);
    }

    signal(SIGINT, old_sig);
    printf("\n");
}

#define MAX_TOK 64

/* ─── Inline range expansion ──────────────────────────────────────────────
 *
 * Syntax:
 *   start:end          step defaults to 1 (or -1 if start > end)
 *   start:end:step     explicit step
 *
 * Examples:
 *   0:100         →  0 1 2 … 100          (step=1 implied)
 *   0:100:5       →  0 5 10 … 100
 *   1.0:2.0:0.1   →  1.0 1.1 … 2.0
 *   10:0:2        →  error: step must be negative for descending range
 *   10:0:-2       →  10 8 6 4 2 0
 *
 * Escape (pass literal colon string to IPC handler):
 *   \0:100        →  "0:100"   (backslash prefix)
 *   '0:100:5'     →  "0:100:5" (single-quote wrap)
 *
 * Detection heuristic:
 *   1 or 2 colons, every segment is a valid decimal number, no escape prefix.
 *
 * Multiple range args produce a cartesian-product sweep:
 *   /demo/arithmetic/add  1:3  10:30:10
 *   → (1,10)(1,20)(1,30)(2,10)(2,20)(2,30)(3,10)(3,20)(3,30)
 * ──────────────────────────────────────────────────────────────────────── */

#include <math.h>   /* fabs */

/* Maximum values per single range axis */
#define RANGE_MAX_VALS 4096

typedef struct {
    int    is_range;       /* 1 = range spec, 0 = literal string          */
    int    escaped;        /* 1 = was prefixed with \, send stripped form  */
    char   literal[256];   /* used when is_range==0                        */
    double *vals;          /* allocated array when is_range==1             */
    int    nvals;
} arg_spec_t;

/* Return 1 if s is a valid decimal number (allows leading sign, one dot) */
static int is_number(const char *s)
{
    if (!s || !*s) return 0;
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    if (!*p) return 0;
    int digits = 0, dots = 0;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') { digits++; continue; }
        if (*p == '.'  && dots == 0) { dots++;  continue; }
        return 0;
    }
    return digits > 0;
}

/* Parse token into arg_spec_t.  Caller must free spec->vals if is_range.
 *
 * Range formats accepted:
 *   "start:end"        – step defaults to +1 or -1 based on direction
 *   "start:end:step"   – explicit step (may be fractional or negative)
 */
static void parse_arg(const char *tok, arg_spec_t *spec)
{
    memset(spec, 0, sizeof(*spec));

    /* Escape: leading backslash → strip it, treat as literal */
    if (tok[0] == '\\') {
        spec->is_range = 0;
        spec->escaped  = 1;
        snprintf(spec->literal, sizeof(spec->literal), "%s", tok + 1);
        return;
    }

    /* Strip surrounding single-quotes → literal */
    size_t tlen = strlen(tok);
    if (tlen >= 2 && tok[0] == '\'' && tok[tlen-1] == '\'') {
        spec->is_range = 0;
        size_t inner = tlen - 2;
        if (inner >= sizeof(spec->literal)) inner = sizeof(spec->literal)-1;
        memcpy(spec->literal, tok + 1, inner);
        spec->literal[inner] = '\0';
        return;
    }

    /* Count colons */
    int colon_count = 0;
    for (const char *p = tok; *p; p++)
        if (*p == ':') colon_count++;

    /* Must have 1 or 2 colons to be a range candidate */
    if (colon_count < 1 || colon_count > 2) {
        spec->is_range = 0;
        snprintf(spec->literal, sizeof(spec->literal), "%s", tok);
        return;
    }

    /* Split and validate segments */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", tok);

    char *seg_start = tmp;
    char *c1 = strchr(seg_start, ':');
    if (!c1) goto literal;
    *c1 = '\0';
    char *seg_end = c1 + 1;

    char *seg_step = NULL;
    if (colon_count == 2) {
        char *c2 = strchr(seg_end, ':');
        if (!c2) goto literal;
        *c2 = '\0';
        seg_step = c2 + 1;
    }

    /* All present segments must be valid numbers */
    if (!is_number(seg_start) || !is_number(seg_end)) goto literal;
    if (seg_step && !is_number(seg_step))              goto literal;

    {
        double vstart = strtod(seg_start, NULL);
        double vend   = strtod(seg_end,   NULL);
        double vstep;

        if (seg_step) {
            vstep = strtod(seg_step, NULL);
        } else {
            /* Default step: +1 ascending, -1 descending */
            vstep = (vend >= vstart) ? 1.0 : -1.0;
        }

        if (vstep == 0.0) {
            printf("  Range error: step is zero in '%s'\n", tok);
            goto literal;
        }

        /* Direction sanity */
        if ((vstep > 0 && vstart > vend) || (vstep < 0 && vstart < vend)) {
            printf("  Range error: step direction mismatch in '%s'\n"
                   "  Hint: use negative step for descending range, e.g. %g:%g:-1\n",
                   tok, vstart, vend);
            goto literal;
        }

        /* Generate values via index arithmetic (no float drift) */
        spec->vals = malloc(sizeof(double) * RANGE_MAX_VALS);
        if (!spec->vals) { spec->is_range = 0; return; }

        spec->nvals   = 0;
        spec->is_range = 1;

        for (int idx = 0; spec->nvals < RANGE_MAX_VALS; idx++) {
            double v = vstart + idx * vstep;
            if (vstep > 0 && v > vend + fabs(vstep) * 0.5) break;
            if (vstep < 0 && v < vend - fabs(vstep) * 0.5) break;
            spec->vals[spec->nvals++] = v;
        }
    }
    return;

literal:
    spec->is_range = 0;
    snprintf(spec->literal, sizeof(spec->literal), "%s", tok);
}

static void free_arg_spec(arg_spec_t *spec)
{
    if (spec->is_range && spec->vals) { free(spec->vals); spec->vals = NULL; }
}

/*
 * Render a double to a compact string: if it's a whole number, no decimal
 * point; otherwise up to 10 significant digits, trailing zeros stripped.
 */
static void fmt_val(double v, char *buf, size_t cap)
{
    if (v == (long long)v && fabs(v) < 1e15)
        snprintf(buf, cap, "%lld", (long long)v);
    else {
        snprintf(buf, cap, "%.10g", v);
    }
}

/*
 * Recursive cartesian-product executor.
 * arg_specs[0..nargs-1] describe each argument position.
 * call_argv[0] = path (fixed), call_argv[1..nargs] = current values.
 * depth = current arg index (0-based).
 */
static int g_sweep_count;
static int g_sweep_errors;

static void sweep_recurse(path_node_t *node,
                           arg_spec_t *specs, int nargs,
                           char **call_argv, char (*val_bufs)[32],
                           int depth)
{
    if (depth == nargs) {
        /* All args fixed — fire the call */
        char out[64 * 1024];
        size_t out_len = 0;

        /* Build compact label showing all varying args */
        char label[256] = "";
        int loff = 0;
        for (int i = 0; i < nargs; i++) {
            if (specs[i].is_range)
                loff += snprintf(label + loff, sizeof(label) - (size_t)loff,
                                 "%s%s", i ? "," : "", call_argv[i + 1]);
        }

        int rc = fluxipc_call(node->sock_path, node->full_path,
                              nargs + 1, call_argv,
                              out, sizeof(out), &out_len);
        g_sweep_count++;
        printf("  [%4d] (%s) → ", g_sweep_count, label);
        if (rc < 0) {
            printf("Error %d: %s\n", rc, strerror(-rc));
            g_sweep_errors++;
        } else if (out_len > 0) {
            /* Print response inline, trimming trailing newline */
            while (out_len > 0 && (out[out_len-1] == '\n' || out[out_len-1] == '\r'))
                out_len--;
            fwrite(out, 1, out_len, stdout);
            putchar('\n');
        } else {
            printf("(ok)\n");
        }
        return;
    }

    arg_spec_t *spec = &specs[depth];

    if (!spec->is_range) {
        /* Literal arg — just set and recurse */
        call_argv[depth + 1] = spec->literal;
        sweep_recurse(node, specs, nargs, call_argv, val_bufs, depth + 1);
    } else {
        /* Range arg — iterate over all values */
        for (int i = 0; i < spec->nvals; i++) {
            fmt_val(spec->vals[i], val_bufs[depth], 32);
            call_argv[depth + 1] = val_bufs[depth];
            sweep_recurse(node, specs, nargs, call_argv, val_bufs, depth + 1);
        }
    }
}

/*
 * Count how many range args exist among specs[0..nargs-1].
 * Returns total number of calls = product of all range lengths.
 */
static long long sweep_call_count(arg_spec_t *specs, int nargs)
{
    long long total = 1;
    for (int i = 0; i < nargs; i++)
        if (specs[i].is_range) total *= specs[i].nvals;
    return total;
}

/*
 * Main inline-range dispatch: called from dispatch() when any argument
 * (tokens[1..ntok-1]) contains a range spec, and cmd resolves to a leaf.
 *
 * Prints a summary header, runs the cartesian product, then a footer.
 */
static void dispatch_with_ranges(path_node_t *node,
                                  char **tokens, int ntok)
{
    int nargs = ntok - 1;   /* number of positional args (excluding path) */
    arg_spec_t specs[MAX_TOK];
    char val_bufs[MAX_TOK][32];

    /* Parse all argument tokens */
    int has_range = 0;
    for (int i = 0; i < nargs; i++) {
        parse_arg(tokens[i + 1], &specs[i]);
        if (specs[i].is_range) has_range++;
    }

    if (!has_range) {
        /* Shouldn't reach here, but handle gracefully */
        for (int i = 0; i < nargs; i++) free_arg_spec(&specs[i]);
        return;
    }

    long long total = sweep_call_count(specs, nargs);
    if (total > 100000) {
        printf("  Sweep would make %lld calls — too many (limit 100000).\n"
               "  Reduce range or step size.\n\n", total);
        for (int i = 0; i < nargs; i++) free_arg_spec(&specs[i]);
        return;
    }

    /* Print header */
    printf("  Sweep %s  [%d range arg(s), %lld total call(s)]\n",
           node->full_path, has_range, total);
    for (int i = 0; i < nargs; i++) {
        if (specs[i].is_range) {
            double actual_step = (specs[i].nvals > 1)
                                 ? specs[i].vals[1] - specs[i].vals[0]
                                 : 0.0;
            printf("    arg[%d]: %d values  %g:%g:%g\n",
                   i + 1,
                   specs[i].nvals,
                   specs[i].vals[0],
                   specs[i].vals[specs[i].nvals - 1],
                   actual_step);
        } else {
            printf("    arg[%d]: literal \"%s\"\n", i + 1, specs[i].literal);
        }
    }
    printf("\n");

    /* Build call_argv: argv[0] = path, argv[1..nargs] = filled by recurse */
    char *call_argv[MAX_TOK + 1];
    call_argv[0] = node->full_path;

    g_sweep_count  = 0;
    g_sweep_errors = 0;

    sweep_recurse(node, specs, nargs, call_argv, val_bufs, 0);

    printf("\n  Sweep done: %d calls, %d error(s).\n\n",
           g_sweep_count, g_sweep_errors);

    for (int i = 0; i < nargs; i++) free_arg_spec(&specs[i]);
}

/* ─── Dispatch ────────────────────────────────────────────────────────────── */

static void dispatch(const char *line)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", line);

    char *tokens[MAX_TOK];
    int ntok = 0;
    char *p = strtok(buf, " \t");
    while (p && ntok < MAX_TOK) { tokens[ntok++] = p; p = strtok(NULL, " \t"); }
    if (ntok == 0) return;

    /* ! prefix on first token: execute as system command */
    if (tokens[0][0] == '!') {
        if (tokens[0][1] == '\0') {
            printf("  Usage: !<command> [args...]\n\n");
            return;
        }
        tokens[0]++;           /* strip '!' */
        for (int i = 1; i < ntok; i++)
            if (tokens[i][0] == '!') tokens[i]++;  /* strip '!' from args */
        tokens[ntok] = NULL;   /* execvp needs NULL terminator */
        pid_t pid = fork();
        if (pid == 0) {
            execvp(tokens[0], tokens);
            fprintf(stderr, "  exec: %s\n", strerror(errno));
            _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
                printf("  exit: %d\n", WEXITSTATUS(status));
        } else {
            printf("  fork: %s\n", strerror(errno));
        }
        printf("\n");
        return;
    }

    const char *cmd = tokens[0];

    if (strcmp(cmd, "ls")     == 0) { cmd_ls(); return; }
    if (strcmp(cmd, "reload") == 0) { load_all_shm();
                                      printf("  Registry reloaded.\n\n"); return; }
    if (strcmp(cmd, "watch")  == 0) { cmd_watch(ntok, tokens); return; }
    if (strcmp(cmd, "help")   == 0) { cmd_help(ntok > 1 ? tokens[1] : NULL); return; }
    if (strcmp(cmd, "cd")     == 0) { cmd_cd(ntok > 1 ? tokens[1] : NULL); return; }
    if (strcmp(cmd, "exit")   == 0) { printf("Bye.\n"); exit(0); }

    /* Resolve as a path (absolute or relative to cwd) */
    path_node_t *node = node_lookup_rel(cmd);
    if (!node) {
        printf("  Unknown path: %s  (try 'ls' or Tab)\n\n", cmd);
        return;
    }

    if (!node->is_leaf) {
        /* List leaf entries under this namespace */
        path_node_t *leaves[4096];
        int count = 0;
        collect_leaves(node, leaves, &count, 4096);
        for (int i = 0; i < count; i++)
            printf("  %s\n", rel_path(leaves[i]));
        if (count == 0)
            printf("  (no leaf entries under %s)\n", node->full_path);
        printf("\n");
        return;
    }

    /* ── Inline range detection ─────────────────────────────────────────────
     * Format: start:end  or  start:end:step
     * A token is a range if it has 1 or 2 colons and every colon-separated
     * segment is a valid decimal number (no escape prefix).
     * ──────────────────────────────────────────────────────────────────────*/
    if (ntok > 1) {
        int has_range = 0;
        for (int i = 1; i < ntok && !has_range; i++) {
            const char *tok = tokens[i];
            /* Skip escaped tokens */
            if (tok[0] == '\\' || (tok[0] == '\'' && tok[strlen(tok)-1] == '\''))
                continue;
            /* Count colons */
            int nc = 0;
            for (const char *q = tok; *q; q++) if (*q == ':') nc++;
            if (nc < 1 || nc > 2) continue;

            /* Validate all segments are numbers */
            char tmp[256]; snprintf(tmp, sizeof(tmp), "%s", tok);
            char *s1 = tmp;
            char *c1 = strchr(s1, ':'); if (!c1) continue;
            *c1 = '\0'; char *s2 = c1 + 1;
            if (!is_number(s1)) continue;
            if (nc == 2) {
                /* Split s2 at the second colon before validating */
                char *c2 = strchr(s2, ':'); if (!c2) continue;
                *c2 = '\0'; char *s3 = c2 + 1;
                if (!is_number(s2) || !is_number(s3)) continue;
            } else {
                if (!is_number(s2)) continue;
            }
            has_range = 1;
        }
        if (has_range) {
            dispatch_with_ranges(node, tokens, ntok);
            return;
        }
    }

    /* ── Normal single call ─────────────────────────────────────────────── */
    char *call_argv[MAX_TOK];
    call_argv[0] = node->full_path;
    for (int i = 1; i < ntok; i++) {
        /* Strip escape prefix / quotes before sending to handler */
        const char *tok = tokens[i];
        if (tok[0] == '\\') {
            tokens[i]++;          /* skip backslash in-place */
        } else if (tok[0] == '!') {
            tokens[i]++;          /* strip ! prefix */
        } else {
            size_t tl = strlen(tok);
            if (tl >= 2 && tok[0] == '\'' && tok[tl-1] == '\'') {
                tokens[i][tl-1] = '\0';
                tokens[i]++;
            }
        }
        call_argv[i] = tokens[i];
    }

    char out[64 * 1024];
    size_t out_len = 0;
    int rc = fluxipc_call(node->sock_path, node->full_path,
                          ntok, call_argv, out, sizeof(out), &out_len);
    if (rc < 0)
        printf("  Error %d: %s\n\n", rc, strerror(-rc));
    else {
        if (out_len > 0) { fwrite(out, 1, out_len, stdout); putchar('\n'); }
        else printf("  OK (no output)\n\n");
    }
}

/* ─── fluxipc_interactive_init ────────────────────────────────────────────── */

int fluxipc_interactive_init(const char *prog_name)
{
    (void)prog_name;
    tree_init();
    update_prompt();
    load_all_shm();

    rl_attempted_completion_function = fluxipc_completer;
    rl_completion_append_character   = '\0';
    using_history();

    printf("\nFluxIPC interactive shell\n");
    printf("  Tab              – complete path (relative to cwd, use ./ to force)\n");
    printf("  cd <path>        – change namespace (cd / to return to root)\n");
    printf("  ls               – show endpoints under current namespace\n");
    printf("  help [path]      – show built-in commands, or details for one endpoint\n");
    printf("  reload           – refresh registry from shared memory\n");
    printf("  watch [s] <path> – call endpoint every s seconds (default 1)\n");
    printf("  exit             – quit\n");
    printf("\nRange syntax (inline per argument):\n");
    printf("  start:end          step=1 (or -1 if descending)\n");
    printf("  start:end:step     explicit step (float/negative ok)\n\n");

    char *line;
    while ((line = readline(g_prompt)) != NULL) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t l = strlen(s);
        while (l > 0 && (s[l - 1] == ' ' || s[l - 1] == '\t'
                         || s[l - 1] == '\n'))
            s[--l] = '\0';
        if (*s) { add_history(s); dispatch(s); }
        free(line);
    }
    printf("\n");
    return 0;
}