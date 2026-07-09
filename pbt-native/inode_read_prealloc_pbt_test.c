/*
 * Property-based tests for inode_read() in
 * eval/pre_alloc/optimization/inode_management.c
 *
 * The test links the real inode_management.c and stubs only allocation,
 * hashing, and low-level file I/O dependencies. A byte-array model backs
 * file_read so inode_read's return contract and copied contents can be
 * checked directly.
 */
#include <theft.h>

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

static struct file_read_observation observed;
static unsigned malloc_buffer_calls;
static unsigned malloc_buffer_last_len;

static void reset_observed(void)
{
    memset(&observed, 0, sizeof(observed));
    malloc_buffer_calls = 0;
    malloc_buffer_last_len = 0;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound + 1U);
}

static unsigned expected_num(unsigned size, unsigned offset, unsigned len)
{
    if (offset >= size) return 0;
    unsigned remaining = size - offset;
    return len < remaining ? len : remaining;
}

static struct inode make_inode(const struct read_case *rc, struct model_file *model)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = rc->size;
    node.dir = (struct dirtb *)model;
    memcpy(model->bytes, rc->bytes, sizeof(model->bytes));
    return node;
}

struct read_ret *malloc_readret(void)
{
    return calloc(1, sizeof(struct read_ret));
}

char *malloc_buffer(unsigned len)
{
    malloc_buffer_calls++;
    malloc_buffer_last_len = len;
    return malloc(len == 0 ? 1 : len);
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data)
{
    observed.calls++;
    observed.node = node;
    observed.offset = offset;
    observed.len = len;
    observed.data = data;

    struct model_file *model = (struct model_file *)node->dir;
    for (unsigned i = 0; i < len; i++) {
        unsigned absolute = offset + i;
        data[i] = absolute < MODEL_CAP ? (char)model->bytes[absolute] : 0;
    }
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
    return 0;
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
    case 0:
        rc->size = 0;
        rc->offset = bounded_choice(t, MODEL_CAP);
        rc->len = bounded_choice(t, MODEL_CAP);
        break;
    case 1:
        rc->size = 1U + bounded_choice(t, MODEL_CAP - 1U);
        rc->offset = bounded_choice(t, rc->size - 1U);
        rc->len = 0;
        break;
    case 2:
        rc->size = 1U + bounded_choice(t, MODEL_CAP - 1U);
        rc->offset = rc->size;
        rc->len = bounded_choice(t, MODEL_CAP);
        break;
    case 3:
        rc->size = 1U + bounded_choice(t, MODEL_CAP - 1U);
        rc->offset = bounded_choice(t, rc->size - 1U);
        rc->len = MODEL_CAP;
        break;
    case 4:
        rc->size = MODEL_CAP;
        rc->offset = MODEL_CAP - 1U;
        rc->len = bounded_choice(t, MODEL_CAP);
        break;
    default:
        rc->size = bounded_choice(t, MODEL_CAP);
        rc->offset = bounded_choice(t, MODEL_CAP);
        rc->len = bounded_choice(t, MODEL_CAP);
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

static enum theft_trial_res prop_empty_reads_return_null_buffer(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    if (!(rc->len == 0 || rc->offset >= rc->size)) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == 0 && ret->buf == NULL;
    if (!ok) {
        fprintf(stderr,
                "empty read failed: size=%u offset=%u len=%u num=%u buf=%p calls=%u\n",
                rc->size, rc->offset, rc->len, ret->num, (void *)ret->buf,
                observed.calls);
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
    int ok = ret->num == expected;
    if (!ok) {
        fprintf(stderr, "clamp failed: size=%u offset=%u len=%u num=%u expected=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected);
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
    if (expected == 0) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == expected && ret->buf != NULL &&
             malloc_buffer_calls == 1 && malloc_buffer_last_len == expected &&
             observed.calls == 1 && observed.node == &node &&
             observed.offset == rc->offset && observed.len == expected &&
             observed.data == ret->buf;
    if (!ok) {
        fprintf(stderr,
                "delegate failed: expected=%u num=%u buf=%p malloc_calls=%u last_len=%u file_calls=%u file_off=%u file_len=%u\n",
                expected, ret->num, (void *)ret->buf, malloc_buffer_calls,
                malloc_buffer_last_len, observed.calls, observed.offset, observed.len);
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
    if (expected == 0) return THEFT_TRIAL_SKIP;

    struct model_file model;
    struct inode node = make_inode(rc, &model);
    reset_observed();

    struct read_ret *ret = inode_read(&node, rc->len, rc->offset);
    if (ret == NULL) return THEFT_TRIAL_ERROR;

    int ok = ret->num == expected && ret->buf != NULL &&
             memcmp(ret->buf, model.bytes + rc->offset, expected) == 0;
    if (!ok) {
        fprintf(stderr, "content failed: size=%u offset=%u len=%u num=%u expected=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_node_returns_null(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    reset_observed();

    struct read_ret *ret = inode_read(NULL, rc->len, rc->offset);
    int ok = ret == NULL && observed.calls == 0 && malloc_buffer_calls == 0;
    if (!ok) {
        fprintf(stderr, "null node failed: ret=%p file_calls=%u malloc_calls=%u\n",
                (void *)ret, observed.calls, malloc_buffer_calls);
        free(ret);
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &read_case_info },                          \
            .trials = trials_,                                         \
            .seed = theft_seed_of_time(),                              \
        };                                                             \
        enum theft_run_res res = theft_run(&cfg);                      \
        printf("  [%s] %s\n",                                        \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);      \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("pre_alloc inode_read property-based tests:\n");
    RUN_PROP("empty_reads_return_null_buffer", prop_empty_reads_return_null_buffer, 500);
    RUN_PROP("num_is_clamped_to_file_size", prop_num_is_clamped_to_file_size, 500);
    RUN_PROP("nonempty_reads_delegate_exact_range", prop_nonempty_reads_delegate_exact_range, 500);
    RUN_PROP("nonempty_reads_match_model_bytes", prop_nonempty_reads_match_model_bytes, 500);
    RUN_PROP("null_node_returns_null", prop_null_node_returns_null, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
