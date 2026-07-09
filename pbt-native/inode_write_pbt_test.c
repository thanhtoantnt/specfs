/*
 * Property-based tests for inode_write() in
 * eval/extent/optimization/inode_management.c
 *
 * These tests isolate inode_write with a stubbed file_write so the observable
 * contract is the returned byte count, node size, and low-level write call.
 */
#include <theft.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

struct file_write_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
    const char *data;
};

static struct file_write_observation observed;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    observed.calls++;
    observed.node = node;
    observed.offset = offset;
    observed.len = len;
    observed.data = data;
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    (void)node;
    (void)offset;
    (void)len;
    (void)data;
}

void clear_file(struct inode *node, unsigned start, unsigned len)
{
    (void)node;
    (void)start;
    (void)len;
}

struct read_ret *malloc_readret(void)
{
    return malloc(sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    return malloc(len == 0 ? 1 : len);
}

unsigned int hash_func(char *str)
{
    (void)str;
    return 0;
}

#define MAX_BUF_LEN 512

struct write_case {
    unsigned initial_size;
    unsigned offset;
    unsigned len;
    char buffer[MAX_BUF_LEN];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    wc->initial_size = bounded_choice(t, MAX_FILE_SIZE);

    switch (theft_random_choice(t, 8)) {
    case 0:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 1:
        wc->offset = MAX_FILE_SIZE;
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 2:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE);
        wc->len = MAX_FILE_SIZE - wc->offset + bounded_choice(t, MAX_BUF_LEN);
        break;
    case 3:
        wc->offset = MAX_FILE_SIZE + 1U + bounded_choice(t, MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    case 4:
        wc->offset = UINT32_MAX - bounded_choice(t, MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    default:
        wc->offset = bounded_choice(t, MAX_FILE_SIZE + MAX_BUF_LEN);
        wc->len = bounded_choice(t, MAX_BUF_LEN);
        break;
    }

    for (unsigned i = 0; i < MAX_BUF_LEN; i++) {
        wc->buffer[i] = (char)theft_random_choice(t, 256);
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
    if (offset >= MAX_FILE_SIZE) return 0;
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
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected = expected_actual_len(wc->len, wc->offset);

    if (actual != expected) {
        fprintf(stderr, "return_is_clamped failed: got %u expected %u\n", actual, expected);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_size_updates_to_written_end(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected_size = wc->initial_size;
    if (actual > 0 && wc->offset + actual > expected_size) {
        expected_size = wc->offset + actual;
    }

    if (node.size != expected_size) {
        fprintf(stderr, "size_update failed: got %u expected %u actual=%u\n",
                node.size, expected_size, actual);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_file_write_matches_effective_write(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    if (actual == 0) {
        if (observed.calls != 0) {
            fprintf(stderr, "zero write called file_write %u times\n", observed.calls);
            return THEFT_TRIAL_FAIL;
        }
        return THEFT_TRIAL_PASS;
    }

    if (observed.calls != 1 || observed.node != &node || observed.offset != wc->offset ||
        observed.len != actual || observed.data != wc->buffer) {
        fprintf(stderr, "file_write mismatch: calls=%u offset=%u len=%u actual=%u\n",
                observed.calls, observed.offset, observed.len, actual);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

static enum theft_trial_res prop_null_inputs_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned null_node = inode_write(NULL, wc->buffer, wc->len, wc->offset);
    unsigned null_buffer = inode_write(&node, NULL, wc->len, wc->offset);

    if (null_node != 0 || null_buffer != 0 || observed.calls != 0 || node.size != wc->initial_size) {
        fprintf(stderr, "null no-op failed: null_node=%u null_buffer=%u calls=%u size=%u initial=%u\n",
                null_node, null_buffer, observed.calls, node.size, wc->initial_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &write_case_info },                          \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inode_write property-based tests:\n");
    RUN_PROP("return_is_clamped", prop_return_is_clamped, 500);
    RUN_PROP("size_updates_to_written_end", prop_size_updates_to_written_end, 500);
    RUN_PROP("file_write_matches_effective_write", prop_file_write_matches_effective_write, 500);
    RUN_PROP("null_inputs_are_noops", prop_null_inputs_are_noops, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
