/*
 * Property-based tests for inode_read() in
 * eval/extent/baseline/inode_management.c
 *
 * Oracle: Algebraic — reference model / negative contract over the public read
 * contract. Stronger state-machine testing is unnecessary because inode_read is
 * a single-call query whose observable behavior is the returned read_ret plus
 * the low-level file_read call. The low-level file operation is stubbed with a
 * byte-array model so these properties exercise the real baseline inode_read
 * symbol while checking clamping, delegation, contents, and NULL-node safety.
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

#define MODEL_CAP 8192U

struct model_file {
    unsigned char bytes[MODEL_CAP];
};

struct file_read_observation {
    unsigned calls;
    struct indextb *tb;
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
    return malloc(len == 0U ? 1U : len);
}

void file_read(struct indextb *tb, unsigned offset, unsigned len, char *data)
{
    observed_read.calls++;
    observed_read.tb = tb;
    observed_read.offset = offset;
    observed_read.len = len;
    observed_read.data = data;

    struct model_file *model = (struct model_file *)tb;
    for (unsigned i = 0; i < len; i++) {
        unsigned absolute = offset + i;
        data[i] = absolute < MODEL_CAP ? (char)model->bytes[absolute] : 0;
    }
}

void file_allocate(struct indextb *tb, unsigned offset, unsigned len)
{
    (void)tb;
    (void)offset;
    (void)len;
}

void file_write(struct indextb *tb, unsigned offset, unsigned len, const char *data)
{
    (void)tb;
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
    node.file = (struct indextb *)model;
    memcpy(model->bytes, rc->bytes, sizeof(model->bytes));
    return node;
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8U)) {
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
             observed_read.calls == 0U && malloc_buffer_calls == 0U;
    if (!ok) {
        fprintf(stderr,
                "offset_beyond_size_returns_empty failed: size=%u offset=%u len=%u num=%u buf=%p file_calls=%u malloc_calls=%u\n",
                rc->size, rc->offset, rc->len, ret->num, (void *)ret->buf,
                observed_read.calls, malloc_buffer_calls);
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
        fprintf(stderr, "num_is_clamped failed: size=%u offset=%u len=%u num=%u expected=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_reads_delegate_exact_effective_range(struct theft *t, void *arg1)
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
             observed_read.calls == 1U && observed_read.tb == node.file &&
             observed_read.offset == rc->offset && observed_read.len == expected &&
             observed_read.data == ret->buf;
    if (!ok) {
        fprintf(stderr,
                "delegate_exact_range failed: expected=%u num=%u buf=%p malloc_calls=%u last_len=%u file_calls=%u file_off=%u file_len=%u\n",
                expected, ret->num, (void *)ret->buf, malloc_buffer_calls,
                malloc_buffer_last_len, observed_read.calls, observed_read.offset,
                observed_read.len);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_read_bytes_match_model(struct theft *t, void *arg1)
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
             memcmp(ret->buf, model.bytes + rc->offset, expected) == 0;
    if (!ok) {
        fprintf(stderr, "read_bytes_match_model failed: size=%u offset=%u len=%u num=%u expected=%u\n",
                rc->size, rc->offset, rc->len, ret->num, expected);
    }

    free(ret->buf);
    free(ret);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_node_returns_null_without_side_effects(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;

    pid_t pid = fork();
    if (pid < 0) return THEFT_TRIAL_SKIP;
    if (pid == 0) {
        reset_observed();
        struct read_ret *ret = inode_read(NULL, rc->len, rc->offset);
        int ok = ret == NULL && observed_read.calls == 0U && malloc_buffer_calls == 0U;
        free(ret);
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return THEFT_TRIAL_ERROR;
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        fprintf(stderr, "null_node_returns_null_without_side_effects failed: offset=%u len=%u status=%d\n",
                rc->offset, rc->len, status);
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

    printf("baseline extent inode_read property-based tests:\n");
    RUN_PROP("offset_beyond_size_returns_empty", prop_offset_beyond_size_returns_empty, 500);
    RUN_PROP("num_is_clamped_to_file_size", prop_num_is_clamped_to_file_size, 500);
    RUN_PROP("reads_delegate_exact_effective_range", prop_reads_delegate_exact_effective_range, 500);
    RUN_PROP("read_bytes_match_model", prop_read_bytes_match_model, 500);
    RUN_PROP("null_node_returns_null_without_side_effects", prop_null_node_returns_null_without_side_effects, 1);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
