/*
 * Property-based tests for inode_read() in
 * eval/pre_alloc/baseline/inode_management.c
 *
 * Oracle: Algebraic/reference model plus negative contracts over the public
 * read contract from sysspec/specfs/inode/inode_read.spec. inode_read is a
 * single-call query, so stronger state-machine testing is unnecessary: the
 * observable behavior is the returned read_ret, absence of inode mutation, and
 * the exact low-level file_read range. This test links the real pre_alloc
 * baseline inode_read implementation and stubs only allocation, hashing, and
 * low-level I/O collaborators. A byte-array model backs file_read so clamping,
 * delegation, copied contents, empty reads, NULL-node safety, and unsigned
 * offset+len overflow boundaries are checked directly.
 */
#include <theft.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "inode_management.h"

#define MODEL_CAP 8192U

struct model_file {
    unsigned char bytes[MODEL_CAP];
};

struct file_read_observation {
    unsigned calls;
    struct inode *node;
    unsigned offset;
    unsigned len;
    char *data;
};

struct read_case {
    unsigned size;
    unsigned offset;
    unsigned len;
    unsigned char bytes[MODEL_CAP];
};

static struct file_read_observation observed_read;
static unsigned malloc_buffer_calls;
static unsigned malloc_buffer_last_len;

static void reset_observed(void)
{
    memset(&observed_read, 0, sizeof(observed_read));
    malloc_buffer_calls = 0U;
    malloc_buffer_last_len = 0U;
}

struct read_ret *malloc_readret(void)
{
    return calloc(1, sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    malloc_buffer_calls++;
    malloc_buffer_last_len = len;

    size_t alloc_len = len == 0U ? 1U : (len <= MODEL_CAP ? len : MODEL_CAP);
    return malloc(alloc_len);
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    observed_read.calls++;
    observed_read.node = node;
    observed_read.offset = offset;
    observed_read.len = len;
    observed_read.data = data;

    if (data == NULL || node == NULL || offset >= MODEL_CAP) {
        return;
    }

    struct model_file *model = (struct model_file *)node->extents;
    unsigned copy_len = MODEL_CAP - offset;
    if (copy_len > len) copy_len = len;
    memcpy(data, model->bytes + offset, copy_len);
}

void file_allocate(struct inode *node, unsigned offset, unsigned len)
{
    (void)node;
    (void)offset;
    (void)len;
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data)
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

unsigned int hash_func(char *name)
{
    (void)name;
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

static unsigned choice_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned expected_num(unsigned size, unsigned offset, unsigned len)
{
    if (offset >= size) return 0U;
    unsigned remaining = size - offset;
    return len < remaining ? len : remaining;
}

static struct inode make_inode(const struct read_case *rc, struct model_file *model)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = rc->size;
    node.extents = (Extent *)model;
    memcpy(model->bytes, rc->bytes, sizeof(model->bytes));
    return node;
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 10U)) {
    case 0:
        rc->size = 0U;
        rc->offset = choice_inclusive(t, MODEL_CAP);
        rc->len = choice_inclusive(t, MODEL_CAP);
        break;
    case 1:
        rc->size = 1U + choice_inclusive(t, MODEL_CAP - 1U);
        rc->offset = choice_inclusive(t, rc->size - 1U);
        rc->len = 0U;
        break;
    case 2:
        rc->size = 1U + choice_inclusive(t, MODEL_CAP - 1U);
        rc->offset = rc->size;
        rc->len = choice_inclusive(t, MODEL_CAP);
        break;
    case 3:
        rc->size = 1U + choice_inclusive(t, MODEL_CAP - 1U);
        rc->offset = choice_inclusive(t, rc->size - 1U);
        rc->len = MODEL_CAP;
        break;
    case 4:
        rc->size = MODEL_CAP;
        rc->offset = MODEL_CAP - 1U;
        rc->len = choice_inclusive(t, MODEL_CAP);
        break;
    case 5:
        rc->size = 2U + choice_inclusive(t, MODEL_CAP - 2U);
        rc->offset = 1U + choice_inclusive(t, rc->size - 2U);
        rc->len = UINT_MAX;
        break;
    case 6:
        rc->size = 2U + choice_inclusive(t, MODEL_CAP - 2U);
        rc->offset = 1U + choice_inclusive(t, rc->size - 2U);
        rc->len = UINT_MAX - choice_inclusive(t, 16U);
        break;
    default:
        rc->size = choice_inclusive(t, MODEL_CAP);
        rc->offset = choice_inclusive(t, MODEL_CAP);
        rc->len = choice_inclusive(t, MODEL_CAP);
        break;
    }

    for (unsigned i = 0; i < MODEL_CAP; i++) {
        rc->bytes[i] = (unsigned char)theft_random_choice(t, 256U);
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

static enum theft_trial_res prop_offset_beyond_size_returns_empty(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    if (rc->offset < rc->size) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == 0U && ret->buf == NULL &&
             observed_read.calls == 0U && malloc_buffer_calls == 0U &&
             node.size == rc->size;
    if (!ok) {
        fprintf(stderr,
                "offset_beyond_size_returns_empty failed: size=%u offset=%u len=%u num=%u buf=%p file_calls=%u malloc_calls=%u final_size=%u\n",
                rc->size, rc->offset, rc->len, ret->num, (void *)ret->buf,
                observed_read.calls, malloc_buffer_calls, node.size);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_len_returns_empty_without_allocating(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    if (!(rc->len == 0U && rc->offset < rc->size)) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == 0U && ret->buf == NULL &&
             observed_read.calls == 0U && malloc_buffer_calls == 0U &&
             node.size == rc->size;
    if (!ok) {
        fprintf(stderr,
                "zero_len_returns_empty failed: size=%u offset=%u len=%u num=%u buf=%p file_calls=%u malloc_calls=%u final_size=%u\n",
                rc->size, rc->offset, rc->len, ret->num, (void *)ret->buf,
                observed_read.calls, malloc_buffer_calls, node.size);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_num_is_clamped_to_file_size(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    unsigned expected = expected_num(rc->size, rc->offset, rc->len);
    int ok = ret->num == expected && node.size == rc->size;
    if (!ok) {
        fprintf(stderr,
                "num_is_clamped failed: size=%u offset=%u len=%u num=%u expected=%u final_size=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected, node.size);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_nonempty_reads_delegate_exact_range(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    unsigned expected = expected_num(rc->size, rc->offset, rc->len);
    if (expected == 0U) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == expected && ret->buf != NULL &&
             malloc_buffer_calls == 1U && malloc_buffer_last_len == expected &&
             observed_read.calls == 1U && observed_read.node == &node &&
             observed_read.offset == rc->offset && observed_read.len == expected &&
             observed_read.data == ret->buf && node.size == rc->size;
    if (!ok) {
        fprintf(stderr,
                "delegate_exact_range failed: size=%u offset=%u len=%u expected=%u num=%u buf=%p malloc_calls=%u last_len=%u file_calls=%u file_off=%u file_len=%u final_size=%u\n",
                rc->size, rc->offset, rc->len, expected, ret->num, (void *)ret->buf,
                malloc_buffer_calls, malloc_buffer_last_len, observed_read.calls,
                observed_read.offset, observed_read.len, node.size);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_nonempty_reads_match_model_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    unsigned expected = expected_num(rc->size, rc->offset, rc->len);
    if (expected == 0U) return THEFT_TRIAL_SKIP;
    if (rc->len > MODEL_CAP) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == expected && ret->buf != NULL &&
             memcmp(ret->buf, model.bytes + rc->offset, expected) == 0 &&
             node.size == rc->size;
    if (!ok) {
        fprintf(stderr,
                "content_matches_model failed: size=%u offset=%u len=%u num=%u expected=%u final_size=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected, node.size);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_node_returns_null_without_side_effects(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    reset_observed();

    struct read_ret *ret = inode_read(NULL, rc->len, rc->offset);
    int ok = ret == NULL && observed_read.calls == 0U && malloc_buffer_calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "null_node_returns_null failed: offset=%u len=%u ret=%p file_calls=%u malloc_calls=%u\n",
                rc->offset, rc->len, (void *)ret, observed_read.calls,
                malloc_buffer_calls);
        free(ret);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                 \
        struct theft_run_config cfg = {                                  \
            .name = name_,                                               \
            .prop1 = prop_,                                              \
            .type_info = { &read_case_info },                            \
            .trials = trials_,                                           \
            .seed = theft_seed_of_time(),                                \
        };                                                               \
        enum theft_run_res res = theft_run(&cfg);                        \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);       \
        if (res != THEFT_RUN_PASS) failures++;                           \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("pre_alloc baseline inode_read property-based tests:\n");
    RUN_PROP("offset_beyond_size_returns_empty", prop_offset_beyond_size_returns_empty, 500);
    RUN_PROP("zero_len_returns_empty_without_allocating", prop_zero_len_returns_empty_without_allocating, 200);
    RUN_PROP("num_is_clamped_to_file_size", prop_num_is_clamped_to_file_size, 500);
    RUN_PROP("nonempty_reads_delegate_exact_range", prop_nonempty_reads_delegate_exact_range, 500);
    RUN_PROP("nonempty_reads_match_model_bytes", prop_nonempty_reads_match_model_bytes, 500);
    RUN_PROP("null_node_returns_null_without_side_effects", prop_null_node_returns_null_without_side_effects, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
