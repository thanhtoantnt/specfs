/*
 * Property-based tests for file_write() in
 * eval/loc/gen/inline-data/lowlevel_file.c
 *
 * Oracle: algebraic invariant/reference-model. For every generated bounded
 * write, the real inline-data + paged storage must match a byte-vector model:
 * the target range is replaced by data (or zeroes when data == NULL), sparse
 * unwritten bytes read as zero, and bytes outside the write range are preserved.
 */
#include <theft.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lowlevel_file.h"

#define STORAGE_BYTES (INLINE_DATA_SIZE + (INDEXTB_NUM * PG_SIZE))
#define MAX_WRITE_BYTES (PG_SIZE * 3U)

struct write_case {
    unsigned offset;
    unsigned len;
    unsigned snapshot_len;
    unsigned char data[MAX_WRITE_BYTES];
};

void *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

unsigned int min(unsigned int a, unsigned int b)
{
    return a < b ? a : b;
}

unsigned int max(unsigned int a, unsigned int b)
{
    return a > b ? a : b;
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
    return STORAGE_BYTES - center < delta ? STORAGE_BYTES - 1U : center + delta;
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    switch (theft_random_choice(t, 8)) {
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
        wc->offset = near_boundary(t, INLINE_DATA_SIZE, PG_SIZE);
        break;
    case 4: {
        unsigned page = random_below(t, INDEXTB_NUM);
        wc->offset = INLINE_DATA_SIZE + page * PG_SIZE;
        break;
    }
    case 5: {
        unsigned page = random_below(t, INDEXTB_NUM);
        wc->offset = INLINE_DATA_SIZE + page * PG_SIZE + (PG_SIZE - 1U);
        if (wc->offset >= STORAGE_BYTES) wc->offset = STORAGE_BYTES - 1U;
        break;
    }
    default:
        wc->offset = random_below(t, STORAGE_BYTES);
        break;
    }

    unsigned available = STORAGE_BYTES - wc->offset;
    unsigned max_len = available < MAX_WRITE_BYTES ? available : MAX_WRITE_BYTES;
    wc->len = 1U + random_below(t, max_len);

    unsigned max_snapshot = wc->offset + wc->len;
    unsigned extra = STORAGE_BYTES - max_snapshot;
    if (extra > PG_SIZE) extra = PG_SIZE;
    wc->snapshot_len = max_snapshot + random_inclusive(t, extra);

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

static int allocate_all_pages(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        tb->index[i] = malloc_page();
        if (tb->index[i] == NULL) return 0;
    }
    return 1;
}

static void fill_storage(struct inode *node, unsigned char *model, unsigned len)
{
    for (unsigned i = 0; i < len; i++) {
        model[i] = (unsigned char)(0x31U + ((i * 17U) % 191U));
    }

    memcpy(node->inline_data, model, INLINE_DATA_SIZE);
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
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

static int read_storage(struct inode *node, unsigned len, unsigned char *out)
{
    if (len == 0U) return 1;
    memset(out, 0xCC, len);
    file_read(node, 0U, len, (char *)out);
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

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, wc->len);
    file_read(&node, wc->offset, wc->len, (char *)out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    if (!ok) fprintf(stderr, "roundtrip mismatch: offset=%u len=%u\n", wc->offset, wc->len);

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_unwritten_bytes_are_zero(struct theft *t, void *arg1)
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
    read_storage(&node, wc->snapshot_len, actual);

    int ok = memcmp(actual, expected, wc->snapshot_len) == 0;
    if (!ok) fprintf(stderr, "sparse model mismatch: offset=%u len=%u snapshot=%u\n",
                     wc->offset, wc->len, wc->snapshot_len);

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
    if (!allocate_all_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_storage(&node, expected, STORAGE_BYTES);
    apply_model_write(expected, wc, false);
    file_write(&node, wc->offset, wc->len, (const char *)wc->data);
    read_storage(&node, wc->snapshot_len, actual);

    int ok = memcmp(actual, expected, wc->snapshot_len) == 0;
    if (!ok) fprintf(stderr, "preservation mismatch: offset=%u len=%u snapshot=%u\n",
                     wc->offset, wc->len, wc->snapshot_len);

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
    if (!allocate_all_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_storage(&node, expected, STORAGE_BYTES);
    apply_model_write(expected, wc, true);
    file_write(&node, wc->offset, wc->len, NULL);
    read_storage(&node, wc->snapshot_len, actual);

    int ok = memcmp(actual, expected, wc->snapshot_len) == 0;
    if (!ok) fprintf(stderr, "null-data model mismatch: offset=%u len=%u snapshot=%u\n",
                     wc->offset, wc->len, wc->snapshot_len);

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
    if (!allocate_all_pages(&tb)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *expected = malloc(STORAGE_BYTES);
    unsigned char *actual = malloc(wc->snapshot_len);
    if (expected == NULL || actual == NULL) {
        free(expected);
        free(actual);
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    fill_storage(&node, expected, STORAGE_BYTES);
    file_write(&node, wc->offset, 0U, (const char *)wc->data);
    read_storage(&node, wc->snapshot_len, actual);

    int ok = memcmp(actual, expected, wc->snapshot_len) == 0;
    if (!ok) fprintf(stderr, "zero-length write changed storage: offset=%u\n", wc->offset);

    free(expected);
    free(actual);
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

    printf("inline_data loc file_write property-based tests:\n");
    RUN_PROP("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    RUN_PROP("sparse_unwritten_bytes_are_zero", prop_sparse_unwritten_bytes_are_zero, 300);
    RUN_PROP("existing_storage_preserves_unwritten_bytes", prop_existing_storage_preserves_unwritten_bytes, 300);
    RUN_PROP("null_data_zero_fills_and_preserves", prop_null_data_zero_fills_and_preserves, 300);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
