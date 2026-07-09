/*
 * Property-based tests for inode_write() in
 * eval/rbtree/baseline/inode_management.c
 *
 * Oracle: Algebraic invariant / negative contract over the public write
 * contract. inode_write is a single-call state mutation; the observable
 * contract is its returned byte count, node->size update, and low-level
 * file_write call. These tests link the real rbtree baseline inode_write
 * implementation and stub only low-level collaborators so the effective write
 * range can be observed precisely.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

#define MAX_BUF_LEN 512U

struct file_write_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
    const char *data;
};

struct clear_file_observation {
    unsigned calls;
    struct inode *node;
    unsigned start;
    unsigned len;
};

static struct file_write_observation observed_write;
static struct clear_file_observation observed_clear;

static void reset_observed(void)
{
    memset(&observed_write, 0, sizeof(observed_write));
    memset(&observed_clear, 0, sizeof(observed_clear));
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    (void)node;
    (void)offset;
    (void)len;
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
    observed_write.calls++;
    observed_write.node = node;
    observed_write.offset = offset;
    observed_write.len = len;
    observed_write.data = data;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    observed_clear.calls++;
    observed_clear.node = node;
    observed_clear.start = start;
    observed_clear.len = len;
}

struct read_ret *malloc_readret(void)
{
    return malloc(sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    return malloc(len == 0U ? 1U : len);
}

unsigned int hash_func(char *str)
{
    (void)str;
    return 0U;
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
}

struct write_case {
    unsigned initial_size;
    unsigned offset;
    unsigned len;
    char buffer[MAX_BUF_LEN];
};

static unsigned choice_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    wc->initial_size = choice_inclusive(t, MAX_FILE_SIZE);

    switch (theft_random_choice(t, 9U)) {
    case 0:
        wc->offset = 0U;
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    case 1:
        wc->offset = choice_inclusive(t, MAX_FILE_SIZE);
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    case 2:
        wc->offset = MAX_FILE_SIZE;
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    case 3:
        wc->offset = MAX_FILE_SIZE - choice_inclusive(t, MAX_BUF_LEN);
        wc->len = choice_inclusive(t, MAX_BUF_LEN * 2U);
        break;
    case 4:
        wc->offset = MAX_FILE_SIZE + 1U + choice_inclusive(t, MAX_BUF_LEN);
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    case 5:
        wc->offset = UINT32_MAX - choice_inclusive(t, MAX_BUF_LEN);
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    case 6:
        wc->offset = choice_inclusive(t, MAX_FILE_SIZE);
        wc->len = 0U;
        break;
    default:
        wc->offset = choice_inclusive(t, MAX_FILE_SIZE + MAX_BUF_LEN);
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
        break;
    }

    for (unsigned i = 0; i < MAX_BUF_LEN; i++) {
        wc->buffer[i] = (char)theft_random_choice(t, 256U);
    }

    *instance = wc;
    return THEFT_ALLOC_OK;
}

static void write_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash write_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct write_case));
}

static void write_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct write_case *wc = instance;
    fprintf(f, "{initial_size=%u, offset=%u, len=%u}",
            wc->initial_size, wc->offset, wc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static unsigned expected_actual_len(unsigned len, unsigned offset)
{
    if (offset >= MAX_FILE_SIZE) return 0U;
    unsigned remaining = MAX_FILE_SIZE - offset;
    return len > remaining ? remaining : len;
}

static struct inode make_inode(unsigned size)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    return node;
}

static enum theft_trial_res prop_return_is_clamped(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected = expected_actual_len(wc->len, wc->offset);

    if (actual != expected) {
        fprintf(stderr, "return_is_clamped failed: got=%u expected=%u offset=%u len=%u\n",
                actual, expected, wc->offset, wc->len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_out_of_range_offsets_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    if (wc->offset < MAX_FILE_SIZE) return THEFT_TRIAL_PASS;

    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    int ok = actual == 0U && node.size == wc->initial_size &&
             observed_write.calls == 0U && observed_clear.calls == 0U;

    if (!ok) {
        fprintf(stderr,
                "out_of_range_noop failed: actual=%u size=%u initial=%u writes=%u clears=%u offset=%u len=%u\n",
                actual, node.size, wc->initial_size, observed_write.calls,
                observed_clear.calls, wc->offset, wc->len);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_size_never_exceeds_max_file_size(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    (void)inode_write(&node, wc->buffer, wc->len, wc->offset);
    int ok = node.size <= MAX_FILE_SIZE;
    if (!ok) {
        fprintf(stderr, "size exceeded MAX_FILE_SIZE: size=%u max=%u offset=%u len=%u\n",
                node.size, MAX_FILE_SIZE, wc->offset, wc->len);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_file_write_matches_effective_write(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected = expected_actual_len(wc->len, wc->offset);
    int ok;
    if (expected == 0U) {
        ok = actual == 0U && observed_write.calls == 0U;
    } else {
        ok = actual == expected && observed_write.calls == 1U &&
             observed_write.node == &node && observed_write.offset == wc->offset &&
             observed_write.len == expected && observed_write.data == wc->buffer;
    }

    if (!ok) {
        fprintf(stderr,
                "file_write mismatch: calls=%u offset=%u len=%u actual=%u expected=%u\n",
                observed_write.calls, observed_write.offset, observed_write.len,
                actual, expected);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_inputs_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned null_node = inode_write(NULL, wc->buffer, wc->len, wc->offset);
    unsigned null_buffer = inode_write(&node, NULL, wc->len, wc->offset);
    int ok = null_node == 0U && null_buffer == 0U &&
             observed_write.calls == 0U && observed_clear.calls == 0U &&
             node.size == wc->initial_size;

    if (!ok) {
        fprintf(stderr,
                "null no-op failed: null_node=%u null_buffer=%u calls=%u clears=%u size=%u initial=%u\n",
                null_node, null_buffer, observed_write.calls, observed_clear.calls,
                node.size, wc->initial_size);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                 \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &write_case_info },                          \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);      \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("rbtree baseline inode_write property-based tests:\n");
    RUN_PROP("return_is_clamped", prop_return_is_clamped, 500);
    RUN_PROP("out_of_range_offsets_are_noops", prop_out_of_range_offsets_are_noops, 500);
    RUN_PROP("size_never_exceeds_max_file_size", prop_size_never_exceeds_max_file_size, 500);
    RUN_PROP("file_write_matches_effective_write", prop_file_write_matches_effective_write, 500);
    RUN_PROP("null_inputs_are_noops", prop_null_inputs_are_noops, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
