/*
 * Property-based tests for file_read() in
 * eval/inline_data/optimization/lowlevel_file.c
 *
 * Oracle: reference byte model over the inline-data prefix plus sparse page
 * table. The properties check exact byte projection across the inline/page
 * boundary, zero-fill behavior for unallocated pages, and the no-op contract
 * for zero-length reads.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_CASE_PAGES 4U
#define MAX_READ_BYTES 1024U
#define MODEL_STORAGE_BYTES (INLINE_DATA_SIZE + (MAX_CASE_PAGES * PG_SIZE))
#define MAX_OFFSET_BOUND (MODEL_STORAGE_BYTES + PG_SIZE)

struct read_case {
    unsigned offset;
    unsigned len;
    unsigned present_mask;
    unsigned char inline_data[INLINE_DATA_SIZE];
    unsigned char page_data[MAX_CASE_PAGES][PG_SIZE];
};

struct counter cnt = { .size = 0 };

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res read_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct read_case *rc = malloc(sizeof(*rc));
    if (rc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 7)) {
    case 0:
        rc->offset = 0U;
        break;
    case 1:
        rc->offset = INLINE_DATA_SIZE > 0U ? INLINE_DATA_SIZE - 1U : 0U;
        break;
    case 2:
        rc->offset = INLINE_DATA_SIZE;
        break;
    case 3:
        rc->offset = INLINE_DATA_SIZE + 1U;
        break;
    case 4:
        rc->offset = INLINE_DATA_SIZE + (bounded_choice(t, MAX_CASE_PAGES) * PG_SIZE) + (PG_SIZE - 1U);
        if (rc->offset >= MAX_OFFSET_BOUND) rc->offset = MAX_OFFSET_BOUND - 1U;
        break;
    default:
        rc->offset = bounded_choice(t, MAX_OFFSET_BOUND);
        break;
    }

    rc->len = 1U + bounded_choice(t, MAX_READ_BYTES);

    rc->present_mask = (unsigned)theft_random_choice(t, (uint64_t)(1U << MAX_CASE_PAGES));
    if (rc->present_mask == 0U) {
        rc->present_mask = 1U << bounded_choice(t, MAX_CASE_PAGES);
    }

    for (unsigned i = 0; i < sizeof(rc->inline_data); i++) {
        rc->inline_data[i] = (unsigned char)theft_random_choice(t, 256U);
    }
    for (unsigned p = 0; p < MAX_CASE_PAGES; p++) {
        for (unsigned i = 0; i < PG_SIZE; i++) {
            rc->page_data[p][i] = (unsigned char)theft_random_choice(t, 256U);
        }
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
    fprintf(f, "{offset=%u, len=%u, present_mask=0x%x}",
            rc->offset, rc->len, rc->present_mask);
}

static struct theft_type_info read_case_info = {
    .alloc = read_case_alloc_cb,
    .free = read_case_free_cb,
    .hash = read_case_hash_cb,
    .print = read_case_print_cb,
};

static struct inode make_inode(struct indextb *tb)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    node.file = tb;
    return node;
}

static void cleanup_table(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static int populate_table(struct indextb *tb, const struct read_case *rc)
{
    for (unsigned p = 0; p < MAX_CASE_PAGES; p++) {
        if (((rc->present_mask >> p) & 1U) == 0U) continue;

        tb->index[p] = malloc_page();
        if (tb->index[p] == NULL) {
            cleanup_table(tb);
            return 0;
        }
        memcpy(tb->index[p], rc->page_data[p], PG_SIZE);
    }
    return 1;
}

static unsigned char model_byte(const struct read_case *rc, unsigned absolute)
{
    if (absolute < INLINE_DATA_SIZE) {
        return rc->inline_data[absolute];
    }

    unsigned adjusted = absolute - INLINE_DATA_SIZE;
    if (adjusted >= (MAX_CASE_PAGES * PG_SIZE)) return 0U;

    unsigned page = adjusted / PG_SIZE;
    unsigned page_off = adjusted % PG_SIZE;
    if (((rc->present_mask >> page) & 1U) == 0U) return 0U;

    return rc->page_data[page][page_off];
}

static void fill_expected(const struct read_case *rc, unsigned offset, unsigned len,
                          unsigned char *expected)
{
    for (unsigned i = 0; i < len; i++) {
        expected[i] = model_byte(rc, offset + i);
    }
}

static void snapshot_storage(const struct inode *node, unsigned char *out)
{
    memcpy(out, node->inline_data, INLINE_DATA_SIZE);
    for (unsigned p = 0; p < MAX_CASE_PAGES; p++) {
        if (node->file != NULL && node->file->index[p] != NULL) {
            memcpy(out + INLINE_DATA_SIZE + (p * PG_SIZE), node->file->index[p], PG_SIZE);
        } else {
            memset(out + INLINE_DATA_SIZE + (p * PG_SIZE), 0, PG_SIZE);
        }
    }
}

static int compare_buffers(const unsigned char *actual, const unsigned char *expected,
                           unsigned len, const char *label, const struct read_case *rc)
{
    for (unsigned i = 0; i < len; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr,
                    "%s mismatch at byte %u: got=%u expected=%u offset=%u len=%u mask=0x%x\n",
                    label, i, actual[i], expected[i], rc->offset, rc->len, rc->present_mask);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_read_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    struct inode node = make_inode(&tb);
    unsigned char *actual = malloc(rc->len);
    unsigned char *expected = malloc(rc->len);
    unsigned char *before = malloc(MODEL_STORAGE_BYTES);
    unsigned char *after = malloc(MODEL_STORAGE_BYTES);

    if (actual == NULL || expected == NULL || before == NULL || after == NULL) {
        free(actual);
        free(expected);
        free(before);
        free(after);
        return THEFT_TRIAL_ERROR;
    }

    memset(&tb, 0, sizeof(tb));
    if (!populate_table(&tb, rc)) {
        free(actual);
        free(expected);
        free(before);
        free(after);
        return THEFT_TRIAL_ERROR;
    }

    memcpy(node.inline_data, rc->inline_data, INLINE_DATA_SIZE);
    snapshot_storage(&node, before);
    memset(actual, 0xCC, rc->len);
    fill_expected(rc, rc->offset, rc->len, expected);

    file_read(&node, rc->offset, rc->len, (char *)actual);
    snapshot_storage(&node, after);

    int ok = compare_buffers(actual, expected, rc->len, "read_model", rc) &&
             memcmp(before, after, MODEL_STORAGE_BYTES) == 0;
    if (!ok) {
        fprintf(stderr, "storage mutated or read mismatch at offset=%u len=%u\n",
                rc->offset, rc->len);
    }

    cleanup_table(&tb);
    free(actual);
    free(expected);
    free(before);
    free(after);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_unallocated_pages_zero_fill(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    struct inode node = make_inode(&tb);
    unsigned offset = INLINE_DATA_SIZE + (rc->offset % (MAX_CASE_PAGES * PG_SIZE));
    unsigned char *out = malloc(rc->len);
    unsigned char *before = malloc(MODEL_STORAGE_BYTES);
    unsigned char *after = malloc(MODEL_STORAGE_BYTES);

    if (out == NULL || before == NULL || after == NULL) {
        free(out);
        free(before);
        free(after);
        return THEFT_TRIAL_ERROR;
    }

    memset(&tb, 0, sizeof(tb));
    memcpy(node.inline_data, rc->inline_data, INLINE_DATA_SIZE);
    snapshot_storage(&node, before);
    memset(out, 0xCC, rc->len);

    file_read(&node, offset, rc->len, (char *)out);
    snapshot_storage(&node, after);

    int ok = 1;
    for (unsigned i = 0; i < rc->len; i++) {
        if (out[i] != 0U) {
            fprintf(stderr, "zero-fill mismatch at byte %u: got=%u offset=%u len=%u\n",
                    i, out[i], offset, rc->len);
            ok = 0;
            break;
        }
    }
    if (ok && memcmp(before, after, MODEL_STORAGE_BYTES) != 0) {
        fprintf(stderr, "unallocated read mutated storage offset=%u len=%u\n",
                offset, rc->len);
        ok = 0;
    }

    free(out);
    free(before);
    free(after);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_read_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct read_case *rc = arg1;
    struct indextb tb;
    struct inode node = make_inode(&tb);
    unsigned char out[16];
    unsigned char before[16];
    unsigned char *snapshot_before = malloc(MODEL_STORAGE_BYTES);
    unsigned char *snapshot_after = malloc(MODEL_STORAGE_BYTES);

    if (snapshot_before == NULL || snapshot_after == NULL) {
        free(snapshot_before);
        free(snapshot_after);
        return THEFT_TRIAL_ERROR;
    }

    memset(&tb, 0, sizeof(tb));
    if (!populate_table(&tb, rc)) {
        free(snapshot_before);
        free(snapshot_after);
        return THEFT_TRIAL_ERROR;
    }

    memcpy(node.inline_data, rc->inline_data, INLINE_DATA_SIZE);
    snapshot_storage(&node, snapshot_before);
    memset(out, 0xA5, sizeof(out));
    memcpy(before, out, sizeof(out));

    file_read(&node, rc->offset, 0U, (char *)out);
    snapshot_storage(&node, snapshot_after);

    int ok = memcmp(out, before, sizeof(out)) == 0 &&
             memcmp(snapshot_before, snapshot_after, MODEL_STORAGE_BYTES) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length read modified state offset=%u\n", rc->offset);
    }

    cleanup_table(&tb);
    free(snapshot_before);
    free(snapshot_after);
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
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);      \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inline_data file_read property-based tests:\n");
    RUN_PROP("read_matches_model", prop_read_matches_model, 300);
    RUN_PROP("unallocated_pages_zero_fill", prop_unallocated_pages_zero_fill, 250);
    RUN_PROP("zero_length_read_is_noop", prop_zero_length_read_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
