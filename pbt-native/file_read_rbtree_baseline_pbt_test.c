/*
 * Property-based tests for file_read() in
 * eval/rbtree/baseline/lowlevel_file.c
 *
 * Oracle: reference byte model over extent-backed reads.
 * Stronger considered:
 *   - State machine: rejected — file_read has no explicit lifecycle/state.
 *   - Differential: rejected — there is no independent second implementation
 *     of this exact lowlevel read contract in the baseline tree.
 * Weaker available: Crash-only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_READ_BYTES (3U * PG_SIZE + 17U)
#define MAX_TEST_PAGES 4U
#define MAX_TEST_PAGE 16U

struct read_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_TEST_PAGES * PG_SIZE];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    rc->start_page = bounded_choice(t, MAX_TEST_PAGE);
    rc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 6U)) {
    case 0:
        rc->page_off = 0U;
        break;
    case 1:
        rc->page_off = 1U;
        break;
    case 2:
        rc->page_off = PG_SIZE - 1U;
        break;
    case 3:
        rc->page_off = PG_SIZE - 16U;
        break;
    case 4:
        rc->page_off = PG_SIZE / 2U;
        break;
    default:
        rc->page_off = bounded_choice(t, PG_SIZE);
        break;
    }

    unsigned span_bytes = rc->page_count * PG_SIZE - rc->page_off;
    unsigned max_len = span_bytes < MAX_READ_BYTES ? span_bytes : MAX_READ_BYTES;
    rc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < sizeof(rc->data); i++) {
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
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u}",
            rc->start_page, rc->page_count, rc->page_off, rc->len);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
};

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

static void free_extents(struct inode *node)
{
    Extent *cur = node->extents;
    while (cur != NULL) {
        Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static void destroy_inode(struct inode *node)
{
    free_extents(node);
}

static Extent *make_extent(unsigned start_page, unsigned page_count,
                           const unsigned char *data)
{
    Extent *ext = malloc(sizeof(*ext));
    if (ext == NULL) return NULL;

    ext->start_page = start_page;
    ext->length = page_count;
    ext->data = malloc(page_count * PG_SIZE);
    if (ext->data == NULL) {
        free(ext);
        return NULL;
    }
    memcpy(ext->data, data, page_count * PG_SIZE);
    ext->next = NULL;
    return ext;
}

static unsigned case_offset(const struct read_case *rc)
{
    return rc->start_page * PG_SIZE + rc->page_off;
}

static unsigned extent_bytes(const struct read_case *rc)
{
    return rc->page_count * PG_SIZE;
}

static enum theft_trial_res prop_allocated_read_matches_extent_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);
    unsigned bytes = extent_bytes(rc);
    unsigned char out[MAX_READ_BYTES];
    unsigned char *before = malloc(bytes);

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL || before == NULL) {
        free(before);
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    memcpy(before, node.extents->data, bytes);
    memset(out, 0xCC, sizeof(out));
    file_read(&node, offset, rc->len, (char *)out);

    int ok = memcmp(out, rc->data + rc->page_off, rc->len) == 0 &&
             memcmp(before, node.extents->data, bytes) == 0;
    if (!ok) {
        fprintf(stderr, "allocated read mismatch offset=%u len=%u\n", offset, rc->len);
    }

    free(before);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_read_zero_fills_gaps(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    if (rc->start_page == 0U) return THEFT_TRIAL_SKIP;

    struct inode node = make_inode();
    unsigned offset = case_offset(rc);
    unsigned read_start = offset - PG_SIZE;
    unsigned read_len = rc->len + (2U * PG_SIZE);
    unsigned char *out = malloc(read_len);
    int ok = 1;

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL || out == NULL) {
        free(out);
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    memset(out, 0xCC, read_len);
    file_read(&node, read_start, read_len, (char *)out);

    for (unsigned i = 0; i < read_len; i++) {
        unsigned absolute = read_start + i;
        unsigned page = absolute / PG_SIZE;
        unsigned expected = 0U;
        if (page >= rc->start_page && page < rc->start_page + rc->page_count) {
            unsigned page_offset = absolute % PG_SIZE;
            expected = rc->data[((page - rc->start_page) * PG_SIZE) + page_offset];
        }
        if (out[i] != expected) {
            fprintf(stderr,
                    "sparse read mismatch byte=%u absolute=%u got=%u expected=%u\n",
                    i, absolute, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_unallocated_read_returns_zeroes_and_does_not_mutate(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);
    unsigned oob_offset = MAX_FILE_SIZE + rc->page_off;
    unsigned char out[MAX_READ_BYTES];
    int ok = 1;

    for (unsigned c = 0; c < 2U; c++) {
        unsigned cur_offset = c == 0U ? offset : oob_offset;
        memset(out, 0xCC, sizeof(out));
        file_read(&node, cur_offset, rc->len, (char *)out);
        for (unsigned i = 0; i < rc->len; i++) {
            if (out[i] != 0U) {
                fprintf(stderr, "unallocated read returned non-zero offset=%u byte=%u got=%u\n",
                        cur_offset, i, out[i]);
                ok = 0;
                break;
            }
        }
        if (!ok) break;
    }

    if (node.extents != NULL) {
        fprintf(stderr, "unallocated read allocated extents unexpectedly\n");
        ok = 0;
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_read_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);
    unsigned char out[16];
    unsigned char before[16];

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL) {
        destroy_inode(&node);
        return THEFT_TRIAL_ERROR;
    }

    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(before));
    file_read(&node, offset, 0U, (char *)out);

    int ok = memcmp(out, before, sizeof(out)) == 0 &&
             memcmp(node.extents->data, rc->data, extent_bytes(rc)) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length read modified destination or source\n");
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                 \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &read_case_info },                           \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                          \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("rbtree baseline file_read property-based tests:\n");
    RUN_PROP("allocated_read_matches_extent_bytes", prop_allocated_read_matches_extent_bytes, 300);
    RUN_PROP("sparse_read_zero_fills_gaps", prop_sparse_read_zero_fills_gaps, 300);
    RUN_PROP("unallocated_read_returns_zeroes_and_does_not_mutate", prop_unallocated_read_returns_zeroes_and_does_not_mutate, 200);
    RUN_PROP("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
