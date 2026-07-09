/*
 * Property-based tests for dispose_inode() in
 * eval/inline_data/baseline/util.c
 *
 * Oracle: Algebraic - destructor ownership/lifecycle invariant from the
 * dispose_inode spec: a valid inode destructor must release the mode-specific
 * payload it owns, destroy the inode mutex, and free the inode itself exactly
 * once. For inline-data file inodes, every non-NULL page pointer in the index
 * table is owned by the inode and must be released before the table is freed.
 * Stronger considered:
 *   - State Machine: rejected, dispose_inode is a terminal single-call
 *     destructor with no valid post-dispose state to inspect.
 *   - Differential: rejected, no independent inline_data baseline inode
 *     destructor exists.
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "mcs.h"

#define MAX_TEST_PAGES 32U
#define MAX_FREE_EVENTS (MAX_TEST_PAGES + 8U)
#define EVENT_INODE 1U
#define EVENT_DIR 2U
#define EVENT_FILE_TABLE 3U
#define EVENT_PAGE 4U
#define EVENT_UNKNOWN 5U
#define EVENT_MUTEX_DESTROY 6U

struct dispose_case {
    unsigned page_count;
    unsigned mode_selector;
    unsigned first_sparse_slot;
    unsigned sparse_stride;
    size_t page_sizes[MAX_TEST_PAGES];
};

static int tracker_enabled;
static const void *expected_inode;
static const void *expected_dir;
static const void *expected_file_table;
static const void *expected_mutex;
static const void *expected_pages[MAX_TEST_PAGES];
static unsigned expected_page_slots[MAX_TEST_PAGES];
static unsigned expected_page_count;
static unsigned observed_inode_frees;
static unsigned observed_dir_frees;
static unsigned observed_file_table_frees;
static unsigned observed_mutex_destroys;
static unsigned observed_page_frees[MAX_TEST_PAGES];
static unsigned observed_unknown_frees;
static unsigned event_kinds[MAX_FREE_EVENTS];
static unsigned event_count;

static void reset_tracker(void)
{
    tracker_enabled = 0;
    expected_inode = NULL;
    expected_dir = NULL;
    expected_file_table = NULL;
    expected_mutex = NULL;
    expected_page_count = 0U;
    observed_inode_frees = 0U;
    observed_dir_frees = 0U;
    observed_file_table_frees = 0U;
    observed_mutex_destroys = 0U;
    observed_unknown_frees = 0U;
    event_count = 0U;
    memset(expected_pages, 0, sizeof(expected_pages));
    memset(expected_page_slots, 0, sizeof(expected_page_slots));
    memset(observed_page_frees, 0, sizeof(observed_page_frees));
    memset(event_kinds, 0, sizeof(event_kinds));
}

static void record_event(unsigned kind)
{
    if (event_count < MAX_FREE_EVENTS) {
        event_kinds[event_count] = kind;
    }
    event_count++;
}

static void tracked_free(void *ptr)
{
    if (tracker_enabled && ptr != NULL) {
        int matched = 0;

        if (ptr == expected_inode) {
            observed_inode_frees++;
            record_event(EVENT_INODE);
            matched = 1;
        } else if (ptr == expected_dir) {
            observed_dir_frees++;
            record_event(EVENT_DIR);
            matched = 1;
        } else if (ptr == expected_file_table) {
            observed_file_table_frees++;
            record_event(EVENT_FILE_TABLE);
            matched = 1;
        }

        for (unsigned i = 0U; !matched && i < expected_page_count; i++) {
            if (ptr == expected_pages[i]) {
                observed_page_frees[i]++;
                record_event(EVENT_PAGE);
                matched = 1;
            }
        }

        if (!matched) {
            observed_unknown_frees++;
            record_event(EVENT_UNKNOWN);
        }
    }

    free(ptr);
}

static int tracked_mcs_mutex_destroy(mcs_mutex_t *lock)
{
    if (tracker_enabled) {
        if (lock == expected_mutex) {
            observed_mutex_destroys++;
            record_event(EVENT_MUTEX_DESTROY);
        } else {
            observed_unknown_frees++;
            record_event(EVENT_UNKNOWN);
        }
    }
    return mcs_mutex_destroy(lock);
}

#define free tracked_free
#define mcs_mutex_destroy tracked_mcs_mutex_destroy
#include "../eval/inline_data/baseline/util.c"
#undef mcs_mutex_destroy
#undef free

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_page_count(struct theft *t)
{
    switch (theft_random_choice(t, 8U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 2U;
    case 3:
        return MAX_TEST_PAGES;
    default:
        return 1U + bounded_choice(t, MAX_TEST_PAGES);
    }
}

static size_t draw_page_size(struct theft *t)
{
    switch (theft_random_choice(t, 8U)) {
    case 0:
        return 1U;
    case 1:
        return PG_SIZE;
    case 2:
        return PG_SIZE + 1U;
    default:
        return 1U + (size_t)bounded_choice(t, 512U);
    }
}

static enum theft_alloc_res dispose_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct dispose_case *dc = calloc(1U, sizeof(*dc));
    if (dc == NULL) {
        return THEFT_ALLOC_ERROR;
    }

    dc->page_count = draw_page_count(t);
    dc->mode_selector = bounded_choice(t, 6U);
    dc->first_sparse_slot = 1U + bounded_choice(t, 16U);
    dc->sparse_stride = 1U + bounded_choice(t, 16U);
    for (unsigned i = 0U; i < dc->page_count; i++) {
        dc->page_sizes[i] = draw_page_size(t);
    }

    *instance = dc;
    return THEFT_ALLOC_OK;
}

static void dispose_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash dispose_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    const struct dispose_case *dc = instance;
    struct theft_hasher h;
    theft_hash_init(&h);
    theft_hash_sink(&h, (const uint8_t *)&dc->page_count, sizeof(dc->page_count));
    theft_hash_sink(&h, (const uint8_t *)&dc->mode_selector, sizeof(dc->mode_selector));
    theft_hash_sink(&h, (const uint8_t *)&dc->first_sparse_slot, sizeof(dc->first_sparse_slot));
    theft_hash_sink(&h, (const uint8_t *)&dc->sparse_stride, sizeof(dc->sparse_stride));
    theft_hash_sink(&h, (const uint8_t *)dc->page_sizes, sizeof(dc->page_sizes));
    return theft_hash_done(&h);
}

static void dispose_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct dispose_case *dc = instance;
    fprintf(f, "{page_count=%u, mode_selector=%u, first_sparse_slot=%u, sparse_stride=%u}",
            dc->page_count, dc->mode_selector, dc->first_sparse_slot, dc->sparse_stride);
}

static struct theft_type_info dispose_case_info = {
    .alloc = dispose_case_alloc_cb,
    .free = dispose_case_free_cb,
    .hash = dispose_case_hash_cb,
    .print = dispose_case_print_cb,
};

static int select_non_dir_mode(unsigned selector)
{
    static const int modes[] = { FILE_MODE, CHR_MODE, BLK_MODE, SOCK_MODE, FIFO_MODE, 0 };
    return modes[selector % (sizeof(modes) / sizeof(modes[0]))];
}

static int append_page_at_slot(struct inode *node, unsigned expected_index,
                               unsigned slot, size_t page_size)
{
    unsigned char *page = malloc(page_size);
    if (page == NULL) {
        return 0;
    }
    memset(page, (int)(0xa0U + expected_index), page_size);
    node->file->index[slot] = page;
    return 1;
}

static void capture_expectations(struct inode *node, const unsigned *slots, unsigned slot_count)
{
    reset_tracker();
    expected_inode = node;
    expected_dir = node->dir;
    expected_file_table = node->file;
    expected_mutex = node->impl;
    expected_page_count = slot_count;
    for (unsigned i = 0U; i < slot_count; i++) {
        expected_page_slots[i] = slots[i];
        expected_pages[i] = node->file->index[slots[i]];
    }
    tracker_enabled = 1;
}

static void cleanup_unfreed_pages(void)
{
    tracker_enabled = 0;
    for (unsigned i = 0U; i < expected_page_count; i++) {
        if (expected_pages[i] != NULL && observed_page_frees[i] == 0U) {
            free((void *)expected_pages[i]);
        }
    }
}

static int mutex_file_table_and_inode_released_once(void)
{
    return observed_mutex_destroys == 1U &&
           observed_file_table_frees == 1U &&
           observed_inode_frees == 1U;
}

static int mutex_and_inode_released_once(void)
{
    return observed_mutex_destroys == 1U && observed_inode_frees == 1U;
}

static int all_pages_released_once(void)
{
    for (unsigned i = 0U; i < expected_page_count; i++) {
        if (observed_page_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static enum theft_trial_res prop_directory_releases_owned_fields(struct theft *t, void *arg1)
{
    (void)t;
    (void)arg1;
    struct inode *node = malloc_inode(DIR_MODE, 0U, 0U);
    if (node == NULL || node->dir == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    capture_expectations(node, NULL, 0U);
    dispose_inode(node);

    int ok = mutex_and_inode_released_once() &&
             observed_dir_frees == 1U &&
             observed_file_table_frees == 0U &&
             expected_page_count == 0U &&
             observed_unknown_frees == 0U;
    tracker_enabled = 0;
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_non_dir_releases_table_inode_and_mutex(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 1U, 2U);
    if (node == NULL || node->file == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    capture_expectations(node, NULL, 0U);
    dispose_inode(node);

    int ok = mutex_file_table_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             expected_page_count == 0U &&
             observed_unknown_frees == 0U;
    tracker_enabled = 0;
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_contiguous_pages_are_released(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    if (dc->page_count == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 3U, 4U);
    if (node == NULL || node->file == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    unsigned slots[MAX_TEST_PAGES];
    for (unsigned i = 0U; i < dc->page_count; i++) {
        slots[i] = i;
        if (!append_page_at_slot(node, i, slots[i], dc->page_sizes[i])) {
            return THEFT_TRIAL_ERROR;
        }
    }

    capture_expectations(node, slots, dc->page_count);
    dispose_inode(node);

    int ok = mutex_file_table_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             all_pages_released_once() &&
             observed_unknown_frees == 0U;
    if (!ok) {
        fprintf(stderr,
                "dispose_inode contiguous page cleanup mismatch: pages=%u mutex=%u file=%u inode=%u first_page_free=%u unknown=%u\n",
                expected_page_count, observed_mutex_destroys, observed_file_table_frees,
                observed_inode_frees, expected_page_count > 0U ? observed_page_frees[0] : 0U,
                observed_unknown_frees);
    }
    cleanup_unfreed_pages();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_pages_are_released(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    if (dc->page_count == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 5U, 6U);
    if (node == NULL || node->file == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    unsigned slots[MAX_TEST_PAGES];
    for (unsigned i = 0U; i < dc->page_count; i++) {
        slots[i] = dc->first_sparse_slot + (i * dc->sparse_stride);
        if (slots[i] >= INDEXTB_NUM || !append_page_at_slot(node, i, slots[i], dc->page_sizes[i])) {
            return THEFT_TRIAL_ERROR;
        }
    }

    capture_expectations(node, slots, dc->page_count);
    dispose_inode(node);

    int ok = mutex_file_table_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             all_pages_released_once() &&
             observed_unknown_frees == 0U;
    if (!ok) {
        fprintf(stderr,
                "dispose_inode sparse page cleanup mismatch: pages=%u first_slot=%u mutex=%u file=%u inode=%u first_page_free=%u unknown=%u\n",
                expected_page_count, expected_page_count > 0U ? expected_page_slots[0] : 0U,
                observed_mutex_destroys, observed_file_table_frees, observed_inode_frees,
                expected_page_count > 0U ? observed_page_frees[0] : 0U,
                observed_unknown_frees);
    }
    cleanup_unfreed_pages();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                      \
    do {                                                                     \
        struct theft_run_config cfg = {                                      \
            .name = name_,                                                   \
            .prop1 = prop_,                                                  \
            .type_info = { &dispose_case_info },                             \
            .trials = trials_,                                               \
            .seed = theft_seed_of_time(),                                    \
        };                                                                   \
        enum theft_run_res res = theft_run(&cfg);                            \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("dispose_inode inline_data baseline property-based tests:\n");
    RUN_PROP("directory_releases_owned_fields", prop_directory_releases_owned_fields, 300);
    RUN_PROP("empty_non_dir_releases_table_inode_and_mutex", prop_empty_non_dir_releases_table_inode_and_mutex, 300);
    RUN_PROP("contiguous_pages_are_released", prop_contiguous_pages_are_released, 150);
    RUN_PROP("sparse_pages_are_released", prop_sparse_pages_are_released, 150);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
