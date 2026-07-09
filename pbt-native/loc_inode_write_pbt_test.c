/*
 * Property-based tests for inode_write() in
 * eval/loc/gen/inode/inode_write.c
 *
 * Oracle: Algebraic invariant / negative contract over the public write
 * contract. Stronger state-machine testing is unnecessary because inode_write
 * is a single-call mutation whose observable state is node->size plus the
 * low-level file calls. The low-level file operations are stubbed so these
 * properties exercise the real generated inode_write symbol while observing
 * the requested offset/length contract precisely.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode.h"

#define MAX_BUF_LEN 1024U

struct file_write_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
    const char *data;
};

struct file_allocate_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
};

struct file_clear_observation {
    unsigned calls;
    struct inode *node;
    unsigned start;
    unsigned len;
};

static struct file_write_observation observed_write;
static struct file_allocate_observation observed_allocate;
static struct file_clear_observation observed_clear;

static void reset_observed(void)
{
    memset(&observed_write, 0, sizeof(observed_write));
    memset(&observed_allocate, 0, sizeof(observed_allocate));
    memset(&observed_clear, 0, sizeof(observed_clear));
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    observed_allocate.calls++;
    observed_allocate.node = node;
    observed_allocate.offset = offset;
    observed_allocate.len = len;
}

void file_clear(struct inode *node, unsigned start, unsigned len)
{
    observed_clear.calls++;
    observed_clear.node = node;
    observed_clear.start = start;
    observed_clear.len = len;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
{
    observed_write.calls++;
    observed_write.node = node;
    observed_write.offset = offset;
    observed_write.len = len;
    observed_write.data = data;
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
        wc->offset = MAX_FILE_SIZE - choice_inclusive(t, MAX_BUF_LEN < MAX_FILE_SIZE ? MAX_BUF_LEN : MAX_FILE_SIZE);
        wc->len = choice_inclusive(t, MAX_BUF_LEN);
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
        wc->offset = choice_inclusive(t, MAX_FILE_SIZE + MAX_BUF_LEN);
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
    if (len == 0U || offset >= MAX_FILE_SIZE) return 0U;
    unsigned remaining = MAX_FILE_SIZE - offset;
    return len > remaining ? remaining : len;
}

static unsigned expected_size_after_write(unsigned initial_size, unsigned len, unsigned offset)
{
    unsigned actual_len = expected_actual_len(len, offset);
    if (actual_len == 0U) return initial_size;

    uint64_t end = (uint64_t)offset + (uint64_t)actual_len;
    unsigned capped_end = end > MAX_FILE_SIZE ? MAX_FILE_SIZE : (unsigned)end;
    return capped_end > initial_size ? capped_end : initial_size;
}

static struct inode make_inode(unsigned size)
{
    struct inode node;
    static struct indextb tb;
    memset(&node, 0, sizeof(node));
    memset(&tb, 0, sizeof(tb));
    node.size = size;
    node.file = &tb;
    return node;
}

/* Oracle: return value is exactly the number of bytes that fit inside
 * [offset, MAX_FILE_SIZE), with zero-length and out-of-range writes as no-ops. */
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

/* Oracle: writes that cannot write any byte do not change inode state and do
 * not request allocation, clearing, or a positive-length low-level write. */
static enum theft_trial_res prop_zero_effective_writes_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    if (expected_actual_len(wc->len, wc->offset) != 0U) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    unsigned actual = inode_write(&node, wc->buffer, wc->len, wc->offset);
    int ok = actual == 0U && node.size == wc->initial_size &&
             observed_allocate.calls == 0U && observed_clear.calls == 0U &&
             (observed_write.calls == 0U ||
              (observed_write.calls == 1U && observed_write.len == 0U));

    if (!ok) {
        fprintf(stderr,
                "zero_effective_noop failed: actual=%u size=%u initial=%u writes=%u write_len=%u allocs=%u clears=%u offset=%u len=%u\n",
                actual, node.size, wc->initial_size, observed_write.calls,
                observed_write.len, observed_allocate.calls, observed_clear.calls,
                wc->offset, wc->len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: inode size after a valid write is max(initial_size, offset + written),
 * never above MAX_FILE_SIZE and unchanged when no byte can be written. */
static enum theft_trial_res prop_size_matches_effective_write(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    (void)inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected = expected_size_after_write(wc->initial_size, wc->len, wc->offset);

    if (node.size != expected || node.size > MAX_FILE_SIZE) {
        fprintf(stderr, "size mismatch: got=%u expected=%u max=%u offset=%u len=%u initial=%u\n",
                node.size, expected, MAX_FILE_SIZE, wc->offset, wc->len,
                wc->initial_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: growth allocation and clearing exactly cover the new byte range from
 * the prior EOF to the model EOF, and are not requested when the file does not grow. */
static enum theft_trial_res prop_growth_allocation_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode(wc->initial_size);
    reset_observed();

    (void)inode_write(&node, wc->buffer, wc->len, wc->offset);
    unsigned expected_size = expected_size_after_write(wc->initial_size, wc->len, wc->offset);
    unsigned expected_growth = expected_size > wc->initial_size ? expected_size - wc->initial_size : 0U;

    int ok;
    if (expected_growth == 0U) {
        ok = observed_allocate.calls == 0U && observed_clear.calls == 0U;
    } else {
        ok = observed_allocate.calls == 1U && observed_clear.calls == 1U &&
             observed_allocate.node == &node && observed_clear.node == &node &&
             observed_allocate.offset == wc->initial_size &&
             observed_clear.start == wc->initial_size &&
             observed_allocate.len == expected_growth &&
             observed_clear.len == expected_growth;
    }

    if (!ok) {
        fprintf(stderr,
                "growth mismatch: expected_growth=%u allocs=%u alloc_off=%u alloc_len=%u clears=%u clear_start=%u clear_len=%u offset=%u len=%u initial=%u\n",
                expected_growth, observed_allocate.calls, observed_allocate.offset,
                observed_allocate.len, observed_clear.calls, observed_clear.start,
                observed_clear.len, wc->offset, wc->len, wc->initial_size);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* Oracle: the low-level write sees the same effective offset, byte count, and
 * source pointer that inode_write reports to its caller. */
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
        ok = actual == 0U &&
             (observed_write.calls == 0U ||
              (observed_write.calls == 1U && observed_write.len == 0U));
    } else {
        ok = observed_write.calls == 1U && observed_write.node == &node &&
             observed_write.offset == wc->offset && observed_write.len == expected &&
             observed_write.data == wc->buffer && actual == expected;
    }

    if (!ok) {
        fprintf(stderr,
                "file_write mismatch: calls=%u offset=%u len=%u actual=%u expected=%u case_offset=%u case_len=%u\n",
                observed_write.calls, observed_write.offset, observed_write.len,
                actual, expected, wc->offset, wc->len);
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &write_case_info },
        .trials = trials,
        .seed = theft_seed_of_time(),
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    printf("loc inode_write property-based tests:\n");
    failures += run_prop("return_is_clamped", prop_return_is_clamped, 500);
    failures += run_prop("zero_effective_writes_are_noops", prop_zero_effective_writes_are_noops, 500);
    failures += run_prop("size_matches_effective_write", prop_size_matches_effective_write, 500);
    failures += run_prop("growth_allocation_matches_model", prop_growth_allocation_matches_model, 500);
    failures += run_prop("file_write_matches_effective_write", prop_file_write_matches_effective_write, 500);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
