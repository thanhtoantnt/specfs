/*
 * Property-based tests for file_read() in
 * eval/extent/optimization/lowlevel_file.c
 *
 * Oracle: algebraic/reference-style byte model for extent-backed reads.
 * Stronger state-machine testing is unnecessary here because file_read is a
 * read-only projection over the inode's extent list. The properties check that
 * allocated ranges are copied exactly, sparse/unallocated pages read as zeroes,
 * reads do not mutate stored data, and zero-length reads are a no-op.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_READ_BYTES 1024U
#define MAX_EXTENT_PAGES 4U
#define MAX_TEST_PAGE 16U

struct read_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_EXTENT_PAGES * PG_SIZE];
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
    rc->page_count = 1U + bounded_choice(t, MAX_EXTENT_PAGES);

    switch (theft_random_choice(t, 5U)) {
    case 0:
        rc->page_off = 0U;
        break;
    case 1:
        rc->page_off = PG_SIZE - 1U;
        break;
    case 2:
        rc->page_off = PG_SIZE - 16U;
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

static struct Extent *make_extent(unsigned start_page, unsigned page_count,
                                  const unsigned char *data)
{
    struct Extent *ext = malloc(sizeof(*ext));
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

static void free_extents(struct inode *node)
{
    struct Extent *cur = node->extents;
    while (cur != NULL) {
        struct Extent *next = cur->next;
        free(cur->data);
        free(cur);
        cur = next;
    }
    node->extents = NULL;
}

static unsigned case_offset(const struct read_case *rc)
{
    return rc->start_page * PG_SIZE + rc->page_off;
}

static enum theft_trial_res prop_allocated_read_matches_extent_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char out[MAX_READ_BYTES];
    memset(out, 0xCC, sizeof(out));
    file_read(&node, offset, rc->len, (char *)out);

    int ok = memcmp(out, rc->data + rc->page_off, rc->len) == 0;
    if (!ok) {
        fprintf(stderr, "allocated read mismatch: offset=%u len=%u\n", offset, rc->len);
    }

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_read_zero_fills_gaps(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);
    unsigned read_start = offset - PG_SIZE;
    unsigned read_len = rc->len + (2U * PG_SIZE);

    if (rc->start_page == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char *out = malloc(read_len);
    if (out == NULL) {
        free_extents(&node);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, read_len);
    file_read(&node, read_start, read_len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < read_len; i++) {
        unsigned absolute = read_start + i;
        unsigned page = absolute / PG_SIZE;
        unsigned expected = 0U;
        if (page >= rc->start_page && page < rc->start_page + rc->page_count) {
            unsigned extent_offset = (page - rc->start_page) * PG_SIZE + (absolute % PG_SIZE);
            expected = rc->data[extent_offset];
        }
        if (out[i] != expected) {
            fprintf(stderr,
                    "sparse read mismatch at byte=%u absolute=%u got=%u expected=%u\n",
                    i, absolute, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_unallocated_read_returns_zeroes(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);

    unsigned char out[MAX_READ_BYTES];
    memset(out, 0xCC, sizeof(out));
    file_read(&node, offset, rc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < rc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "unallocated read returned non-zero at %u: %u\n", i, out[i]);
            ok = 0;
            break;
        }
    }
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_read_does_not_mutate_allocated_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(rc);

    node.extents = make_extent(rc->start_page, rc->page_count, rc->data);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    unsigned char before[MAX_EXTENT_PAGES * PG_SIZE];
    memcpy(before, rc->data, sizeof(before));

    unsigned char out[MAX_READ_BYTES];
    memset(out, 0xCC, sizeof(out));
    file_read(&node, offset, rc->len, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < rc->page_count; i++) {
        const unsigned char *page = node.extents->data + (i * PG_SIZE);
        if (memcmp(page, before + (i * PG_SIZE), PG_SIZE) != 0) {
            fprintf(stderr, "read mutated allocated page %u\n", rc->start_page + i);
            ok = 0;
            break;
        }
    }

    free_extents(&node);
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
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(before));
    file_read(&node, offset, 0U, (char *)out);

    int ok = memcmp(out, before, sizeof(out)) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length read modified destination\n");
    }

    free_extents(&node);
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

    printf("file_read extent property-based tests:\n");
    RUN_PROP("allocated_read_matches_extent_bytes", prop_allocated_read_matches_extent_bytes, 300);
    RUN_PROP("sparse_read_zero_fills_gaps", prop_sparse_read_zero_fills_gaps, 300);
    RUN_PROP("unallocated_read_returns_zeroes", prop_unallocated_read_returns_zeroes, 200);
    RUN_PROP("read_does_not_mutate_allocated_pages", prop_read_does_not_mutate_allocated_pages, 200);
    RUN_PROP("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
