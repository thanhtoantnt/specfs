/*
 * Property-based tests for file_write() in
 * eval/loc/gen/checksum/lowlevel_file.c
 *
 * Standalone harness: supplies the minimal inode/index-table model and a
 * deterministic checksum oracle so checksum preconditions and postconditions
 * are observable without the rest of specfs.
 */
#include <theft.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG_SIZE 64U
#define INDEXTB_NUM 16U
#define MAX_WRITE_BYTES 192U
#define MAX_CASE_PAGES 4U

struct inode {
    uint32_t checksum;
};

struct indextb {
    struct inode *parent_inode;
    char *index[INDEXTB_NUM];
};

unsigned char *malloc_page(void);
bool checksum_validate(const struct inode *node);
void checksum_update(struct inode *node);

/* Include the target after defining its dependencies. */
#include "../eval/loc/gen/checksum/lowlevel_file.c"

static uint32_t checksum_model(const struct inode *node)
{
    uintptr_t v = (uintptr_t)node;
    return (uint32_t)(0x9E3779B9U ^ (v >> 4) ^ (v >> 12));
}

bool checksum_validate(const struct inode *node)
{
    return node != NULL && node->checksum == checksum_model(node);
}

void checksum_update(struct inode *node)
{
    assert(node != NULL);
    node->checksum = checksum_model(node);
}

unsigned char *malloc_page(void)
{
    unsigned char *page = malloc(PG_SIZE);
    if (page != NULL) memset(page, 0, PG_SIZE);
    return page;
}

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

    wc->page_count = 1U + bounded_choice(t, MAX_CASE_PAGES);
    wc->start_page = bounded_choice(t, INDEXTB_NUM - wc->page_count);

    switch (theft_random_choice(t, 5)) {
    case 0: wc->page_off = 0U; break;
    case 1: wc->page_off = 1U; break;
    case 2: wc->page_off = PG_SIZE - 1U; break;
    case 3: wc->page_off = PG_SIZE / 2U; break;
    default: wc->page_off = bounded_choice(t, PG_SIZE); break;
    }

    unsigned available = wc->page_count * PG_SIZE - wc->page_off;
    unsigned max_len = available < MAX_WRITE_BYTES ? available : MAX_WRITE_BYTES;
    wc->len = 1U + bounded_choice(t, max_len);

    for (unsigned i = 0; i < MAX_WRITE_BYTES; i++) {
        wc->data[i] = (unsigned char)theft_random_choice(t, 256);
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

static void init_table(struct indextb *tb, struct inode *node)
{
    memset(tb, 0, sizeof(*tb));
    memset(node, 0, sizeof(*node));
    tb->parent_inode = node;
    checksum_update(node);
}

static void cleanup_table(struct indextb *tb)
{
    for (unsigned i = 0; i < INDEXTB_NUM; i++) {
        free(tb->index[i]);
        tb->index[i] = NULL;
    }
}

static unsigned case_offset(const struct write_case *wc)
{
    return wc->start_page * PG_SIZE + wc->page_off;
}

static unsigned case_region_bytes(const struct write_case *wc)
{
    return wc->page_count * PG_SIZE;
}

static enum theft_trial_res prop_roundtrip_and_checksum_valid(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_table(&tb, &node);

    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(wc->len);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, wc->len);
    file_read(&tb, offset, wc->len, (char *)out);

    int ok = memcmp(out, wc->data, wc->len) == 0 && checksum_validate(&node);
    if (!ok) fprintf(stderr, "roundtrip/checksum mismatch: offset=%u len=%u\n", offset, wc->len);

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_sparse_write_zero_fills_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_table(&tb, &node);

    unsigned region = case_region_bytes(wc);
    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    memset(out, 0xCC, region);
    file_read(&tb, wc->start_page * PG_SIZE, region, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned abs_off = wc->start_page * PG_SIZE + i;
        unsigned char expected = 0U;
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "sparse mismatch at byte %u: got=%u expected=%u\n", i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_existing_pages_preserve_unwritten_bytes(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_table(&tb, &node);

    for (unsigned p = 0; p < wc->page_count; p++) {
        tb.index[wc->start_page + p] = (char *)malloc_page();
        if (tb.index[wc->start_page + p] == NULL) {
            cleanup_table(&tb);
            return THEFT_TRIAL_ERROR;
        }
        memset(tb.index[wc->start_page + p], (int)(0x40U + p), PG_SIZE);
    }
    checksum_update(&node);

    unsigned region = case_region_bytes(wc);
    unsigned offset = case_offset(wc);
    file_write(&tb, offset, wc->len, (const char *)wc->data);

    unsigned char *out = malloc(region);
    if (out == NULL) {
        cleanup_table(&tb);
        return THEFT_TRIAL_ERROR;
    }
    file_read(&tb, wc->start_page * PG_SIZE, region, (char *)out);

    int ok = 1;
    for (unsigned i = 0; i < region; i++) {
        unsigned page_delta = i / PG_SIZE;
        unsigned abs_off = wc->start_page * PG_SIZE + i;
        unsigned char expected = (unsigned char)(0x40U + page_delta);
        if (abs_off >= offset && abs_off < offset + wc->len) {
            expected = wc->data[abs_off - offset];
        }
        if (out[i] != expected) {
            fprintf(stderr, "preservation mismatch at byte %u: got=%u expected=%u\n", i, out[i], expected);
            ok = 0;
            break;
        }
    }

    free(out);
    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_zero_length_write_is_noop(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    struct inode node;
    struct indextb tb;
    init_table(&tb, &node);
    uint32_t before_checksum = node.checksum;

    file_write(&tb, case_offset(wc), 0U, (const char *)wc->data);

    int ok = node.checksum == before_checksum && checksum_validate(&node);
    for (unsigned i = 0; ok && i < INDEXTB_NUM; i++) {
        ok = tb.index[i] == NULL;
    }
    if (!ok) fprintf(stderr, "zero-length write changed checksum or allocated a page\n");

    cleanup_table(&tb);
    return ok ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

static enum theft_trial_res prop_bad_checksum_rejects_write(struct theft *t, void *arg1)
{
    (void)t;
    const struct write_case *wc = arg1;
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return THEFT_TRIAL_ERROR;
    }

    if (pid == 0) {
        struct inode node;
        struct indextb tb;
        init_table(&tb, &node);
        node.checksum ^= 0xA5A5A5A5U;
        file_write(&tb, case_offset(wc), wc->len, (const char *)wc->data);
        cleanup_table(&tb);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return THEFT_TRIAL_ERROR;
    }

    if (WIFSIGNALED(status)) return THEFT_TRIAL_PASS;
    fprintf(stderr, "bad checksum write was not rejected: status=%d\n", status);
    return THEFT_TRIAL_FAIL;
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

    printf("checksum lowlevel file_write property-based tests:\n");
    RUN_PROP("roundtrip_and_checksum_valid", prop_roundtrip_and_checksum_valid, 300);
    RUN_PROP("sparse_write_zero_fills_unwritten_bytes", prop_sparse_write_zero_fills_unwritten_bytes, 300);
    RUN_PROP("existing_pages_preserve_unwritten_bytes", prop_existing_pages_preserve_unwritten_bytes, 300);
    RUN_PROP("zero_length_write_is_noop", prop_zero_length_write_is_noop, 100);
    RUN_PROP("bad_checksum_rejects_write", prop_bad_checksum_rejects_write, 100);

    printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

