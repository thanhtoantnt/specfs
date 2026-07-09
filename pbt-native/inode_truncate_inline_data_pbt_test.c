/*
 * Property-based tests for inode_truncate() in
 * eval/inline_data/optimization/inode_management.c
 *
 * Uses theft (C PBT framework): https://github.com/silentbicycle/theft
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "inode_management.h"

struct lowlevel_observation {
    unsigned allocate_calls;
    struct inode *allocate_node;
    unsigned allocate_offset;
    unsigned allocate_len;

    unsigned clear_calls;
    struct inode *clear_node;
    unsigned clear_offset;
    unsigned clear_len;
};

static struct lowlevel_observation observed;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    observed.allocate_calls++;
    observed.allocate_node = node;
    observed.allocate_offset = offset;
    observed.allocate_len = len;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    observed.clear_calls++;
    observed.clear_node = node;
    observed.clear_offset = start;
    observed.clear_len = len;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

struct read_ret *malloc_readret(void)
{
    return malloc(sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    return malloc(len == 0 ? 1 : len);
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

unsigned int hash_func(char *name)
{
    (void)name;
    return 0;
}

struct truncate_case {
    unsigned old_size;
    unsigned new_size;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned boundary_near(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = bounded_choice(t, radius);
    if (theft_random_choice(t, 2) == 0) {
        return center > delta ? center - delta : 0;
    }
    if (UINT32_MAX - center < delta) return UINT32_MAX;
    return center + delta;
}

static enum theft_alloc_res truncate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct truncate_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
    case 0:
        tc->old_size = bounded_choice(t, INLINE_DATA_SIZE);
        tc->new_size = bounded_choice(t, INLINE_DATA_SIZE);
        break;
    case 1:
        tc->old_size = boundary_near(t, INLINE_DATA_SIZE, 512);
        tc->new_size = boundary_near(t, INLINE_DATA_SIZE, 512);
        break;
    case 2:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        tc->new_size = boundary_near(t, MAX_FILE_SIZE, 512);
        break;
    case 3:
        tc->old_size = boundary_near(t, MAX_FILE_SIZE, 512);
        tc->new_size = bounded_choice(t, MAX_FILE_SIZE);
        break;
    case 4:
        tc->old_size = MAX_FILE_SIZE;
        tc->new_size = MAX_FILE_SIZE + 1U + bounded_choice(t, 512);
        break;
    default:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        tc->new_size = bounded_choice(t, MAX_FILE_SIZE);
        break;
    }

    if (tc->old_size > MAX_FILE_SIZE) tc->old_size = MAX_FILE_SIZE;
    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void truncate_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash truncate_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, (const uint8_t *)&tc->new_size, sizeof(tc->new_size));
    return theft_hash_done(&h);
}

static void truncate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_case *tc = instance;
    fprintf(f, "{old_size=%u, new_size=%u}", tc->old_size, tc->new_size);
}

static struct theft_type_info truncate_case_info = {
    .alloc = truncate_case_alloc_cb,
    .free = truncate_case_free_cb,
    .hash = truncate_case_hash_cb,
    .print = truncate_case_print_cb,
};

static struct inode make_inode(unsigned size)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

static enum theft_trial_res prop_size_always_updates_within_limit(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size > MAX_FILE_SIZE) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->new_size) {
        fprintf(stderr, "size mismatch: old=%u new=%u got=%u\n",
                tc->old_size, tc->new_size, node.size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_shrink_clears_exact_tail(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size >= tc->old_size || tc->new_size > MAX_FILE_SIZE) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (observed.allocate_calls != 0 || observed.clear_calls != 1 ||
        observed.clear_node != &node || observed.clear_offset != tc->new_size ||
        observed.clear_len != tc->old_size - tc->new_size) {
        fprintf(stderr,
                "bad shrink clear: old=%u new=%u alloc=%u clear=%u offset=%u len=%u\n",
                tc->old_size, tc->new_size, observed.allocate_calls,
                observed.clear_calls, observed.clear_offset, observed.clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_equal_size_does_not_touch_storage(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    struct inode node = make_inode(tc->old_size);

    reset_observed();
    inode_truncate(&node, tc->old_size);

    if (node.size != tc->old_size || observed.allocate_calls != 0 || observed.clear_calls != 0) {
        fprintf(stderr, "equal truncate changed state: size=%u alloc=%u clear=%u\n",
                node.size, observed.allocate_calls, observed.clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_grow_allocates_and_clears_new_region(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size <= tc->old_size || tc->new_size > MAX_FILE_SIZE) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    unsigned grow_len = tc->new_size - tc->old_size;
    if (observed.allocate_calls != 1 || observed.allocate_node != &node ||
        observed.allocate_offset != tc->old_size || observed.allocate_len != grow_len ||
        observed.clear_calls != 1 || observed.clear_node != &node ||
        observed.clear_offset != tc->old_size || observed.clear_len != grow_len) {
        fprintf(stderr,
                "bad grow storage: old=%u new=%u alloc=%u (%u,%u) clear=%u (%u,%u)\n",
                tc->old_size, tc->new_size, observed.allocate_calls,
                observed.allocate_offset, observed.allocate_len, observed.clear_calls,
                observed.clear_offset, observed.clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_out_of_range_size_is_rejected(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;
    if (tc->new_size <= MAX_FILE_SIZE) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(tc->old_size);
    reset_observed();
    inode_truncate(&node, tc->new_size);

    if (node.size != tc->old_size || observed.allocate_calls != 0 || observed.clear_calls != 0) {
        fprintf(stderr,
                "out-of-range truncate mutated state: old=%u new=%u size=%u alloc=%u clear=%u\n",
                tc->old_size, tc->new_size, node.size,
                observed.allocate_calls, observed.clear_calls);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_null_inode_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_case *tc = arg1;

    pid_t pid = fork();
    if (pid < 0) return THEFT_TRIAL_SKIP;
    if (pid == 0) {
        reset_observed();
        inode_truncate(NULL, tc->new_size);
        _exit((observed.allocate_calls == 0 && observed.clear_calls == 0) ? 0 : 2);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return THEFT_TRIAL_SKIP;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "null inode crashed with signal %d\n", WTERMSIG(status));
        } else {
            fprintf(stderr, "null inode touched storage or exited abnormally: status=%d\n", status);
        }
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &truncate_case_info },                       \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);       \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inline_data inode_truncate property-based tests:\n");
    RUN_PROP("size_always_updates_within_limit", prop_size_always_updates_within_limit, 500);
    RUN_PROP("shrink_clears_exact_tail", prop_shrink_clears_exact_tail, 500);
    RUN_PROP("equal_size_does_not_touch_storage", prop_equal_size_does_not_touch_storage, 200);
    RUN_PROP("grow_allocates_and_clears_new_region", prop_grow_allocates_and_clears_new_region, 500);
    RUN_PROP("out_of_range_size_is_rejected", prop_out_of_range_size_is_rejected, 100);
    RUN_PROP("null_inode_is_noop", prop_null_inode_is_noop, 20);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
