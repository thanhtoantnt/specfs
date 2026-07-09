/*
 * Safety-focused property-based tests for inode_truncate() in
 * eval/delay_alloc/optimization/inode_management.c
 *
 * Uses theft to check invalid-size and NULL-inode contracts against the real
 * production function while stubbing the low-level file operations.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "inode_management.h"

struct lowlevel_observation {
    unsigned allocate_calls;
    struct indextb *allocate_tb;
    unsigned allocate_offset;
    unsigned allocate_len;

    unsigned clear_calls;
    struct inode *clear_node;
    unsigned clear_offset;
    unsigned clear_len;

    unsigned write_calls;
};

static struct lowlevel_observation observed;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
}

void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    observed.allocate_calls++;
    observed.allocate_tb = tb;
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

void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
    observed.write_calls++;
}

void file_read(struct indextb *tb, unsigned offset, unsigned len, char *data)
{
    (void)tb;
    (void)offset;
    (void)len;
    (void)data;
}

struct read_ret *malloc_readret(void)
{
    return NULL;
}

char *malloc_buffer(unsigned len)
{
    (void)len;
    return NULL;
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

struct truncate_safety_case {
    unsigned old_size;
    unsigned requested_size;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned boundary_above_max(struct theft *t)
{
    switch (theft_random_choice(t, 5)) {
    case 0:
        return MAX_FILE_SIZE + 1U;
    case 1:
        return MAX_FILE_SIZE + 1U + bounded_choice(t, 4096U);
    case 2:
        return MAX_FILE_SIZE + PAGE_SIZE + bounded_choice(t, PAGE_SIZE);
    case 3:
        return UINT32_MAX - bounded_choice(t, 4096U);
    default:
        return MAX_FILE_SIZE + 1U + bounded_choice(t, 1024U * 1024U);
    }
}

static enum theft_alloc_res truncate_safety_case_alloc_cb(struct theft *t, void *env,
                                                          void **instance)
{
    (void)env;
    struct truncate_safety_case *tc = malloc(sizeof(*tc));
    if (tc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 6)) {
    case 0:
        tc->old_size = 0;
        break;
    case 1:
        tc->old_size = bounded_choice(t, PAGE_SIZE);
        break;
    case 2:
        tc->old_size = MAX_FILE_SIZE - bounded_choice(t, PAGE_SIZE);
        break;
    case 3:
        tc->old_size = MAX_FILE_SIZE;
        break;
    default:
        tc->old_size = bounded_choice(t, MAX_FILE_SIZE);
        break;
    }
    tc->requested_size = boundary_above_max(t);

    *instance = tc;
    return THEFT_ALLOC_OK;
}

static void truncate_safety_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash truncate_safety_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct truncate_safety_case *tc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&tc->old_size, sizeof(tc->old_size));
    theft_hash_sink(&h, (const uint8_t *)&tc->requested_size, sizeof(tc->requested_size));
    return theft_hash_done(&h);
}

static void truncate_safety_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct truncate_safety_case *tc = instance;
    fprintf(f, "{old_size=%u, requested_size=%u}", tc->old_size, tc->requested_size);
}

static struct theft_type_info truncate_safety_case_info = {
    .alloc = truncate_safety_case_alloc_cb,
    .free = truncate_safety_case_free_cb,
    .hash = truncate_safety_case_hash_cb,
    .print = truncate_safety_case_print_cb,
};

static struct inode make_inode(unsigned size, struct indextb *file)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    node.file = file;
    return node;
}

static enum theft_trial_res prop_out_of_range_size_is_rejected(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_safety_case *tc = arg1;
    if (tc->requested_size <= MAX_FILE_SIZE || tc->old_size > MAX_FILE_SIZE) {
        return THEFT_TRIAL_SKIP;
    }

    struct indextb file;
    memset(&file, 0, sizeof(file));
    struct inode node = make_inode(tc->old_size, &file);

    reset_observed();
    inode_truncate(&node, tc->requested_size);

    if (node.size != tc->old_size || observed.allocate_calls != 0 ||
        observed.clear_calls != 0 || observed.write_calls != 0) {
        fprintf(stderr,
                "out-of-range truncate mutated state: old=%u requested=%u size=%u "
                "alloc=%u clear=%u write=%u alloc=(%u,%u) clear=(%u,%u)\n",
                tc->old_size, tc->requested_size, node.size,
                observed.allocate_calls, observed.clear_calls, observed.write_calls,
                observed.allocate_offset, observed.allocate_len,
                observed.clear_offset, observed.clear_len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_null_inode_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    struct truncate_safety_case *tc = arg1;

    pid_t pid = fork();
    if (pid < 0) return THEFT_TRIAL_SKIP;
    if (pid == 0) {
        reset_observed();
        inode_truncate(NULL, tc->requested_size);
        _exit((observed.allocate_calls == 0 && observed.clear_calls == 0 &&
               observed.write_calls == 0) ? 0 : 2);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return THEFT_TRIAL_SKIP;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "null inode crashed with signal %d for requested_size=%u\n",
                    WTERMSIG(status), tc->requested_size);
        } else {
            fprintf(stderr,
                    "null inode touched storage or exited abnormally: status=%d requested_size=%u\n",
                    status, tc->requested_size);
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
            .type_info = { &truncate_safety_case_info },                \
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

    printf("delay_alloc inode_truncate safety property-based tests:\n");
    RUN_PROP("out_of_range_size_is_rejected", prop_out_of_range_size_is_rejected, 100);
    RUN_PROP("null_inode_is_noop", prop_null_inode_is_noop, 20);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
