/*
 * Property-based tests for locate() in
 * eval/extent/optimization/path_handling.c
 *
 * Oracle: reference path traversal plus lock-coupling contract. Starting from a
 * locked inode, an empty path returns the original inode still locked; a fully
 * present path returns the final inode locked and unlocks intermediates; a miss
 * returns NULL and releases every lock held by the traversal. Directory lookup
 * is checked against the generated tree shape rather than the implementation.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "path_handling.h"

#define MAX_LOCATE_DEPTH 8U
#define MAX_COMPONENT_LEN 12U
#define MAX_LOCK_EVENTS ((MAX_LOCATE_DEPTH * 2U) + 4U)

struct locate_case {
    unsigned path_len;
    unsigned missing_at;
    int has_missing;
    int empty_path;
    char names[MAX_LOCATE_DEPTH][MAX_COMPONENT_LEN + 1U];
    char miss_name[MAX_COMPONENT_LEN + 1U];
};

struct lock_event {
    char op;
    struct inode *node;
};

static struct lock_event lock_events[MAX_LOCK_EVENTS];
static unsigned lock_event_count;
static int lock_balance[MAX_LOCATE_DEPTH + 1U];
static struct inode *tracked_nodes[MAX_LOCATE_DEPTH + 1U];
static unsigned tracked_node_count;
static int lock_error;

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static void gen_name(struct theft *t, char out[MAX_COMPONENT_LEN + 1U], const char *prefix)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_-";
    unsigned prefix_len = (unsigned)strlen(prefix);
    unsigned max_suffix = MAX_COMPONENT_LEN - prefix_len;
    unsigned suffix_len = max_suffix == 0U ? 0U : 1U + bounded_choice(t, max_suffix);

    memcpy(out, prefix, prefix_len);
    for (unsigned i = 0; i < suffix_len; i++) {
        out[prefix_len + i] = alphabet[bounded_choice(t, (unsigned)(sizeof(alphabet) - 1U))];
    }
    out[prefix_len + suffix_len] = '\0';
}

static enum theft_alloc_res locate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct locate_case *lc = malloc(sizeof(*lc));
    if (lc == NULL) return THEFT_ALLOC_ERROR;
    memset(lc, 0, sizeof(*lc));

    lc->empty_path = theft_random_choice(t, 5) == 0;
    lc->path_len = lc->empty_path ? 0U : 1U + bounded_choice(t, MAX_LOCATE_DEPTH);
    lc->has_missing = !lc->empty_path && theft_random_choice(t, 3) == 0;
    lc->missing_at = lc->has_missing ? bounded_choice(t, lc->path_len) : lc->path_len;

    for (unsigned i = 0; i < lc->path_len; i++) {
        char prefix[4];
        snprintf(prefix, sizeof(prefix), "n%u", i % 10U);
        gen_name(t, lc->names[i], prefix);
    }
    gen_name(t, lc->miss_name, "miss");
    for (unsigned i = 0; i < lc->path_len; i++) {
        if (strcmp(lc->miss_name, lc->names[i]) == 0) {
            snprintf(lc->miss_name, sizeof(lc->miss_name), "miss_%u", i);
            lc->miss_name[MAX_COMPONENT_LEN] = '\0';
        }
    }

    *instance = lc;
    return THEFT_ALLOC_OK;
}

static void locate_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash locate_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct locate_case));
}

static void locate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct locate_case *lc = instance;
    fprintf(f, "{path=[");
    for (unsigned i = 0; i < lc->path_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0U ? "" : ", ", lc->names[i]);
    }
    fprintf(f, "], has_missing=%d, missing_at=%u, miss=\"%s\"}",
            lc->has_missing, lc->missing_at, lc->miss_name);
}

static struct theft_type_info locate_case_info = {
    .alloc = locate_case_alloc_cb,
    .free = locate_case_free_cb,
    .hash = locate_case_hash_cb,
    .print = locate_case_print_cb,
};

char **malloc_path(unsigned len)
{
    return calloc((size_t)len + 1U, sizeof(char *));
}

char *malloc_string(const char *name)
{
    size_t len = strlen(name);
    char *copy = malloc(len + 1U);
    if (copy == NULL) return NULL;
    memcpy(copy, name, len + 1U);
    return copy;
}

static int tracked_index(struct inode *node)
{
    for (unsigned i = 0; i < tracked_node_count; i++) {
        if (tracked_nodes[i] == node) return (int)i;
    }
    return -1;
}

static void record_lock_event(char op, struct inode *node)
{
    int index = tracked_index(node);
    if (index < 0) {
        lock_error = 1;
        return;
    }
    if (op == 'L') {
        lock_balance[index]++;
    } else {
        lock_balance[index]--;
        if (lock_balance[index] < 0) lock_error = 1;
    }
    if (lock_event_count < MAX_LOCK_EVENTS) {
        lock_events[lock_event_count].op = op;
        lock_events[lock_event_count].node = node;
        lock_event_count++;
    } else {
        lock_error = 1;
    }
}

void lock(struct inode *node)
{
    record_lock_event('L', node);
}

void unlock(struct inode *node)
{
    record_lock_event('U', node);
}

static unsigned bucket_for(char *name)
{
    unsigned hash = 0U;
    while (*name != '\0') {
        hash = hash * 131U + (unsigned char)*name;
        name++;
    }
    return hash & 0x1ffU;
}

struct inode *find(struct dirtb *dir, char *name)
{
    if (dir == NULL || name == NULL) return NULL;
    struct entry *entry = dir->tb[bucket_for(name) % DIRTB_NUM];
    while (entry != NULL) {
        if (strcmp(entry->name, name) == 0) return (struct inode *)entry->inum;
        entry = entry->next;
    }
    return NULL;
}

static void reset_lock_observation(struct inode *nodes, unsigned count)
{
    memset(lock_events, 0, sizeof(lock_events));
    memset(lock_balance, 0, sizeof(lock_balance));
    memset(tracked_nodes, 0, sizeof(tracked_nodes));
    lock_event_count = 0U;
    tracked_node_count = count;
    lock_error = 0;
    for (unsigned i = 0; i < count; i++) {
        tracked_nodes[i] = &nodes[i];
    }
    lock_balance[0] = 1;
}

static void build_tree(const struct locate_case *lc, struct inode nodes[MAX_LOCATE_DEPTH + 1U],
                       struct dirtb dirs[MAX_LOCATE_DEPTH + 1U],
                       struct entry entries[MAX_LOCATE_DEPTH])
{
    memset(nodes, 0, sizeof(struct inode) * (MAX_LOCATE_DEPTH + 1U));
    memset(dirs, 0, sizeof(struct dirtb) * (MAX_LOCATE_DEPTH + 1U));
    memset(entries, 0, sizeof(struct entry) * MAX_LOCATE_DEPTH);

    for (unsigned i = 0; i <= lc->path_len; i++) {
        nodes[i].mode = DIR_MODE;
        nodes[i].dir = &dirs[i];
        nodes[i].maj = i + 1U;
    }

    for (unsigned i = 0; i < lc->path_len; i++) {
        if (lc->has_missing && i == lc->missing_at) break;
        entries[i].name = (char *)lc->names[i];
        entries[i].inum = &nodes[i + 1U];
        unsigned bucket = bucket_for(entries[i].name) % DIRTB_NUM;
        entries[i].next = dirs[i].tb[bucket];
        dirs[i].tb[bucket] = &entries[i];
    }
}

static void make_path(const struct locate_case *lc, char *path[MAX_LOCATE_DEPTH + 1U])
{
    for (unsigned i = 0; i < lc->path_len; i++) {
        path[i] = (lc->has_missing && i == lc->missing_at) ? (char *)lc->miss_name : (char *)lc->names[i];
    }
    path[lc->path_len] = NULL;
}

static struct inode *reference_locate(struct inode *cur, char *path[])
{
    struct inode *current = cur;
    for (unsigned i = 0; path[i] != NULL; i++) {
        struct inode *next = find(current->dir, path[i]);
        if (next == NULL) return NULL;
        current = next;
    }
    return current;
}

static int balances_match_expected(const struct locate_case *lc)
{
    for (unsigned i = 0; i <= lc->path_len; i++) {
        int expected = 0;
        if (lc->path_len == 0U) {
            expected = i == 0U ? 1 : 0;
        } else if (!lc->has_missing) {
            expected = i == lc->path_len ? 1 : 0;
        }
        if (lock_balance[i] != expected) {
            fprintf(stderr, "lock balance node[%u]: got=%d expected=%d\n", i, lock_balance[i], expected);
            return 0;
        }
    }
    return !lock_error;
}

static int successful_lock_order_is_coupled(const struct locate_case *lc,
                                            struct inode nodes[MAX_LOCATE_DEPTH + 1U])
{
    if (lc->path_len == 0U) return lock_event_count == 0U;
    if (lock_event_count != lc->path_len * 2U) return 0;
    for (unsigned i = 0; i < lc->path_len; i++) {
        if (lock_events[2U * i].op != 'L' || lock_events[2U * i].node != &nodes[i + 1U]) return 0;
        if (lock_events[2U * i + 1U].op != 'U' || lock_events[2U * i + 1U].node != &nodes[i]) return 0;
    }
    return 1;
}

static int missing_lock_order_releases_current(const struct locate_case *lc,
                                               struct inode nodes[MAX_LOCATE_DEPTH + 1U])
{
    if (!lc->has_missing) return 1;
    if (lock_event_count != (lc->missing_at * 2U) + 1U) return 0;
    for (unsigned i = 0; i < lc->missing_at; i++) {
        if (lock_events[2U * i].op != 'L' || lock_events[2U * i].node != &nodes[i + 1U]) return 0;
        if (lock_events[2U * i + 1U].op != 'U' || lock_events[2U * i + 1U].node != &nodes[i]) return 0;
    }
    return lock_events[lock_event_count - 1U].op == 'U' &&
           lock_events[lock_event_count - 1U].node == &nodes[lc->missing_at];
}

static enum theft_trial_res prop_matches_reference_result(struct theft *t, void *arg1)
{
    (void)t;
    const struct locate_case *lc = arg1;
    struct inode nodes[MAX_LOCATE_DEPTH + 1U];
    struct dirtb dirs[MAX_LOCATE_DEPTH + 1U];
    struct entry entries[MAX_LOCATE_DEPTH];
    char *path[MAX_LOCATE_DEPTH + 1U];

    build_tree(lc, nodes, dirs, entries);
    make_path(lc, path);
    struct inode *expected = reference_locate(&nodes[0], path);
    reset_lock_observation(nodes, lc->path_len + 1U);

    struct inode *actual = locate(&nodes[0], path);
    if (actual != expected) {
        fprintf(stderr, "reference mismatch: actual=%p expected=%p path_len=%u\n",
                (void *)actual, (void *)expected, lc->path_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_success_leaves_only_result_locked(struct theft *t, void *arg1)
{
    (void)t;
    const struct locate_case *lc = arg1;
    if (lc->has_missing) return THEFT_TRIAL_SKIP;
    struct inode nodes[MAX_LOCATE_DEPTH + 1U];
    struct dirtb dirs[MAX_LOCATE_DEPTH + 1U];
    struct entry entries[MAX_LOCATE_DEPTH];
    char *path[MAX_LOCATE_DEPTH + 1U];

    build_tree(lc, nodes, dirs, entries);
    make_path(lc, path);
    reset_lock_observation(nodes, lc->path_len + 1U);

    struct inode *actual = locate(&nodes[0], path);
    if (actual != &nodes[lc->path_len] || !balances_match_expected(lc)) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_failure_releases_all_locks(struct theft *t, void *arg1)
{
    (void)t;
    const struct locate_case *lc = arg1;
    if (!lc->has_missing) return THEFT_TRIAL_SKIP;
    struct inode nodes[MAX_LOCATE_DEPTH + 1U];
    struct dirtb dirs[MAX_LOCATE_DEPTH + 1U];
    struct entry entries[MAX_LOCATE_DEPTH];
    char *path[MAX_LOCATE_DEPTH + 1U];

    build_tree(lc, nodes, dirs, entries);
    make_path(lc, path);
    reset_lock_observation(nodes, lc->path_len + 1U);

    struct inode *actual = locate(&nodes[0], path);
    if (actual != NULL || !balances_match_expected(lc)) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_lock_order_is_hand_over_hand(struct theft *t, void *arg1)
{
    (void)t;
    const struct locate_case *lc = arg1;
    struct inode nodes[MAX_LOCATE_DEPTH + 1U];
    struct dirtb dirs[MAX_LOCATE_DEPTH + 1U];
    struct entry entries[MAX_LOCATE_DEPTH];
    char *path[MAX_LOCATE_DEPTH + 1U];

    build_tree(lc, nodes, dirs, entries);
    make_path(lc, path);
    reset_lock_observation(nodes, lc->path_len + 1U);

    (void)locate(&nodes[0], path);
    if (lc->has_missing) {
        return missing_lock_order_releases_current(lc, nodes) ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
    }
    return successful_lock_order_is_coupled(lc, nodes) ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &locate_case_info },                               \
            .trials = trials_,                                                \
            .seed = theft_seed_of_time(),                                     \
        };                                                                    \
        enum theft_run_res res = theft_run(&cfg);                             \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) failures++;                                \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("locate property-based tests:\n");
    RUN_PROP("matches_reference_result", prop_matches_reference_result, 300);
    RUN_PROP("success_leaves_only_result_locked", prop_success_leaves_only_result_locked, 300);
    RUN_PROP("failure_releases_all_locks", prop_failure_releases_all_locks, 300);
    RUN_PROP("lock_order_is_hand_over_hand", prop_lock_order_is_hand_over_hand, 300);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
