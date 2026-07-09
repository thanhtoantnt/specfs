/*
 * Property-based tests for file_allocate() in
 * eval/delay_alloc/optimization/lowlevel_file.c
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "delalloc.h"
#include "lowlevel_file.h"

#define MAX_SEEDED_PAGES 8U
#define TEST_PAGE_WINDOW 64U

extern struct alloc_buffer alloc_buffer;
extern int data_write_count;
extern int data_read_count;

struct allocate_case {
    unsigned offset;
    unsigned len;
    unsigned seeded_count;
    unsigned seeded_pages[MAX_SEEDED_PAGES];
    unsigned char seed;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned edge_offset(struct theft *t)
{
    switch (theft_random_choice(t, 10)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return PAGE_SIZE - 1U;
    case 3:
        return PAGE_SIZE;
    case 4:
        return PAGE_SIZE + 1U;
    case 5:
        return MAX_FILE_SIZE - 1U;
    case 6:
        return MAX_FILE_SIZE;
    case 7:
        return UINT32_MAX;
    default:
        return bounded_choice(t, TEST_PAGE_WINDOW * PAGE_SIZE);
    }
}

static unsigned edge_len(struct theft *t)
{
    switch (theft_random_choice(t, 9)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return PAGE_SIZE - 1U;
    case 3:
        return PAGE_SIZE;
    case 4:
        return PAGE_SIZE + 1U;
    case 5:
        return 2U * PAGE_SIZE + 17U;
    case 6:
        return MAX_FILE_SIZE;
    case 7:
        return UINT32_MAX;
    default:
        return bounded_choice(t, 4U * PAGE_SIZE + 1U);
    }
}

static enum theft_alloc_res allocate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct allocate_case *ac = malloc(sizeof(*ac));
    if (ac == NULL) return THEFT_ALLOC_ERROR;

    ac->offset = edge_offset(t);
    ac->len = edge_len(t);
    ac->seeded_count = bounded_choice(t, MAX_SEEDED_PAGES + 1U);
    ac->seed = (unsigned char)theft_random_choice(t, 256U);

    for (unsigned i = 0; i < MAX_SEEDED_PAGES; i++) {
        ac->seeded_pages[i] = bounded_choice(t, TEST_PAGE_WINDOW);
    }

    *instance = ac;
    return THEFT_ALLOC_OK;
}

static void allocate_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash allocate_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct allocate_case));
}

static void allocate_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct allocate_case *ac = instance;
    fprintf(f, "{offset=%u, len=%u, seeded_count=%u, seed=%u}",
            ac->offset, ac->len, ac->seeded_count, ac->seed);
}

static struct theft_type_info allocate_case_info = {
    .alloc = allocate_case_alloc_cb,
    .free = allocate_case_free_cb,
    .hash = allocate_case_hash_cb,
    .print = allocate_case_print_cb,
};

void buffer_lock(struct alloc_buffer *buf)
{
    (void)buf;
}

void buffer_unlock(struct alloc_buffer *buf)
{
    (void)buf;
}

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PAGE_SIZE);
    if (page != NULL) memset(page, 0, PAGE_SIZE);
    return page;
}

static void reset_delalloc_state(void)
{
    memset(&alloc_buffer, 0, sizeof(alloc_buffer));
    data_write_count = 0;
    data_read_count = 0;
}

static void init_table(struct indextb *tb)
{
    reset_delalloc_state();
    memset(tb, 0, sizeof(*tb));
}

static void cleanup_table(struct indextb *tb)
{
    brels(tb);
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
    reset_delalloc_state();
}

static int seed_pages(struct indextb *tb, const struct allocate_case *ac)
{
    for (unsigned i = 0; i < ac->seeded_count; i++) {
        unsigned page = ac->seeded_pages[i];
        if (tb->index[page] != NULL) continue;
        tb->index[page] = malloc_page();
        if (tb->index[page] == NULL) return 0;
        for (unsigned j = 0; j < PAGE_SIZE; j++) {
            tb->index[page][j] = (unsigned char)(ac->seed + page + j * 17U);
        }
    }
    return 1;
}

static int snapshot_table(unsigned char **snapshot, struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) snapshot[i] = tb->index[i];
    return 1;
}

static int table_pointers_match(unsigned char **snapshot, struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        if (snapshot[i] != tb->index[i]) {
            fprintf(stderr, "page pointer changed at page=%u\n", i);
            return 0;
        }
    }
    return 1;
}

static int seeded_contents_match(struct indextb *tb, const struct allocate_case *ac)
{
    for (unsigned i = 0; i < ac->seeded_count; i++) {
        unsigned page = ac->seeded_pages[i];
        if (tb->index[page] == NULL) {
            fprintf(stderr, "seeded page disappeared page=%u\n", page);
            return 0;
        }
        for (unsigned j = 0; j < PAGE_SIZE; j++) {
            unsigned char expected = (unsigned char)(ac->seed + page + j * 17U);
            if (tb->index[page][j] != expected) {
                fprintf(stderr, "seeded byte changed page=%u off=%u got=%u expected=%u\n",
                        page, j, tb->index[page][j], expected);
                return 0;
            }
        }
    }
    return 1;
}

static int delalloc_state_is_idle(void)
{
    if (alloc_buffer.size != 0U || data_write_count != 0 || data_read_count != 0) {
        fprintf(stderr, "delalloc side effect size=%u writes=%d reads=%d\n",
                alloc_buffer.size, data_write_count, data_read_count);
        return 0;
    }
    return 1;
}

/* Oracle: delayed allocation defines file_allocate as a no-op, so for any
 * generated range it must not allocate previously missing table pages. */
static enum theft_trial_res prop_allocate_does_not_materialize_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb;
    unsigned char *before[INDEXTB_NUM];
    init_table(&tb);

    if (!seed_pages(&tb, ac)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    snapshot_table(before, &tb);

    file_allocate(&tb, ac->offset, ac->len);

    int ok = table_pointers_match(before, &tb);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: because allocation is delayed, file_allocate must preserve every
 * byte of already allocated pages, even when the requested range overlaps them. */
static enum theft_trial_res prop_allocate_preserves_existing_page_contents(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb;
    init_table(&tb);

    if (!seed_pages(&tb, ac)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    file_allocate(&tb, ac->offset, ac->len);

    int ok = seeded_contents_match(&tb, ac);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: file_allocate is a pure no-op for delayed allocation and must not
 * enqueue delayed writes, flush data, or count physical reads/writes. */
static enum theft_trial_res prop_allocate_has_no_delalloc_side_effects(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb;
    init_table(&tb);

    if (!seed_pages(&tb, ac)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    reset_delalloc_state();
    file_allocate(&tb, ac->offset, ac->len);

    int ok = delalloc_state_is_idle();
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* Oracle: delayed allocation is idempotent; repeating file_allocate with the
 * same range must leave the table in the same state as one no-op call. */
static enum theft_trial_res prop_allocate_is_idempotent_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb;
    unsigned char *before[INDEXTB_NUM];
    init_table(&tb);

    if (!seed_pages(&tb, ac)) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    snapshot_table(before, &tb);

    file_allocate(&tb, ac->offset, ac->len);
    file_allocate(&tb, ac->offset, ac->len);

    int ok = table_pointers_match(before, &tb) && seeded_contents_match(&tb, ac) && delalloc_state_is_idle();
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

typedef enum theft_trial_res (*property_cb)(struct theft *, void *);

static int run_prop(const char *name, property_cb prop, unsigned trials)
{
    struct theft_run_config cfg = {
        .name = name,
        .prop1 = prop,
        .type_info = { &allocate_case_info },
        .trials = trials,
        .seed = theft_seed_of_time(),
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  [%s] %s\n", res == THEFT_RUN_PASS ? "PASS" : "FAIL", name);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    printf("delay_alloc lowlevel file_allocate property-based tests:\n");
    failures += run_prop("allocate_does_not_materialize_pages", prop_allocate_does_not_materialize_pages, 300);
    failures += run_prop("allocate_preserves_existing_page_contents", prop_allocate_preserves_existing_page_contents, 300);
    failures += run_prop("allocate_has_no_delalloc_side_effects", prop_allocate_has_no_delalloc_side_effects, 300);
    failures += run_prop("allocate_is_idempotent_noop", prop_allocate_is_idempotent_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
