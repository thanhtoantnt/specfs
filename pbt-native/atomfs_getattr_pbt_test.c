/*
 * Property-based tests for atomfs_getattr() in
 * eval/loc/gen/interface/atomfs_getattr.c
 *
 * Oracle: dependency-contract/reference orchestration. locate() is stubbed to
 * enforce its lock contract and to return either a generated target inode or
 * NULL; malloc_getattr_ret() is stubbed to record the exact attributes forwarded
 * by atomfs_getattr(). The real atomfs_getattr.c body is included below.
 */
#include <theft.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define _INTERFACE_H
#define MAX_GETATTR_DEPTH 6U
#define MAX_COMPONENT_LEN 12U

struct inode *root_inum;

void lock(struct inode *inum);
void unlock(struct inode *inum);
struct inode *locate(struct inode *cur, char *path[]);
struct getattr_ret *malloc_getattr_ret(struct inode *inum, unsigned mode,
                                       unsigned size, unsigned maj, unsigned min);

#include "../eval/loc/gen/interface/atomfs_getattr.c"

struct getattr_case {
    unsigned path_len;
    int locate_finds;
    int malloc_fails;
    unsigned mode;
    unsigned size;
    unsigned maj;
    unsigned min;
    char names[MAX_GETATTR_DEPTH][MAX_COMPONENT_LEN + 1U];
};

struct getattr_observation {
    const struct getattr_case *gc;
    struct inode *root;
    struct inode *target;
    char **path;
    unsigned lock_calls;
    unsigned unlock_calls;
    unsigned locate_calls;
    unsigned malloc_calls;
    int root_balance;
    int target_balance;
    int contract_error;
    struct inode *malloc_inum;
    unsigned malloc_mode;
    unsigned malloc_size;
    unsigned malloc_maj;
    unsigned malloc_min;
};

static struct getattr_observation obs;

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_edge_unsigned(struct theft *t)
{
    switch (theft_random_choice(t, 10U)) {
    case 0: return 0U;
    case 1: return 1U;
    case 2: return 2U;
    case 3: return UINT_MAX;
    case 4: return UINT_MAX - 1U;
    case 5: return UINT_MAX / 2U;
    default: return (unsigned)theft_random_choice(t, (uint64_t)UINT_MAX + 1ULL);
    }
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

static enum theft_alloc_res getattr_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct getattr_case *gc = calloc(1U, sizeof(*gc));
    if (gc == NULL) return THEFT_ALLOC_ERROR;

    gc->path_len = bounded_choice(t, MAX_GETATTR_DEPTH + 1U);
    gc->locate_finds = theft_random_choice(t, 4U) != 0;
    gc->malloc_fails = gc->locate_finds && theft_random_choice(t, 8U) == 0;

    static const unsigned modes[] = { FILE_MODE, DIR_MODE, CHR_MODE, BLK_MODE, SOCK_MODE, FIFO_MODE };
    gc->mode = modes[bounded_choice(t, (unsigned)(sizeof(modes) / sizeof(modes[0])))] ;
    gc->size = draw_edge_unsigned(t);
    gc->maj = draw_edge_unsigned(t);
    gc->min = draw_edge_unsigned(t);

    for (unsigned i = 0; i < gc->path_len; i++) {
        char prefix[4];
        snprintf(prefix, sizeof(prefix), "p%u", i % 10U);
        gen_name(t, gc->names[i], prefix);
    }

    *instance = gc;
    return THEFT_ALLOC_OK;
}

static void getattr_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash getattr_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct getattr_case));
}

static void getattr_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct getattr_case *gc = instance;
    fprintf(f, "{path=[");
    for (unsigned i = 0; i < gc->path_len; i++) {
        fprintf(f, "%s\"%s\"", i == 0U ? "" : ", ", gc->names[i]);
    }
    fprintf(f, "], locate_finds=%d, malloc_fails=%d, mode=%u, size=%u, maj=%u, min=%u}",
            gc->locate_finds, gc->malloc_fails, gc->mode, gc->size, gc->maj, gc->min);
}

static struct theft_type_info getattr_case_info = {
    .alloc = getattr_case_alloc_cb,
    .free = getattr_case_free_cb,
    .hash = getattr_case_hash_cb,
    .print = getattr_case_print_cb,
};

static int *balance_for(struct inode *inum)
{
    if (inum == obs.root) return &obs.root_balance;
    if (inum == obs.target) return &obs.target_balance;
    obs.contract_error = 1;
    return NULL;
}

void lock(struct inode *inum)
{
    int *balance = balance_for(inum);
    obs.lock_calls++;
    if (balance == NULL) return;
    (*balance)++;
}

void unlock(struct inode *inum)
{
    int *balance = balance_for(inum);
    obs.unlock_calls++;
    if (balance == NULL) return;
    (*balance)--;
    if (*balance < 0) obs.contract_error = 1;
}

struct inode *locate(struct inode *cur, char *path[])
{
    obs.locate_calls++;
    if (cur != obs.root || path != obs.path || obs.root_balance <= 0) {
        obs.contract_error = 1;
    }

    if (!obs.gc->locate_finds) {
        unlock(cur);
        return NULL;
    }

    if (obs.gc->path_len == 0U) {
        return cur;
    }

    lock(obs.target);
    unlock(cur);
    return obs.target;
}

struct getattr_ret *malloc_getattr_ret(struct inode *inum, unsigned mode,
                                       unsigned size, unsigned maj, unsigned min)
{
    obs.malloc_calls++;
    obs.malloc_inum = inum;
    obs.malloc_mode = mode;
    obs.malloc_size = size;
    obs.malloc_maj = maj;
    obs.malloc_min = min;

    if (inum != obs.target || *balance_for(inum) <= 0) {
        obs.contract_error = 1;
    }
    if (obs.gc->malloc_fails) {
        return NULL;
    }

    struct getattr_ret *ret = malloc(sizeof(*ret));
    if (ret == NULL) return NULL;
    ret->inum = inum;
    ret->mode = mode;
    ret->size = size;
    ret->maj = maj;
    ret->min = min;
    return ret;
}

static void make_path(const struct getattr_case *gc, char *path[MAX_GETATTR_DEPTH + 1U])
{
    for (unsigned i = 0; i < gc->path_len; i++) {
        path[i] = (char *)gc->names[i];
    }
    path[gc->path_len] = NULL;
}

static void setup_case(const struct getattr_case *gc, struct inode *root, struct inode *target,
                       char *path[MAX_GETATTR_DEPTH + 1U])
{
    memset(root, 0, sizeof(*root));
    memset(target, 0, sizeof(*target));
    make_path(gc, path);

    target->mode = gc->mode;
    target->size = gc->size;
    target->maj = gc->maj;
    target->min = gc->min;

    memset(&obs, 0, sizeof(obs));
    obs.gc = gc;
    obs.root = root;
    obs.target = gc->path_len == 0U ? root : target;
    obs.path = path;

    if (gc->path_len == 0U) {
        *root = *target;
    }

    root_inum = root;
}

static int no_locks_held(void)
{
    return obs.root_balance == 0 && obs.target_balance == 0 && !obs.contract_error;
}

static enum theft_trial_res prop_success_returns_fresh_attribute_snapshot(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;
    if (!gc->locate_finds || gc->malloc_fails) return THEFT_TRIAL_SKIP;

    struct inode root;
    struct inode target;
    char *path[MAX_GETATTR_DEPTH + 1U];
    setup_case(gc, &root, &target, path);

    struct getattr_ret *ret = atomfs_getattr(path);
    int ok = ret != NULL && ret->inum == obs.target && ret->mode == gc->mode &&
             ret->size == gc->size && ret->maj == gc->maj && ret->min == gc->min &&
             obs.locate_calls == 1U && obs.malloc_calls == 1U && no_locks_held();
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_failure_returns_null_without_allocation(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;
    if (gc->locate_finds) return THEFT_TRIAL_SKIP;

    struct inode root;
    struct inode target;
    char *path[MAX_GETATTR_DEPTH + 1U];
    setup_case(gc, &root, &target, path);

    struct getattr_ret *ret = atomfs_getattr(path);
    int ok = ret == NULL && obs.locate_calls == 1U && obs.malloc_calls == 0U && no_locks_held();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_forwards_exact_located_inode_fields_to_allocator(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;
    if (!gc->locate_finds) return THEFT_TRIAL_SKIP;

    struct inode root;
    struct inode target;
    char *path[MAX_GETATTR_DEPTH + 1U];
    setup_case(gc, &root, &target, path);

    struct getattr_ret *ret = atomfs_getattr(path);
    int ok = obs.malloc_calls == 1U && obs.malloc_inum == obs.target &&
             obs.malloc_mode == obs.target->mode && obs.malloc_size == obs.target->size &&
             obs.malloc_maj == obs.target->maj && obs.malloc_min == obs.target->min &&
             no_locks_held();
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocator_failure_still_releases_located_inode(struct theft *t, void *arg1)
{
    (void)t;
    const struct getattr_case *gc = arg1;
    if (!gc->locate_finds || !gc->malloc_fails) return THEFT_TRIAL_SKIP;

    struct inode root;
    struct inode target;
    char *path[MAX_GETATTR_DEPTH + 1U];
    setup_case(gc, &root, &target, path);

    struct getattr_ret *ret = atomfs_getattr(path);
    int ok = ret == NULL && obs.locate_calls == 1U && obs.malloc_calls == 1U && no_locks_held();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &getattr_case_info },                              \
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
    printf("atomfs_getattr property-based tests:\n");
    RUN_PROP("success_returns_fresh_attribute_snapshot", prop_success_returns_fresh_attribute_snapshot, 400);
    RUN_PROP("failure_returns_null_without_allocation", prop_failure_returns_null_without_allocation, 400);
    RUN_PROP("forwards_exact_located_inode_fields_to_allocator", prop_forwards_exact_located_inode_fields_to_allocator, 400);
    RUN_PROP("allocator_failure_still_releases_located_inode", prop_allocator_failure_still_releases_located_inode, 400);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
