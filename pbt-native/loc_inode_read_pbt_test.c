/*
 * Property-based tests for inode_read() in
 * eval/loc/gen/inode/inode_read.c
 *
 * Oracle: Algebraic/reference invariant over the public read contract in the
 * function comment. Stronger state-machine testing is unnecessary because
 * inode_read is a single-call query: for a valid initialized inode it returns
 * exactly min(len, node->size - offset) bytes, requests one low-level read for
 * that span, reports an empty read for zero/out-of-range spans, and handles its
 * allocation-failure branches without issuing I/O. The allocation wrappers and
 * file_read are stubbed so these properties exercise the real generated
 * inode_read symbol while observing the exact length/offset contract.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inode.h"

#define MAX_READ_LEN (MAX_FILE_SIZE * 2U)

struct file_read_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
    char *data;
    int out_of_model_range;
};

struct malloc_buffer_observation {
    unsigned calls;
    unsigned len;
};

static struct file_read_observation observed_read;
static struct malloc_buffer_observation observed_malloc_buffer;
static int fail_readret_alloc;
static int fail_buffer_alloc;
static unsigned model_size;
static unsigned char model_data[MAX_FILE_SIZE];

static void reset_observed(void)
{
    memset(&observed_read, 0, sizeof(observed_read));
    memset(&observed_malloc_buffer, 0, sizeof(observed_malloc_buffer));
    fail_readret_alloc = 0;
    fail_buffer_alloc = 0;
    model_size = 0U;
    memset(model_data, 0, sizeof(model_data));
}

struct read_ret *malloc_readret(void)
{
    if (fail_readret_alloc) return NULL;

    struct read_ret *ret = malloc(sizeof(*ret));
    if (ret == NULL) return NULL;
    ret->buf = NULL;
    ret->num = 0U;
    return ret;
}

char *malloc_buffer(unsigned len)
{
    observed_malloc_buffer.calls++;
    observed_malloc_buffer.len = len;
    if (fail_buffer_alloc) return NULL;
    return malloc(len == 0U ? 1U : len);
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    observed_read.calls++;
    observed_read.node = node;
    observed_read.offset = offset;
    observed_read.len = len;
    observed_read.data = data;

    if ((uint64_t)offset + (uint64_t)len > (uint64_t)model_size ||
        (uint64_t)offset + (uint64_t)len > (uint64_t)MAX_FILE_SIZE) {
        observed_read.out_of_model_range = 1;
        return;
    }

    memcpy(data, &model_data[offset], len);
}

struct read_case {
    unsigned size;
    unsigned offset;
    unsigned len;
    unsigned char data[MAX_FILE_SIZE];
};

static unsigned choice_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned near_value(struct theft *t, unsigned center, unsigned radius,
                           unsigned max_value)
{
    unsigned delta = choice_inclusive(t, radius);
    unsigned value;
    if (theft_random_choice(t, 2U) == 0U) {
        value = center > delta ? center - delta : 0U;
    } else {
        uint64_t widened = (uint64_t)center + (uint64_t)delta;
        value = widened > max_value ? max_value : (unsigned)widened;
    }
    return value;
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env,
                                               void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8U)) {
    case 0:
        rc->size = 0U;
        break;
    case 1:
        rc->size = MAX_FILE_SIZE;
        break;
    case 2:
        rc->size = choice_inclusive(t, 8U);
        break;
    default:
        rc->size = choice_inclusive(t, MAX_FILE_SIZE);
        break;
    }

    switch (theft_random_choice(t, 10U)) {
    case 0:
        rc->offset = 0U;
        break;
    case 1:
        rc->offset = rc->size;
        break;
    case 2:
        rc->offset = rc->size > 0U ? rc->size - 1U : 0U;
        break;
    case 3:
        rc->offset = near_value(t, rc->size, 16U, MAX_FILE_SIZE + MAX_READ_LEN);
        break;
    case 4:
        rc->offset = MAX_FILE_SIZE;
        break;
    case 5:
        rc->offset = MAX_FILE_SIZE + 1U + choice_inclusive(t, MAX_READ_LEN);
        break;
    case 6:
        rc->offset = UINT32_MAX - choice_inclusive(t, MAX_READ_LEN);
        break;
    default:
        rc->offset = choice_inclusive(t, MAX_FILE_SIZE + MAX_READ_LEN);
        break;
    }

    switch (theft_random_choice(t, 8U)) {
    case 0:
        rc->len = 0U;
        break;
    case 1:
        rc->len = 1U;
        break;
    case 2:
        rc->len = rc->size > rc->offset ? rc->size - rc->offset : 0U;
        break;
    case 3:
        rc->len = MAX_READ_LEN;
        break;
    default:
        rc->len = choice_inclusive(t, MAX_READ_LEN);
        break;
    }

    for (unsigned i = 0; i < MAX_FILE_SIZE; i++) {
        rc->data[i] = (unsigned char)theft_random_choice(t, 256U);
    }

    *instance = rc;
    return THEFT_ALLOC_OK;
}

static void read_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash read_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct read_case));
}

static void read_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct read_case *rc = instance;
    fprintf(f, "{size=%u, offset=%u, len=%u}", rc->size, rc->offset, rc->len);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
};

static struct inode make_inode(const struct read_case *rc)
{
    struct inode node;
    static struct indextb tb;
    memset(&node, 0, sizeof(node));
    memset(&tb, 0, sizeof(tb));
    node.size = rc->size;
    node.file = &tb;
    model_size = rc->size;
    memcpy(model_data, rc->data, sizeof(model_data));
    return node;
}

static unsigned expected_read_len(const struct read_case *rc)
{
    if (rc->len == 0U || rc->offset >= rc->size) return 0U;
    unsigned available = rc->size - rc->offset;
    return rc->len > available ? available : rc->len;
}

static int buffer_equals_model(const char *buf, unsigned offset, unsigned len)
{
    for (unsigned i = 0; i < len; i++) {
        if ((unsigned char)buf[i] != model_data[offset + i]) {
            fprintf(stderr,
                    "buffer mismatch at byte %u: got=%u expected=%u offset=%u len=%u\n",
                    i, (unsigned char)buf[i], model_data[offset + i], offset, len);
            return 0;
        }
    }
    return 1;
}

/* Oracle: successful reads return exactly min(len, node->size - offset) bytes
 * and the returned buffer contains the bytes read from the model file. */
static enum theft_trial_res prop_read_returns_exact_slice(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode(rc);
    reset_observed();
    node = make_inode(rc);

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    unsigned expected = expected_read_len(rc);
    int ok = ret->num == expected;
    if (ok && expected == 0U) {
        ok = ret->buf == NULL;
    } else if (ok) {
        ok = ret->buf != NULL && buffer_equals_model(ret->buf, rc->offset, expected);
    }

    if (!ok) {
        fprintf(stderr,
                "exact_slice failed: num=%u expected=%u buf=%p size=%u offset=%u len=%u\n",
                ret->num, expected, (void *)ret->buf, rc->size, rc->offset, rc->len);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: inode_read issues exactly one low-level file_read for non-empty
 * successful reads, with the same inode, offset, effective length, and returned
 * buffer pointer; empty reads issue no low-level read. */
static enum theft_trial_res prop_file_read_call_matches_effective_span(struct theft *t,
                                                                       void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode(rc);
    reset_observed();
    node = make_inode(rc);

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    unsigned expected = expected_read_len(rc);
    int ok;
    if (expected == 0U) {
        ok = observed_read.calls == 0U && observed_malloc_buffer.calls == 0U &&
             ret->num == 0U && ret->buf == NULL;
    } else {
        ok = observed_malloc_buffer.calls == 1U && observed_malloc_buffer.len == expected &&
             observed_read.calls == 1U && observed_read.node == &node &&
             observed_read.offset == rc->offset && observed_read.len == expected &&
             observed_read.data == ret->buf && !observed_read.out_of_model_range &&
             ret->num == expected;
    }

    if (!ok) {
        fprintf(stderr,
                "file_read call mismatch: expected=%u read_calls=%u read_off=%u read_len=%u malloc_calls=%u malloc_len=%u size=%u offset=%u len=%u\n",
                expected, observed_read.calls, observed_read.offset, observed_read.len,
                observed_malloc_buffer.calls, observed_malloc_buffer.len,
                rc->size, rc->offset, rc->len);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: empty ranges (offset >= size or len == 0) return {buf=NULL,num=0}
 * without allocating a data buffer or issuing low-level I/O. */
static enum theft_trial_res prop_empty_ranges_are_noops(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    if (expected_read_len(rc) != 0U) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(rc);
    reset_observed();
    node = make_inode(rc);

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == 0U && ret->buf == NULL && observed_read.calls == 0U &&
             observed_malloc_buffer.calls == 0U && node.size == rc->size &&
             node.file != NULL;
    if (!ok) {
        fprintf(stderr,
                "empty noop failed: num=%u buf=%p read_calls=%u malloc_calls=%u size=%u offset=%u len=%u\n",
                ret->num, (void *)ret->buf, observed_read.calls,
                observed_malloc_buffer.calls, rc->size, rc->offset, rc->len);
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: if the data-buffer allocation fails for a non-empty read, inode_read
 * returns the allocated read_ret with {buf=NULL,num=0} and does not call
 * file_read. */
static enum theft_trial_res prop_buffer_alloc_failure_returns_empty(struct theft *t,
                                                                    void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    unsigned expected = expected_read_len(rc);
    if (expected == 0U) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode(rc);
    reset_observed();
    fail_buffer_alloc = 1;
    node = make_inode(rc);

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_FAIL;

    int ok = ret->num == 0U && ret->buf == NULL &&
             observed_malloc_buffer.calls == 1U && observed_malloc_buffer.len == expected &&
             observed_read.calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "buffer alloc failure mismatch: num=%u buf=%p malloc_calls=%u malloc_len=%u read_calls=%u expected=%u\n",
                ret->num, (void *)ret->buf, observed_malloc_buffer.calls,
                observed_malloc_buffer.len, observed_read.calls, expected);
    }

    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: if read_ret allocation fails, inode_read returns NULL and performs
 * no data-buffer allocation or low-level read. */
static enum theft_trial_res prop_readret_alloc_failure_returns_null(struct theft *t,
                                                                    void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode(rc);
    reset_observed();
    fail_readret_alloc = 1;
    node = make_inode(rc);

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    int ok = ret == NULL && observed_malloc_buffer.calls == 0U && observed_read.calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "readret alloc failure mismatch: ret=%p malloc_calls=%u read_calls=%u\n",
                (void *)ret, observed_malloc_buffer.calls, observed_read.calls);
        free(ret);
    }

    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &read_case_info },
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

    printf("loc inode_read property-based tests:\n");
    failures += run_prop("read_returns_exact_slice", prop_read_returns_exact_slice, 500);
    failures += run_prop("file_read_call_matches_effective_span", prop_file_read_call_matches_effective_span, 500);
    failures += run_prop("empty_ranges_are_noops", prop_empty_ranges_are_noops, 300);
    failures += run_prop("buffer_alloc_failure_returns_empty", prop_buffer_alloc_failure_returns_empty, 300);
    failures += run_prop("readret_alloc_failure_returns_null", prop_readret_alloc_failure_returns_null, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
