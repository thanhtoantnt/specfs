/*
 * Property-based tests for file_write() in
 * eval/inline_data/optimization/lowlevel_file.c
 *
 * Oracle: algebraic reference model. The real inline_data + page table storage
 * must match a byte-array model for bounded writes around the inline/page
 * transition: written bytes are replaced by data or zeroes, sparse unwritten
 * bytes read as zero, and bytes outside the write range are preserved.
 */
#include <theft.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define TEST_PAGES 6U
#define TEST_STORAGE_BYTES (INLINE_DATA_SIZE + (TEST_PAGES * PG_SIZE))
#define MAX_WRITE_BYTES ((PG_SIZE * 2U) + 128U)

struct write_case {
    unsigned offset;
    unsigned len;
    unsigned snapshot_len;
    unsigned char data[MAX_WRITE_BYTES];
};

struct counter cnt = { .size = 0 };

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static unsigned random_below(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned random_inclusive(struct theft *t, unsigned max_value)
{
    return (unsigned)theft_random_choice(t, (uint64_t)max_value + 1U);
}

static unsigned near_boundary(struct theft *t, unsigned center, unsigned radius)
{
    unsigned delta = random_inclusive(t, radius);
    if (theft_random_choice(t, 2) == 0) {
        return center > delta ? center - delta : 0U;
    }
    return TEST_STORAGE_BYTES - center <= delta ? TEST_STORAGE_BYTES - 1U : center + delta;
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 10)) {
    case 0:
        wc->offset = 0U;
        break;
    case 1:
        wc->offset = INLINE_DATA_SIZE > 0U ? INLINE_DATA_SIZE - 1U : 0U;
        break;
    case 2:
        wc->offset = INLINE_DATA_SIZE;
        break;
    case 3:
        wc->offset = INLINE_DATA_SIZE + 1U;
        break;
    case 4:
        wc->offset = near_boundary(t, INLINE_DATA_SIZE, 256U);
        break;
    case 5:
        wc->offset = near_boundary(t, INLINE_DATA_SIZE + PG_SIZE, 256U);
        break;
    case 6:
        wc->offset = INLINE_DATA_SIZE + (random_below(t, TEST_PAGES) * PG_SIZE);
        break;
    case 7:
        wc->offset = INLINE_DATA_SIZE + (random_below(t, TEST_PAGES) * PG_SIZE) + (PG_SIZE - 1U);
        if (wc->offset >= TEST_STORAGE_BYTES) wc->offset = TEST_STORAGE_BYTES - 1U;
        break;
    default:
        wc->offset = random_below(t, TEST_STORAGE_BYTES);
        break;
    }

    unsigned available = TEST_STORAGE_BYTES - wc->offset;
    unsigned max_len = available < MAX_WRITE_BYTES ? available : MAX_WRITE_BYTES;
    wc->len = 1U + random_below(t, max_len);

    unsigned end = wc->offset + wc->len;
    unsigned trailing = TEST_STORAGE_BYTES - end;
    if (trailing > PG_SIZE) trailing = PG_SIZE;
    wc->snapshot_len = end + random_inclusive(t, trailing);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256U);
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
    fprintf(f, "{offset=%u, len=%u, snapshot_len=%u}",
            wc->offset, wc->len, wc->snapshot_len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static void init_inode(struct inode *node, struct indextb *tb)
{
    memset(node, 0, sizeof(*node));
    memset(tb, 0, sizeof(*tb));
    node->file = tb;
}

static void cleanup_table(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static int allocate_test_pages(struct indextb *tb)
{
    for (unsigned i = 0; i < TEST_PAGES; i++) {
        tb->index[i] = malloc_page();
        if (tb->index[i] == NULL) return 0;
    }
    return 1;
}

static unsigned char storage_byte(const struct inode *node, unsigned offset)
{
    if (offset < INLINE_DATA_SIZE) {
        return (unsigned char)node->inline_data[offset];
    }

    unsigned adjusted = offset - INLINE_DATA_SIZE;
    unsigned page = adjusted / PG_SIZE;
    unsigned page_offset = adjusted % PG_SIZE;
    if (page >= INDEXTB_NUM || node->file->index[page] == NULL) return 0U;
    return node->file->index[page][page_offset];
}

static void snapshot_storage(const struct inode *node, unsigned len, unsigned char *out)
{
    for (unsigned i = 0; i < len; i++) {
        out[i] = storage_byte(node, i);
    }
}

static void fill_existing_storage(struct inode *node, unsigned char *model)
{
    for (unsigned i = 0; i < TEST_STORAGE_BYTES; i++) {
        model[i] = (unsigned char)(0x21U + ((i * 29U) % 223U));
    }

    memcpy(node->inline_data, model, INLINE_DATA_SIZE);
    for (unsigned page = 0; page < TEST_PAGES; page++) {
        memcpy(node->file->index[page],
               model + INLINE_DATA_SIZE + page * PG_SIZE,
               PG_SIZE);
    }
}

static void apply_model_write(unsigned char *model, const struct write_case *wc, bool null_data)
{
    for (unsigned i = 0; i < wc->len; i++) {
        model[wc->offset + i] = null_data ? 0U : wc->data[i];
    }
}

static int compare_or_report(const unsigned char *actual, const unsigned char *expected,
                             unsigned len, const char *name,
                             const struct write_case *wc)
{
    for (unsigned i = 0; i < len; i++) {
        if (actual[i] != expected[i]) {
            fprintf(stderr,
                    "%s mismatch at absolute byte %u: got=%u expected=%u offset=%u len=%u snapshot=%u\n",
                    name, i, actual[i], expected[i], wc->offset, wc->len, wc->snapshot_len);
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    file_write(&node, wc->offset, wc->len, (const char *)wc->data);

    int ok = 1;
    for (unsigned i = 0; i < wc->len; i++) {
        unsigned absolute = wc->offset + i;
        unsigned char got = storage_byte(&node, absolute);
        if (got != wc->data[i]) {
            fprintf(stderr,
                    "roundtrip mismatch at write byte %u absolute=%u got=%u expected=%u offset=%u len=%u\n",
                    i, absolute, got, wc->data[i], wc->offset, wc->len);
            ok = 0;
            break;
        }
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_storage_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    unsigned char *expected = calloc(wc->snapshot_len, 1U);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    apply_model_write(expected, wc, false);
    file_write(&node, wc->offset, wc->len, (const char *)wc->data);
    snapshot_storage(&node, wc->snapshot_len, actual);

    int ok = compare_or_report(actual, expected, wc->snapshot_len,
                               "sparse_storage", wc);

    free(expected);
    free(actual);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_existing_storage_preserves_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);
    if (!allocate_test_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(TEST_STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_existing_storage(&node, expected);
    apply_model_write(expected, wc, false);
    file_write(&node, wc->offset, wc->len, (const char *)wc->data);
    snapshot_storage(&node, wc->snapshot_len, actual);

    int ok = compare_or_report(actual, expected, wc->snapshot_len,
                               "existing_storage", wc);

    free(expected);
    free(actual);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_zero_fills_and_preserves(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);
    if (!allocate_test_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(TEST_STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_existing_storage(&node, expected);
    apply_model_write(expected, wc, true);
    file_write(&node, wc->offset, wc->len, NULL);
    snapshot_storage(&node, wc->snapshot_len, actual);

    int ok = compare_or_report(actual, expected, wc->snapshot_len,
                               "null_data", wc);

    free(expected);
    free(actual);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);
    if (!allocate_test_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(TEST_STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_existing_storage(&node, expected);
    file_write(&node, wc->offset, 0U, (const char *)wc->data);
    snapshot_storage(&node, wc->snapshot_len, actual);

    int ok = compare_or_report(actual, expected, wc->snapshot_len,
                               "zero_length", wc);

    free(expected);
    free(actual);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_allocates_only_touched_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_inode(&node, &tb);

    file_write(&node, wc->offset, wc->len, (const char *)wc->data);

    unsigned first_page = 0U;
    unsigned last_page = 0U;
    bool touches_pages = wc->offset + wc->len > INLINE_DATA_SIZE;
    if (touches_pages) {
        unsigned page_start = wc->offset > INLINE_DATA_SIZE ? wc->offset - INLINE_DATA_SIZE : 0U;
        unsigned page_end = wc->offset + wc->len - 1U - INLINE_DATA_SIZE;
        first_page = page_start / PG_SIZE;
        last_page = page_end / PG_SIZE;
    }

    int ok = 1;
    for (unsigned page = 0; page < TEST_PAGES; page++) {
        bool should_exist = touches_pages && page >= first_page && page <= last_page;
        bool exists = tb.index[page] != NULL;
        if (exists != should_exist) {
            fprintf(stderr,
                    "allocation mismatch page=%u exists=%d expected=%d offset=%u len=%u first=%u last=%u\n",
                    page, exists, should_exist, wc->offset, wc->len, first_page, last_page);
            ok = 0;
            break;
        }
    }

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                  \
        struct theft_run_config cfg = {                                   \
            .name = name_,                                                \
            .prop1 = prop_,                                               \
            .type_info = { &write_case_info },                            \
            .trials = trials_,                                            \
            .seed = theft_seed_of_time(),                                 \
        };                                                                \
        enum theft_run_res res = theft_run(&cfg);                         \
        printf("  [%s] %s\n",                                           \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);        \
        if (res != THEFT_RUN_PASS) failures++;                            \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("inline_data file_write property-based tests:\n");
    RUN_PROP("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    RUN_PROP("sparse_storage_matches_model", prop_sparse_storage_matches_model, 300);
    RUN_PROP("existing_storage_preserves_unwritten_bytes", prop_existing_storage_preserves_unwritten_bytes, 300);
    RUN_PROP("null_data_zero_fills_and_preserves", prop_null_data_zero_fills_and_preserves, 300);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 100);
    RUN_PROP("allocates_only_touched_pages", prop_allocates_only_touched_pages, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
