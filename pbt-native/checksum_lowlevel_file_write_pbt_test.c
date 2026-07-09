/*
 * Property-based checksum-oracle tests for file_write() in
 * eval/extent/optimization/lowlevel_file.c
 *
 * Oracle: Algebraic/model checksum. For bounded file regions, the checksum of
 * bytes read from the real extent-backed file must equal the checksum of a
 * simple byte-array model after the same writes.
 * Stronger considered:
 *   - State Machine: rejected - file_write has no lifecycle API.
 *   - Differential: rejected - no independent production implementation in scope.
 */
#include <theft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "lowlevel_file.h"

#define MAX_CASE_PAGES 4U
#define MAX_TEST_PAGE 16U
#define MAX_WRITE_BYTES 2048U

struct checksum_case {
    unsigned start_page;
    unsigned page_count;
    unsigned off1;
    unsigned len1;
    unsigned off2;
    unsigned len2;
    unsigned clear_off;
    unsigned clear_len;
    unsigned char data1[MAX_WRITE_BYTES];
    unsigned char data2[MAX_WRITE_BYTES];
};

static unsigned bounded_choice(struct theft *t, unsigned bound)
{
    return (unsigned)theft_random_choice(t, (uint64_t)bound);
}

static unsigned choose_offset(struct theft *t, unsigned region_len)
{
    switch (theft_random_choice(t, 6U)) {
    case 0: return 0U;
    case 1: return region_len > 1U ? 1U : 0U;
    case 2: return PG_SIZE - 1U;
    case 3: return PG_SIZE / 2U;
    case 4: return region_len - 1U;
    default: return bounded_choice(t, region_len);
    }
}

static unsigned choose_len(struct theft *t, unsigned available)
{
    unsigned max_len = available < MAX_WRITE_BYTES ? available : MAX_WRITE_BYTES;
    switch (theft_random_choice(t, 6U)) {
    case 0: return 1U;
    case 1: return max_len;
    case 2: return max_len > 1U ? max_len - 1U : 1U;
    case 3: return PG_SIZE < max_len ? PG_SIZE : max_len;
    default: return 1U + bounded_choice(t, max_len);
    }
}

static enum theft_alloc_res checksum_case_alloc_cb(struct theft *t, void *env, void **instance)
{
    (void)env;
    struct checksum_case *cc = malloc(sizeof(*cc));
    if (cc == NULL) return THEFT_ALLOC_ERROR;

    memset(cc, 0, sizeof(*cc));
    cc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    cc->start_page = bounded_choice(t, MAX_TEST_PAGE);

    unsigned region_len = cc->page_count * PG_SIZE;
    cc->off1 = choose_offset(t, region_len);
    cc->len1 = choose_len(t, region_len - cc->off1);
    cc->off2 = choose_offset(t, region_len);
    cc->len2 = choose_len(t, region_len - cc->off2);
    cc->clear_off = choose_offset(t, region_len);
    cc->clear_len = choose_len(t, region_len - cc->clear_off);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        cc->data1[i] = (unsigned char)theft_random_choice(t, 256U);
        cc->data2[i] = (unsigned char)theft_random_choice(t, 256U);
    }

    *instance = cc;
    return THEFT_ALLOC_OK;
}

static void checksum_case_free_cb(void *instance, void *env)
{
    (void)env;
    free(instance);
}

static theft_hash checksum_case_hash_cb(const void *instance, void *env)
{
    (void)env;
    return theft_hash_onepass(instance, sizeof(struct checksum_case));
}

static void checksum_case_print_cb(FILE *f, const void *instance, void *env)
{
    (void)env;
    const struct checksum_case *cc = instance;
    fprintf(f,
            "{start_page=%u, page_count=%u, off1=%u, len1=%u, off2=%u, len2=%u, clear_off=%u, clear_len=%u}",
            cc->start_page, cc->page_count, cc->off1, cc->len1,
            cc->off2, cc->len2, cc->clear_off, cc->clear_len);
}

static struct theft_type_info checksum_case_info = {
    .alloc = checksum_case_alloc_cb,
    .free = checksum_case_free_cb,
    .hash = checksum_case_hash_cb,
    .print = checksum_case_print_cb,
};

static uint64_t checksum(const unsigned char *data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

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

static unsigned base_offset(const struct checksum_case *cc)
{
    return cc->start_page * PG_SIZE;
}

static unsigned region_bytes(const struct checksum_case *cc)
{
    return cc->page_count * PG_SIZE;
}

static int read_region_checksum(struct inode *node, unsigned offset, unsigned len, uint64_t *out_checksum)
{
    unsigned char *out = malloc(len);
    if (out == NULL) return 0;
    memset(out, 0xCC, len);
    file_read(node, offset, len, (char *)out);
    *out_checksum = checksum(out, len);
    free(out);
    return 1;
}

static enum theft_trial_res prop_single_write_region_checksum_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct checksum_case *cc = arg1;
    struct inode node = make_inode();
    unsigned base = base_offset(cc);
    unsigned region_len = region_bytes(cc);
    unsigned char *model = calloc(region_len, 1U);
    if (model == NULL) return THEFT_TRIAL_ERROR;

    memcpy(model + cc->off1, cc->data1, cc->len1);
    file_write(&node, base + cc->off1, cc->len1, (const char *)cc->data1);

    uint64_t actual = 0;
    int ok = read_region_checksum(&node, base, region_len, &actual);
    uint64_t expected = checksum(model, region_len);
    if (!ok || actual != expected) {
        fprintf(stderr, "single-write checksum mismatch: actual=%llu expected=%llu\n",
                (unsigned long long)actual, (unsigned long long)expected);
        ok = 0;
    }

    free(model);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_two_writes_region_checksum_matches_model(struct theft *t, void *arg1)
{
    (void)t;
    const struct checksum_case *cc = arg1;
    struct inode node = make_inode();
    unsigned base = base_offset(cc);
    unsigned region_len = region_bytes(cc);
    unsigned char *model = calloc(region_len, 1U);
    if (model == NULL) return THEFT_TRIAL_ERROR;

    memcpy(model + cc->off1, cc->data1, cc->len1);
    file_write(&node, base + cc->off1, cc->len1, (const char *)cc->data1);
    memcpy(model + cc->off2, cc->data2, cc->len2);
    file_write(&node, base + cc->off2, cc->len2, (const char *)cc->data2);

    uint64_t actual = 0;
    int ok = read_region_checksum(&node, base, region_len, &actual);
    uint64_t expected = checksum(model, region_len);
    if (!ok || actual != expected) {
        fprintf(stderr, "two-write checksum mismatch: actual=%llu expected=%llu\n",
                (unsigned long long)actual, (unsigned long long)expected);
        ok = 0;
    }

    free(model);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_null_write_zeroes_checksum_span(struct theft *t, void *arg1)
{
    (void)t;
    const struct checksum_case *cc = arg1;
    struct inode node = make_inode();
    unsigned base = base_offset(cc);
    unsigned region_len = region_bytes(cc);
    unsigned char *model = calloc(region_len, 1U);
    if (model == NULL) return THEFT_TRIAL_ERROR;

    memcpy(model + cc->off1, cc->data1, cc->len1);
    file_write(&node, base + cc->off1, cc->len1, (const char *)cc->data1);
    memset(model + cc->clear_off, 0, cc->clear_len);
    file_write(&node, base + cc->clear_off, cc->clear_len, NULL);

    uint64_t actual = 0;
    int ok = read_region_checksum(&node, base, region_len, &actual);
    uint64_t expected = checksum(model, region_len);
    if (!ok || actual != expected) {
        fprintf(stderr, "null-write checksum mismatch: actual=%llu expected=%llu\n",
                (unsigned long long)actual, (unsigned long long)expected);
        ok = 0;
    }

    free(model);
    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_write_preserves_checksum(struct theft *t, void *arg1)
{
    (void)t;
    const struct checksum_case *cc = arg1;
    struct inode node = make_inode();
    unsigned base = base_offset(cc);
    unsigned region_len = region_bytes(cc);

    file_write(&node, base + cc->off1, cc->len1, (const char *)cc->data1);

    uint64_t before = 0;
    uint64_t after = 0;
    int ok = read_region_checksum(&node, base, region_len, &before);
    file_write(&node, base + cc->off2, 0U, (const char *)cc->data2);
    ok = ok && read_region_checksum(&node, base, region_len, &after);
    if (!ok || before != after) {
        fprintf(stderr, "zero-length checksum changed: before=%llu after=%llu\n",
                (unsigned long long)before, (unsigned long long)after);
        ok = 0;
    }

    free_extents(&node);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

#define RUN_PROP(name_, prop_, trials_)                                  \
    do {                                                                  \
        struct theft_run_config cfg = {                                   \
            .name = name_,                                                \
            .prop1 = prop_,                                               \
            .type_info = { &checksum_case_info },                         \
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

    printf("checksum-oracle extent lowlevel file_write property-based tests:\n");
    RUN_PROP("single_write_region_checksum_matches_model", prop_single_write_region_checksum_matches_model, 300);
    RUN_PROP("two_writes_region_checksum_matches_model", prop_two_writes_region_checksum_matches_model, 300);
    RUN_PROP("null_write_zeroes_checksum_span", prop_null_write_zeroes_checksum_span, 200);
    RUN_PROP("zero_length_write_preserves_checksum", prop_zero_length_write_preserves_checksum, 200);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
