#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_ALLOC_BYTES (3U * PG_SIZE + 17U)
#define TEST_PAGE_WINDOW 64U
#define MAX_RANGE_PAGES 6U

struct allocate_case {
    unsigned start_page;
    unsigned page_off;
    unsigned len;
    unsigned char fill;
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned draw_start_page(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0:
        return 0U;
    case 1:
        return INDEXTB_NUM - 1U;
    case 2:
        return INDEXTB_NUM - 2U;
    default:
        return bounded_choice(t, TEST_PAGE_WINDOW);
    }
}

static unsigned draw_page_offset(struct theft *t)
{
    switch (theft_random_choice(t, 6U)) {
    case 0:
        return 0U;
    case 1:
        return 1U;
    case 2:
        return PG_SIZE - 1U;
    case 3:
        return PG_SIZE - 16U;
    case 4:
        return PG_SIZE / 2U;
    default:
        return bounded_choice(t, PG_SIZE);
    }
}

static enum theft_alloc_res allocate_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct allocate_case *ac = malloc(sizeof(*ac));
    if (ac == NULL) return THEFT_ALLOC_ERROR;

    ac->start_page = draw_start_page(t);
    ac->page_off = draw_page_offset(t);

    uint64_t offset = (uint64_t)ac->start_page * PG_SIZE + ac->page_off;
    uint64_t remaining = (uint64_t)MAX_FILE_SIZE - offset;
    unsigned max_len = remaining < MAX_ALLOC_BYTES ? (unsigned)remaining : MAX_ALLOC_BYTES;
    if (max_len == 0U) max_len = 1U;
    ac->len = 1U + bounded_choice(t, max_len);
    ac->fill = (unsigned char)(1U + bounded_choice(t, 255U));

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
    fprintf(f, "{start_page=%u, page_off=%u, len=%u, fill=%u}",
            ac->start_page, ac->page_off, ac->len, ac->fill);
}

static struct theft_type_info allocate_case_info = {
    .alloc = allocate_case_alloc_cb,
    .free = allocate_case_free_cb,
    .hash = allocate_case_hash_cb,
    .print = allocate_case_print_cb,
};

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

static struct indextb make_table(void)
{
    struct indextb tb;
    memset(&tb, 0, sizeof(tb));
    return tb;
}

static void destroy_table(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static unsigned case_offset(const struct allocate_case *ac)
{
    return ac->start_page * PG_SIZE + ac->page_off;
}

static unsigned end_page_for_range(unsigned offset, unsigned len)
{
    return (offset + len - 1U) / PG_SIZE;
}

static unsigned range_page_count(unsigned offset, unsigned len)
{
    return end_page_for_range(offset, len) - (offset / PG_SIZE) + 1U;
}

static unsigned allocated_page_count(const struct indextb *tb)
{
    unsigned count = 0U;
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
        if (tb->index[page] != NULL) count++;
    }
    return count;
}

static int page_is_zeroed(const unsigned char *page)
{
    for (unsigned i = 0; i < PG_SIZE; i++) {
        if (page[i] != 0U) return 0;
    }
    return 1;
}

static void fill_page(unsigned char *page, unsigned page_index, unsigned char fill)
{
    for (unsigned i = 0; i < PG_SIZE; i++) {
        page[i] = (unsigned char)(fill + (unsigned char)(page_index * 17U) + (unsigned char)(i * 31U));
    }
}

static int allocate_pattern_page(struct indextb *tb, unsigned page, unsigned char fill)
{
    tb->index[page] = malloc_page();
    if (tb->index[page] == NULL) return 0;
    fill_page(tb->index[page], page, fill);
    return 1;
}

static enum theft_trial_res prop_valid_range_allocates_all_pages_zeroed(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(ac);
    unsigned start_page = offset / PG_SIZE;
    unsigned end_page = end_page_for_range(offset, ac->len);

    file_allocate(&tb, offset, ac->len);

    int ok = 1;
    for (unsigned page = 0; page < INDEXTB_NUM; page++) {
        int in_range = page >= start_page && page <= end_page;
        if (in_range && tb.index[page] == NULL) {
            fprintf(stderr, "missing allocated page=%u range=[%u,%u] offset=%u len=%u\n",
                    page, start_page, end_page, offset, ac->len);
            ok = 0;
            break;
        }
        if (!in_range && tb.index[page] != NULL) {
            fprintf(stderr, "allocated page outside range page=%u range=[%u,%u]\n",
                    page, start_page, end_page);
            ok = 0;
            break;
        }
        if (in_range && !page_is_zeroed(tb.index[page])) {
            fprintf(stderr, "new page not zeroed page=%u offset=%u len=%u\n",
                    page, offset, ac->len);
            ok = 0;
            break;
        }
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_preserves_existing_pages(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(ac);
    unsigned start_page = offset / PG_SIZE;
    unsigned pages = range_page_count(offset, ac->len);
    if (pages > MAX_RANGE_PAGES) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char *before[MAX_RANGE_PAGES] = { 0 };
    unsigned char *snapshot = malloc((size_t)pages * PG_SIZE);
    if (snapshot == NULL) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    for (unsigned i = 0; i < pages; i++) {
        unsigned page = start_page + i;
        if (!allocate_pattern_page(&tb, page, ac->fill)) {
            free(snapshot);
            destroy_table(&tb);
            return THEFT_TRIAL_ERROR;
        }
        before[i] = tb.index[page];
        memcpy(snapshot + (size_t)i * PG_SIZE, tb.index[page], PG_SIZE);
    }

    file_allocate(&tb, offset, ac->len);

    int ok = 1;
    for (unsigned i = 0; i < pages; i++) {
        unsigned page = start_page + i;
        if (tb.index[page] != before[i]) {
            fprintf(stderr, "existing page pointer changed page=%u\n", page);
            ok = 0;
            break;
        }
        if (memcmp(tb.index[page], snapshot + (size_t)i * PG_SIZE, PG_SIZE) != 0) {
            fprintf(stderr, "existing page contents changed page=%u\n", page);
            ok = 0;
            break;
        }
    }

    free(snapshot);
    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_reallocate_is_idempotent(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(ac);
    unsigned start_page = offset / PG_SIZE;
    unsigned pages = range_page_count(offset, ac->len);
    if (pages > MAX_RANGE_PAGES) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    file_allocate(&tb, offset, ac->len);

    unsigned char *before[MAX_RANGE_PAGES] = { 0 };
    for (unsigned i = 0; i < pages; i++) {
        before[i] = tb.index[start_page + i];
        if (before[i] == NULL) {
            destroy_table(&tb);
            return THEFT_TRIAL_FAIL;
        }
    }
    unsigned count_before = allocated_page_count(&tb);

    file_allocate(&tb, offset, ac->len);

    int ok = allocated_page_count(&tb) == count_before;
    for (unsigned i = 0; ok && i < pages; i++) {
        unsigned page = start_page + i;
        if (tb.index[page] != before[i]) {
            fprintf(stderr, "reallocation changed pointer page=%u\n", page);
            ok = 0;
            break;
        }
        if (!page_is_zeroed(tb.index[page])) {
            fprintf(stderr, "reallocation changed zeroed contents page=%u\n", page);
            ok = 0;
            break;
        }
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_allocation_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct allocate_case *ac = arg1;
    struct indextb tb = make_table();
    unsigned offset = case_offset(ac);
    unsigned page = offset / PG_SIZE;

    if (!allocate_pattern_page(&tb, page, ac->fill)) {
        destroy_table(&tb);
        return THEFT_TRIAL_ERROR;
    }

    unsigned char before[PG_SIZE];
    unsigned char *ptr_before = tb.index[page];
    unsigned count_before = allocated_page_count(&tb);
    memcpy(before, tb.index[page], sizeof(before));

    file_allocate(&tb, offset, 0U);

    int ok = allocated_page_count(&tb) == count_before && tb.index[page] == ptr_before &&
             memcmp(tb.index[page], before, sizeof(before)) == 0;
    if (!ok) {
        fprintf(stderr, "zero-length allocation mutated table offset=%u page=%u\n", offset, page);
    }

    destroy_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                \
    do {                                                               \
        struct theft_run_config cfg = {                                \
            .name = name_,                                             \
            .prop1 = prop_,                                            \
            .type_info = { &allocate_case_info },                      \
            .trials = trials_,                                         \
            .seed = theft_seed_of_time(),                              \
        };                                                             \
        enum theft_run_res res = theft_run(&cfg);                      \
        printf("  [%s] %s\n",                                        \
               res == THEFT_RUN_PASS ? "PASS" : "FAIL", name_);     \
        if (res != THEFT_RUN_PASS) failures++;                         \
    } while (0)

int main(void)
{
    int failures = 0;

    printf("baseline extent file_allocate property-based tests:\n");
    RUN_PROP("valid_range_allocates_all_pages_zeroed", prop_valid_range_allocates_all_pages_zeroed, 300);
    RUN_PROP("preserves_existing_pages", prop_preserves_existing_pages, 300);
    RUN_PROP("reallocate_is_idempotent", prop_reallocate_is_idempotent, 300);
    RUN_PROP("zero_length_allocation_is_noop", prop_zero_length_allocation_is_noop, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
