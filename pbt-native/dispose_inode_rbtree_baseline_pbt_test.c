/*
 * Property-based tests for dispose_inode() in
 * eval/rbtree/baseline/util.c
 *
 * Oracle: Algebraic - destructor ownership/lifecycle invariant from the
 * dispose_inode spec: a valid inode destructor must release the mode-specific
 * payload it owns, destroy the inode mutex, and free the inode itself exactly
 * once. For non-directory inodes, every owned extent node/data payload and
 * every owned prealloc node/data payload must also be released.
 * Stronger considered:
 *   - State Machine: rejected, dispose_inode is a terminal single-call
 *     destructor with no valid post-dispose state to inspect.
 *   - Differential: rejected, no independent rbtree-baseline inode destructor exists.
 * Weaker available: Crash-Only.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "mcs.h"

#define MAX_TEST_EXTENTS 12U
#define MAX_TEST_PREALLOCS 12U
#define MAX_FREE_EVENTS (2U * MAX_TEST_EXTENTS + 2U * MAX_TEST_PREALLOCS + 8U)
#define EVENT_INODE 1U
#define EVENT_DIR 2U
#define EVENT_EXTENT_NODE 3U
#define EVENT_EXTENT_DATA 4U
#define EVENT_PREALLOC_NODE 5U
#define EVENT_PREALLOC_DATA 6U
#define EVENT_PREALLOC_ARRAY_DATA 7U
#define EVENT_UNKNOWN 8U
#define EVENT_MUTEX_DESTROY 9U

struct dispose_case {
    unsigned extent_count;
    unsigned prealloc_count;
    unsigned mode_selector;
    unsigned start_pages[MAX_TEST_EXTENTS];
    unsigned lengths[MAX_TEST_EXTENTS];
    size_t extent_payload_sizes[MAX_TEST_EXTENTS];
    unsigned prealloc_starts[MAX_TEST_PREALLOCS];
    unsigned prealloc_lengths[MAX_TEST_PREALLOCS];
    size_t prealloc_payload_sizes[MAX_TEST_PREALLOCS];
};

static int tracker_enabled;
static const void *expected_inode;
static const void *expected_dir;
static const void *expected_mutex;
static const void *expected_extent_nodes[MAX_TEST_EXTENTS];
static const void *expected_extent_data[MAX_TEST_EXTENTS];
static const void *expected_prealloc_nodes[MAX_TEST_PREALLOCS];
static const void *expected_prealloc_data[MAX_TEST_PREALLOCS];
static const void *expected_prealloc_array_data[MAX_TEST_PREALLOCS];
static unsigned expected_extent_count;
static unsigned expected_prealloc_count;
static unsigned expected_prealloc_array_count;
static unsigned observed_inode_frees;
static unsigned observed_dir_frees;
static unsigned observed_mutex_destroys;
static unsigned observed_extent_node_frees[MAX_TEST_EXTENTS];
static unsigned observed_extent_data_frees[MAX_TEST_EXTENTS];
static unsigned observed_prealloc_node_frees[MAX_TEST_PREALLOCS];
static unsigned observed_prealloc_data_frees[MAX_TEST_PREALLOCS];
static unsigned observed_prealloc_array_data_frees[MAX_TEST_PREALLOCS];
static unsigned observed_unknown_frees;
static unsigned event_kinds[MAX_FREE_EVENTS];
static unsigned event_count;

static void reset_tracker(void)
{
    tracker_enabled = 0;
    expected_inode = NULL;
    expected_dir = NULL;
    expected_mutex = NULL;
    expected_extent_count = 0U;
    expected_prealloc_count = 0U;
    expected_prealloc_array_count = 0U;
    observed_inode_frees = 0U;
    observed_dir_frees = 0U;
    observed_mutex_destroys = 0U;
    observed_unknown_frees = 0U;
    event_count = 0U;
    memset(expected_extent_nodes, 0, sizeof(expected_extent_nodes));
    memset(expected_extent_data, 0, sizeof(expected_extent_data));
    memset(expected_prealloc_nodes, 0, sizeof(expected_prealloc_nodes));
    memset(expected_prealloc_data, 0, sizeof(expected_prealloc_data));
    memset(expected_prealloc_array_data, 0, sizeof(expected_prealloc_array_data));
    memset(observed_extent_node_frees, 0, sizeof(observed_extent_node_frees));
    memset(observed_extent_data_frees, 0, sizeof(observed_extent_data_frees));
    memset(observed_prealloc_node_frees, 0, sizeof(observed_prealloc_node_frees));
    memset(observed_prealloc_data_frees, 0, sizeof(observed_prealloc_data_frees));
    memset(observed_prealloc_array_data_frees, 0, sizeof(observed_prealloc_array_data_frees));
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
        }

        for (unsigned i = 0U; !matched && i < expected_extent_count; i++) {
            if (ptr == expected_extent_nodes[i]) {
                observed_extent_node_frees[i]++;
                record_event(EVENT_EXTENT_NODE);
                matched = 1;
            } else if (ptr == expected_extent_data[i]) {
                observed_extent_data_frees[i]++;
                record_event(EVENT_EXTENT_DATA);
                matched = 1;
            }
        }

        for (unsigned i = 0U; !matched && i < expected_prealloc_count; i++) {
            if (ptr == expected_prealloc_nodes[i]) {
                observed_prealloc_node_frees[i]++;
                record_event(EVENT_PREALLOC_NODE);
                matched = 1;
            } else if (ptr == expected_prealloc_data[i]) {
                observed_prealloc_data_frees[i]++;
                record_event(EVENT_PREALLOC_DATA);
                matched = 1;
            }
        }

        for (unsigned i = 0U; !matched && i < expected_prealloc_array_count; i++) {
            if (ptr == expected_prealloc_array_data[i]) {
                observed_prealloc_array_data_frees[i]++;
                record_event(EVENT_PREALLOC_ARRAY_DATA);
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
#include "../eval/rbtree/baseline/util.c"
#undef mcs_mutex_destroy
#undef free

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_extent_count(struct theft *t)
{
    switch (theft_random_choice(t, 8U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 2U;
    case 3:
        return MAX_TEST_EXTENTS;
    default:
        return 1U + bounded_choice(t, MAX_TEST_EXTENTS);
    }
}

static unsigned draw_prealloc_count(struct theft *t)
{
    switch (theft_random_choice(t, 8U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return 2U;
    case 3:
        return MAX_TEST_PREALLOCS;
    default:
        return 1U + bounded_choice(t, MAX_TEST_PREALLOCS);
    }
}

static size_t draw_payload_size(struct theft *t)
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

    dc->extent_count = draw_extent_count(t);
    dc->prealloc_count = draw_prealloc_count(t);
    dc->mode_selector = bounded_choice(t, 6U);
    for (unsigned i = 0U; i < dc->extent_count; i++) {
        dc->start_pages[i] = bounded_choice(t, 128U);
        dc->lengths[i] = 1U + bounded_choice(t, 4U);
        dc->extent_payload_sizes[i] = draw_payload_size(t);
    }
    for (unsigned i = 0U; i < dc->prealloc_count; i++) {
        dc->prealloc_starts[i] = bounded_choice(t, 128U);
        dc->prealloc_lengths[i] = 1U + bounded_choice(t, 4U);
        dc->prealloc_payload_sizes[i] = draw_payload_size(t);
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
    theft_hash_sink(&h, (const uint8_t *)&dc->extent_count, sizeof(dc->extent_count));
    theft_hash_sink(&h, (const uint8_t *)&dc->prealloc_count, sizeof(dc->prealloc_count));
    theft_hash_sink(&h, (const uint8_t *)&dc->mode_selector, sizeof(dc->mode_selector));
    theft_hash_sink(&h, (const uint8_t *)dc->start_pages, sizeof(dc->start_pages));
    theft_hash_sink(&h, (const uint8_t *)dc->lengths, sizeof(dc->lengths));
    theft_hash_sink(&h, (const uint8_t *)dc->extent_payload_sizes, sizeof(dc->extent_payload_sizes));
    theft_hash_sink(&h, (const uint8_t *)dc->prealloc_starts, sizeof(dc->prealloc_starts));
    theft_hash_sink(&h, (const uint8_t *)dc->prealloc_lengths, sizeof(dc->prealloc_lengths));
    theft_hash_sink(&h, (const uint8_t *)dc->prealloc_payload_sizes, sizeof(dc->prealloc_payload_sizes));
    return theft_hash_done(&h);
}

static void dispose_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct dispose_case *dc = instance;
    fprintf(f, "{extent_count=%u, prealloc_count=%u, mode_selector=%u}",
            dc->extent_count, dc->prealloc_count, dc->mode_selector);
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

static int append_extent(struct inode *node, unsigned index, unsigned start_page,
                         unsigned length, size_t payload_size)
{
    Extent *ext = calloc(1U, sizeof(*ext));
    if (ext == NULL) {
        return 0;
    }
    unsigned char *data = malloc(payload_size);
    if (data == NULL) {
        free(ext);
        return 0;
    }
    memset(data, (int)(0xa0U + index), payload_size);

    ext->start_page = start_page;
    ext->length = length;
    ext->data = data;
    ext->next = node->extents;
    node->extents = ext;
    return 1;
}

static int append_prealloc(struct inode *node, unsigned index, unsigned lstart,
                           unsigned length, size_t payload_size)
{
    Prealloc *pre = calloc(1U, sizeof(*pre));
    if (pre == NULL) {
        return 0;
    }
    unsigned char *data = malloc(payload_size);
    unsigned char *array_data = malloc(payload_size);
    if (data == NULL || array_data == NULL) {
        free(data);
        free(array_data);
        free(pre);
        return 0;
    }
    memset(data, (int)(0xb0U + index), payload_size);
    memset(array_data, (int)(0xc0U + index), payload_size);

    pre->pa_lstart = lstart;
    pre->pa_data = data;
    pre->pa_len = length;
    pre->pa_free = 0U;
    pre->next = node->preallocs;
    node->preallocs = pre;
    node->prealloc_data[index] = array_data;
    return 1;
}

static void capture_expectations(struct inode *node)
{
    reset_tracker();
    expected_inode = node;
    expected_dir = node->dir;
    expected_mutex = node->impl;

    unsigned i = 0U;
    for (Extent *cur = node->extents; cur != NULL && i < MAX_TEST_EXTENTS; cur = cur->next) {
        expected_extent_nodes[i] = cur;
        expected_extent_data[i] = cur->data;
        i++;
    }
    expected_extent_count = i;

    i = 0U;
    for (Prealloc *cur = node->preallocs; cur != NULL && i < MAX_TEST_PREALLOCS; cur = cur->next) {
        expected_prealloc_nodes[i] = cur;
        expected_prealloc_data[i] = cur->pa_data;
        i++;
    }
    i = 0U;
    for (unsigned j = 0U; j < node->prealloc_count && j < MAX_TEST_PREALLOCS; j++) {
        expected_prealloc_array_data[i++] = node->prealloc_data[j];
    }
    expected_prealloc_array_count = i;
    tracker_enabled = 1;
}

static void cleanup_unfreed_payloads(void)
{
    tracker_enabled = 0;
    for (unsigned i = 0U; i < expected_extent_count; i++) {
        if (expected_extent_nodes[i] != NULL && observed_extent_node_frees[i] == 0U) {
            free((void *)expected_extent_nodes[i]);
        }
        if (expected_extent_data[i] != NULL && observed_extent_data_frees[i] == 0U) {
            free((void *)expected_extent_data[i]);
        }
    }
    for (unsigned i = 0U; i < expected_prealloc_count; i++) {
        if (expected_prealloc_nodes[i] != NULL && observed_prealloc_node_frees[i] == 0U) {
            free((void *)expected_prealloc_nodes[i]);
        }
        if (expected_prealloc_data[i] != NULL && observed_prealloc_data_frees[i] == 0U) {
            free((void *)expected_prealloc_data[i]);
        }
    }
    for (unsigned i = 0U; i < expected_prealloc_array_count; i++) {
        if (expected_prealloc_array_data[i] != NULL && observed_prealloc_array_data_frees[i] == 0U) {
            free((void *)expected_prealloc_array_data[i]);
        }
    }
    if (expected_dir != NULL && observed_dir_frees == 0U) {
        free((void *)expected_dir);
    }
}

static int mutex_and_inode_released_once(void)
{
    return observed_mutex_destroys == 1U && observed_inode_frees == 1U;
}

static int all_extent_nodes_released_once(void)
{
    for (unsigned i = 0U; i < expected_extent_count; i++) {
        if (observed_extent_node_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static int all_extent_payloads_released_once(void)
{
    for (unsigned i = 0U; i < expected_extent_count; i++) {
        if (observed_extent_data_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static int all_prealloc_nodes_released_once(void)
{
    for (unsigned i = 0U; i < expected_prealloc_count; i++) {
        if (observed_prealloc_node_frees[i] != 1U) {
            return 0;
        }
    }
    return 1;
}

static int all_prealloc_payloads_released_once(void)
{
    for (unsigned i = 0U; i < expected_prealloc_count; i++) {
        if (observed_prealloc_data_frees[i] != 1U) {
            return 0;
        }
    }
    for (unsigned i = 0U; i < expected_prealloc_array_count; i++) {
        if (observed_prealloc_array_data_frees[i] != 1U) {
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

    capture_expectations(node);
    dispose_inode(node);

    int ok = mutex_and_inode_released_once() &&
             observed_dir_frees == 1U &&
             expected_extent_count == 0U &&
             expected_prealloc_count == 0U &&
             observed_unknown_frees == 0U;
    tracker_enabled = 0;
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_empty_non_dir_releases_inode_and_mutex_only(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 1U, 2U);
    if (node == NULL || node->impl == NULL || node->extents != NULL || node->preallocs != NULL) {
        return THEFT_TRIAL_ERROR;
    }

    capture_expectations(node);
    dispose_inode(node);

    int ok = mutex_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             expected_extent_count == 0U &&
             expected_prealloc_count == 0U &&
             observed_unknown_frees == 0U;
    tracker_enabled = 0;
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_extent_inode_releases_nodes_and_payloads(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    if (dc->extent_count == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 3U, 4U);
    if (node == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    for (unsigned i = 0U; i < dc->extent_count; i++) {
        if (!append_extent(node, i, dc->start_pages[i], dc->lengths[i], dc->extent_payload_sizes[i])) {
            return THEFT_TRIAL_ERROR;
        }
    }
    if (node->prealloc_count != 0U || node->preallocs != NULL) {
        return THEFT_TRIAL_ERROR;
    }

    capture_expectations(node);
    dispose_inode(node);

    int ok = mutex_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             all_extent_nodes_released_once() &&
             all_extent_payloads_released_once() &&
             expected_prealloc_count == 0U &&
             observed_unknown_frees == 0U;
    if (!ok) {
        fprintf(stderr,
                "dispose_inode extent cleanup mismatch: extents=%u mutex=%u inode=%u first_data_free=%u unknown=%u\n",
                expected_extent_count, observed_mutex_destroys, observed_inode_frees,
                expected_extent_count > 0U ? observed_extent_data_frees[0] : 0U,
                observed_unknown_frees);
    }
    cleanup_unfreed_payloads();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_prealloc_inode_releases_nodes_and_payloads(struct theft *t, void *arg1)
{
    (void)t;
    const struct dispose_case *dc = arg1;
    if (dc->prealloc_count == 0U) {
        return THEFT_TRIAL_SKIP;
    }

    struct inode *node = malloc_inode(select_non_dir_mode(dc->mode_selector), 5U, 6U);
    if (node == NULL || node->impl == NULL) {
        return THEFT_TRIAL_ERROR;
    }

    for (unsigned i = 0U; i < dc->prealloc_count; i++) {
        if (!append_prealloc(node, i, dc->prealloc_starts[i], dc->prealloc_lengths[i],
                             dc->prealloc_payload_sizes[i])) {
            return THEFT_TRIAL_ERROR;
        }
    }
    node->prealloc_count = dc->prealloc_count;
    if (node->extents != NULL) {
        return THEFT_TRIAL_ERROR;
    }

    capture_expectations(node);
    dispose_inode(node);

    int ok = mutex_and_inode_released_once() &&
             observed_dir_frees == 0U &&
             expected_extent_count == 0U &&
             all_prealloc_nodes_released_once() &&
             all_prealloc_payloads_released_once() &&
             observed_unknown_frees == 0U;
    if (!ok) {
        fprintf(stderr,
                "dispose_inode prealloc cleanup mismatch: preallocs=%u array_data=%u mutex=%u inode=%u first_node_data_free=%u first_array_data_free=%u unknown=%u\n",
                expected_prealloc_count, expected_prealloc_array_count,
                observed_mutex_destroys, observed_inode_frees,
                expected_prealloc_count > 0U ? observed_prealloc_data_frees[0] : 0U,
                expected_prealloc_array_count > 0U ? observed_prealloc_array_data_frees[0] : 0U,
                observed_unknown_frees);
    }
    cleanup_unfreed_payloads();
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                       \
    do {                                                                      \
        struct theft_run_config cfg = {                                       \
            .name = name_,                                                    \
            .prop1 = prop_,                                                   \
            .type_info = { &dispose_case_info },                              \
            .trials = trials_,                                                \
            .seed = theft_seed_of_time(),                                     \
        };                                                                    \
        enum theft_run_res res = theft_run(&cfg);                             \
        printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_); \
        if (res != THEFT_RUN_PASS) {                                          \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    int failures = 0;
    printf("dispose_inode rbtree baseline property-based tests:\n");
    RUN_PROP("directory_releases_owned_fields", prop_directory_releases_owned_fields, 250);
    RUN_PROP("empty_non_dir_releases_inode_and_mutex_only", prop_empty_non_dir_releases_inode_and_mutex_only, 250);
    RUN_PROP("extent_inode_releases_nodes_and_payloads", prop_extent_inode_releases_nodes_and_payloads, 120);
    RUN_PROP("prealloc_inode_releases_nodes_and_payloads", prop_prealloc_inode_releases_nodes_and_payloads, 120);
    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
