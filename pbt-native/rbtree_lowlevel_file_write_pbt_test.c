/*
 * Property-based tests for file_write() in
 * eval/rbtree/optimization/lowlevel_file.c
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_WRITE_BYTES 1024U
#define MAX_TEST_PAGES 4U

struct write_case {
    unsigned start_page;
    unsigned page_count;
    unsigned page_off;
    unsigned len;
    unsigned char data[MAX_WRITE_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static enum theft_alloc_res write_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct write_case *wc = malloc(sizeof(*wc));
    if (wc == NULL) return THEFT_ALLOC_ERROR;

    wc->start_page = bounded_choice(t, 16U);
    wc->page_count = 1U + bounded_choice(t, MAX_TEST_PAGES);

    switch (theft_random_choice(t, 5)) {
    case 0: wc->page_off = 0U; break;
    case 1: wc->page_off = 1U; break;
    case 2: wc->page_off = PG_SIZE - 1U; break;
    case 3: wc->page_off = PG_SIZE - 8U; break;
    default: wc->page_off = bounded_choice(t, PG_SIZE); break;
    }

    unsigned span = wc->page_count * PG_SIZE - wc->page_off;
    unsigned max_len = span < MAX_WRITE_BYTES ? span : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

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
    fprintf(f, "{start_page=%u, page_count=%u, page_off=%u, len=%u}",
            wc->start_page, wc->page_count, wc->page_off, wc->len);
}

static struct theft_type_info write_case_info = {
    .alloc = write_case_alloc_cb,
    .free = write_case_free_cb,
    .hash = write_case_hash_cb,
    .print = write_case_print_cb,
};

static struct inode make_inode(void)
{
    struct inode node;
    memset(&node, 0, sizeof(node));
    return node;
}

unsigned char *malloc_contigous_pages(unsigned num)
{
    unsigned char *data = malloc(num * PG_SIZE);
    if (data != NULL) memset(data, 0, num * PG_SIZE);
    return data;
}

static Extent *make_extent(unsigned start_page, unsigned page_count, unsigned char fill)
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
    memset(ext->data, fill, page_count * PG_SIZE);
    ext->next = NULL;
    return ext;
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

static void free_prealloc_tree(struct rb_node *node)
{
    if (node == NULL) return;
    free_prealloc_tree(node->left);
    free_prealloc_tree(node->right);
    free(node->prealloc);
    free(node);
}

static void destroy_inode(struct inode *node)
{
    free_extents(node);
    free_prealloc_tree(node->prealloc_tree);
    node->prealloc_tree = NULL;
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static enum theft_trial_res prop_roundtrip_matches_input(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned offset = case_offset(wc);
    unsigned char out[MAX_WRITE_BYTES];

    memset(out, 0xCC, sizeof(out));
    file_write(&node, offset, wc->len, (const char *)wc->data);
    file_read(&node, offset, wc->len, (char *)out);

    int ok = memcmp(out, wc->data, wc->len) == 0;
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_write_preserves_unrelated_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned extent_bytes = wc->page_count * PG_SIZE;
    unsigned prefix = wc->page_off;
    unsigned suffix_start = prefix + wc->len;
    unsigned char fill = 0xA5;
    int ok = 1;

    node.extents = make_extent(wc->start_page, wc->page_count, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;

    file_write(&node, case_offset(wc), wc->len, (const char *)wc->data);

    for (unsigned i = 0; i < prefix; i++) {
        if (node.extents->data[i] != fill) ok = 0;
    }
    if (ok && memcmp(node.extents->data + prefix, wc->data, wc->len) != 0) ok = 0;
    for (unsigned i = suffix_start; ok && i < extent_bytes; i++) {
        if (node.extents->data[i] != fill) ok = 0;
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_data_zero_fills(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned char out[MAX_WRITE_BYTES];
    int ok = 1;

    memset(out, 0xCC, sizeof(out));
    file_write(&node, case_offset(wc), wc->len, NULL);
    file_read(&node, case_offset(wc), wc->len, (char *)out);

    for (unsigned i = 0; i < wc->len; i++) {
        if (out[i] != 0U) ok = 0;
    }

    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_out_of_range_write_is_rejected(struct theft *t, void *arg1)
{
    const struct write_case *wc = arg1;
    struct inode node = make_inode();
    unsigned char before[PG_SIZE];
    unsigned offset = MAX_FILE_SIZE - bounded_choice(t, 32U) - 1U;
    unsigned len = (MAX_FILE_SIZE - offset) + 1U + bounded_choice(t, 64U);
    unsigned char fill = 0x5AU;

    node.extents = make_extent(INDEXTB_NUM - 1U, 1U, fill);
    if (node.extents == NULL) return THEFT_TRIAL_ERROR;
    memcpy(before, node.extents->data, sizeof(before));

    file_write(&node, offset, len, (const char *)wc->data);

    int ok = memcmp(before, node.extents->data, sizeof(before)) == 0;
    destroy_inode(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                                \
        struct theft_run_config cfg = {                                 \
            .name = name_,                                              \
            .prop1 = prop_,                                             \
            .type_info = { &write_case_info },                          \
            .trials = trials_,                                          \
            .seed = theft_seed_of_time(),                               \
        };                                                              \
        enum theft_run_res res = theft_run(&cfg);                       \
        printf("  [%s] %s\n",                                         \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);       \
        if (res != THEFT_RUN_PASS) failures++;                          \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("rbtree file_write property-based tests:\n");
    RUN_PROP("roundtrip_matches_input", prop_roundtrip_matches_input, 300);
    RUN_PROP("write_preserves_unrelated_bytes", prop_write_preserves_unrelated_bytes, 300);
    RUN_PROP("null_data_zero_fills", prop_null_data_zero_fills, 200);
    RUN_PROP("out_of_range_write_is_rejected", prop_out_of_range_write_is_rejected, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
