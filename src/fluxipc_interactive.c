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

    DIR *d = opendir("/run");
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
            snprintf(sub, sizeof(sub), "/run/%s", ent->d_name);
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

    /* Second+ token: path completion for commands that take a path arg */
    if (start > 0) {
        int tok = token_index_at(start);
        if ((first_word_is("help")  && tok >= 1) ||
            (first_word_is("watch") && tok >= 1) ||
            (first_word_is("cd")    && tok >= 1))
            goto do_path;
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
        if (matches && matches[0]) {
            const char *cname = matches[0];
            if (g_cand_dot_slash && cname[0] == '.' && cname[1] == '/')
                cname += 2;

            path_node_t *node = NULL;
            if (g_cand_relative || g_cand_dot_slash) {
                /* cname may be multi-level (e.g. "stub/primary") */
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
        return matches;
    }
}

/* ─── Hint on '/' key ─────────────────────────────────────────────────────── */

static int hint_on_slash(int count, int key)
{
    (void)count;
    rl_insert_text("/");

    const char *line = rl_line_buffer;
    int pos = rl_point;
    if (pos < 2) return 0;

    char tok[FLUXIPC_PATH_MAX];
    const char *start = line;
    while (*start == ' ' || *start == '\t') start++;
    size_t len = (size_t)(line + pos - start);
    if (len >= sizeof(tok)) return 0;
    memcpy(tok, start, len);
    tok[len] = '\0';

    path_node_t *node = node_lookup_rel(tok);
    if (node && node->is_leaf && node->usage[0]) {
        printf("\n  \033[2m%s\033[0m\n", node->usage);
        rl_on_new_line();
    }

    (void)key;
    return 0;
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

/* ─── Dispatch ────────────────────────────────────────────────────────────── */

#define MAX_TOK 64

static void dispatch(const char *line)
{
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", line);

    char *tokens[MAX_TOK];
    int ntok = 0;
    char *p = strtok(buf, " \t");
    while (p && ntok < MAX_TOK) { tokens[ntok++] = p; p = strtok(NULL, " \t"); }
    if (ntok == 0) return;

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

    /* Show usage hint on bare-name invocation */
    if (ntok == 1 && node->usage[0])
        printf("  \033[2mUsage: %s %s\033[0m\n\n", node->full_path, node->usage);

    char *call_argv[MAX_TOK];
    call_argv[0] = node->full_path;
    for (int i = 1; i < ntok; i++) call_argv[i] = tokens[i];

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
    rl_bind_key('/', hint_on_slash);
    using_history();

    printf("\nFluxIPC interactive shell\n");
    printf("  Tab              – complete path (relative to cwd, use ./ to force)\n");
    printf("  cd <path>        – change namespace (cd / to return to root)\n");
    printf("  ls               – show endpoints under current namespace\n");
    printf("  help [path]      – show built-in commands, or details for one endpoint\n");
    printf("  reload           – refresh registry from shared memory\n");
    printf("  watch [s] <path> – call endpoint every s seconds (default 1)\n");
    printf("  exit             – quit\n\n");

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
