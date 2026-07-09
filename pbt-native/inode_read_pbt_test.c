/**
 * PBT test for inode_read (extent/optimization variant).
 *
 * Properties tested:
 * 1. len_zero_returns_null_buf: When len==0 and offset < node->size,
 *    ret->buf must be NULL and ret->num must be 0 (spec Case 1).
 * 2. read_within_bounds: When offset < size and len > 0,
 *    ret->num == min(len, size - offset) and ret->buf is non-NULL.
 * 3. offset_beyond_size_returns_empty: When offset >= size,
 *    ret->num == 0 and ret->buf == NULL.
 * 4. read_matches_written_data: Write data, then read it back;
 *    the buffer contents must match.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "theft.h"

/* ------------------------------------------------------------------ */
/* Minimal types matching extent/optimization                          */
/* ------------------------------------------------------------------ */

#define PG_SIZE 4096
#define INDEXTB_NUM 8192
#define MAX_FILE_SIZE ((unsigned)(INDEXTB_NUM * PG_SIZE))
#define DIRTB_NUM 512

typedef struct Extent {
    unsigned start_page;
    unsigned length;
    unsigned char *data;
    struct Extent *next;
} Extent;

typedef struct inode {
    int mutex;
    void *impl;
    void *hd;
    unsigned maj;
    unsigned min;
    unsigned int mode;
    unsigned int size;
    struct dirtb *dir;
    Extent *extents;
} inode;

typedef struct read_ret {
    char *buf;
    unsigned num;
} read_ret;

/* ------------------------------------------------------------------ */
/* Stubs                                                               */
/* ------------------------------------------------------------------ */

struct read_ret* malloc_readret(void) {
    return (struct read_ret*)calloc(1, sizeof(struct read_ret));
}

char* malloc_buffer(unsigned len) {
    return (char*)malloc(len == 0 ? 1 : len);
}

unsigned int hash_func(char* name) { (void)name; return 0; }

/* Simple extent storage for a test inode */
static Extent* find_extent_impl(struct inode *node, unsigned page) {
    Extent *curr = node->extents;
    while (curr) {
        if (page >= curr->start_page && page < curr->start_page + curr->length)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

static void allocate_pages(struct inode *node, unsigned start_page, unsigned end_page) {
    for (unsigned p = start_page; p <= end_page && p < INDEXTB_NUM; p++) {
        if (!find_extent_impl(node, p)) {
            Extent *ext = calloc(1, sizeof(Extent));
            ext->start_page = p;
            ext->length = 1;
            ext->data = calloc(1, PG_SIZE);
            ext->next = node->extents;
            node->extents = ext;
        }
    }
}

void file_allocate(struct inode *node, unsigned offset, unsigned len) {
    if (len == 0) return;
    unsigned sp = offset / PG_SIZE;
    unsigned ep = (offset + len - 1) / PG_SIZE;
    allocate_pages(node, sp, ep);
}

void file_read(struct inode *node, unsigned offset, unsigned len, char *data) {
    unsigned cur = offset, remaining = len;
    char *dest = data;
    while (remaining > 0) {
        unsigned page = cur / PG_SIZE;
        unsigned po = cur % PG_SIZE;
        unsigned cl = PG_SIZE - po;
        if (cl > remaining) cl = remaining;
        Extent *ext = find_extent_impl(node, page);
        if (ext) {
            memcpy(dest, ext->data + (page - ext->start_page) * PG_SIZE + po, cl);
        } else {
            memset(dest, 0, cl);
        }
        dest += cl; cur += cl; remaining -= cl;
    }
}

void file_write(struct inode *node, unsigned offset, unsigned len, const char *data) {
    file_allocate(node, offset, len);
    unsigned cur = offset, remaining = len;
    const char *src = data;
    while (remaining > 0) {
        unsigned page = cur / PG_SIZE;
        unsigned po = cur % PG_SIZE;
        unsigned cl = PG_SIZE - po;
        if (cl > remaining) cl = remaining;
        Extent *ext = find_extent_impl(node, page);
        if (ext) {
            unsigned eo = (page - ext->start_page) * PG_SIZE + po;
            if (src) { memcpy(ext->data + eo, src, cl); src += cl; }
            else { memset(ext->data + eo, 0, cl); }
        }
        cur += cl; remaining -= cl;
    }
}

void clear_file(struct inode *node, unsigned start, unsigned len) {
    file_write(node, start, len, NULL);
}

/* ------------------------------------------------------------------ */
/* The inode_read under test (copy from extent/optimization)           */
/* ------------------------------------------------------------------ */

struct read_ret* inode_read(struct inode* node, unsigned len, unsigned offset) {
    if (node == NULL) {
        return NULL;
    }
    unsigned file_size = node->size;
    if (offset >= file_size) {
        struct read_ret* ret = malloc_readret();
        if (ret == NULL) return NULL;
        ret->num = 0;
        ret->buf = NULL;
        return ret;
    }
    unsigned actual_len = len;
    if (offset + len > file_size) {
        actual_len = file_size - offset;
    }
    char* buf = malloc_buffer(actual_len);
    if (buf == NULL) return NULL;
    file_read(node, offset, actual_len, buf);
    struct read_ret* ret = malloc_readret();
    if (ret == NULL) { free(buf); return NULL; }
    ret->buf = buf;
    ret->num = actual_len;
    return ret;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void free_inode_extents(struct inode *node) {
    Extent *e = node->extents;
    while (e) {
        Extent *next = e->next;
        free(e->data);
        free(e);
        e = next;
    }
    node->extents = NULL;
}

static unsigned my_min(unsigned a, unsigned b) { return a < b ? a : b; }

/* ------------------------------------------------------------------ */
/* Property 1: len_zero_returns_null_buf                               */
/* ------------------------------------------------------------------ */

struct read_args {
    unsigned size;    /* inode size (capped at 64K for speed) */
    unsigned offset;  /* read offset */
    unsigned len;     /* read length */
};

static enum theft_trial_res prop_len_zero_null_buf(struct theft *t, void *arg1) {
    (void)t;
    struct read_args *args = (struct read_args *)arg1;

    /* Constraint: we need offset < size and len == 0 */
    unsigned size = (args->size % 65536) + 1;  /* [1, 65536] */
    unsigned offset = args->offset % size;     /* [0, size-1] */

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    /* Allocate backing pages so file_read doesn't crash */
    file_allocate(&node, 0, size);

    struct read_ret *ret = inode_read(&node, 0, offset);  /* len=0 */

    if (ret == NULL) {
        free_inode_extents(&node);
        return THEFT_TRIAL_SKIP;
    }

    int pass = (ret->num == 0 && ret->buf == NULL);

    if (!pass) {
        fprintf(stderr, "  len_zero_null_buf FAIL: size=%u offset=%u → num=%u buf=%p\n",
                size, offset, ret->num, (void*)ret->buf);
    }

    free(ret->buf);
    free(ret);
    free_inode_extents(&node);
    return pass ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* ------------------------------------------------------------------ */
/* Property 2: read_within_bounds                                      */
/* ------------------------------------------------------------------ */

static enum theft_trial_res prop_read_within_bounds(struct theft *t, void *arg1) {
    (void)t;
    struct read_args *args = (struct read_args *)arg1;

    unsigned size = (args->size % 65536) + 1;
    unsigned offset = args->offset % size;
    unsigned len = (args->len % 65536) + 1;  /* must be > 0 */

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    file_allocate(&node, 0, size);

    struct read_ret *ret = inode_read(&node, len, offset);

    if (ret == NULL) {
        free_inode_extents(&node);
        return THEFT_TRIAL_SKIP;
    }

    unsigned expected_num = my_min(len, size - offset);
    int pass = (ret->num == expected_num && ret->buf != NULL);

    if (!pass) {
        fprintf(stderr, "  read_within_bounds FAIL: size=%u offset=%u len=%u → num=%u (expect %u) buf=%p\n",
                size, offset, len, ret->num, expected_num, (void*)ret->buf);
    }

    free(ret->buf);
    free(ret);
    free_inode_extents(&node);
    return pass ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* ------------------------------------------------------------------ */
/* Property 3: offset_beyond_size_returns_empty                        */
/* ------------------------------------------------------------------ */

static enum theft_trial_res prop_offset_beyond_size(struct theft *t, void *arg1) {
    (void)t;
    struct read_args *args = (struct read_args *)arg1;

    unsigned size = (args->size % 65536) + 1;
    unsigned offset = size + (args->offset % 1000);  /* >= size */
    unsigned len = (args->len % 1000) + 1;

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;

    struct read_ret *ret = inode_read(&node, len, offset);

    if (ret == NULL) {
        return THEFT_TRIAL_SKIP;
    }

    int pass = (ret->num == 0 && ret->buf == NULL);

    if (!pass) {
        fprintf(stderr, "  offset_beyond_size FAIL: size=%u offset=%u len=%u → num=%u buf=%p\n",
                size, offset, len, ret->num, (void*)ret->buf);
    }

    free(ret->buf);
    free(ret);
    return pass ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* ------------------------------------------------------------------ */
/* Property 4: read_matches_written_data                               */
/* ------------------------------------------------------------------ */

static enum theft_trial_res prop_read_matches_write(struct theft *t, void *arg1) {
    (void)t;
    struct read_args *args = (struct read_args *)arg1;

    unsigned size = (args->size % 8192) + 1;  /* smaller for write perf */
    unsigned write_len = (args->len % size) + 1;
    unsigned offset = args->offset % (size - write_len + 1);

    struct inode node;
    memset(&node, 0, sizeof(node));
    node.size = size;
    file_allocate(&node, 0, size);

    /* Write known pattern */
    char *pattern = malloc(write_len);
    for (unsigned i = 0; i < write_len; i++) pattern[i] = (char)(i ^ 0xAB);
    file_write(&node, offset, write_len, pattern);

    /* Read it back via inode_read */
    struct read_ret *ret = inode_read(&node, write_len, offset);

    if (ret == NULL) {
        free(pattern);
        free_inode_extents(&node);
        return THEFT_TRIAL_SKIP;
    }

    int pass = (ret->num == write_len && ret->buf != NULL &&
                memcmp(ret->buf, pattern, write_len) == 0);

    if (!pass) {
        fprintf(stderr, "  read_matches_write FAIL: size=%u offset=%u len=%u → num=%u\n",
                size, offset, write_len, ret->num);
    }

    free(pattern);
    free(ret->buf);
    free(ret);
    free_inode_extents(&node);
    return pass ? THEFT_TRIAL_PASS : THEFT_TRIAL_FAIL;
}

/* ------------------------------------------------------------------ */
/* Theft alloc/free for read_args                                      */
/* ------------------------------------------------------------------ */

static enum theft_alloc_res args_alloc(struct theft *t, void *env, void **instance) {
    (void)env;
    struct read_args *a = malloc(sizeof(*a));
    if (!a) return THEFT_ALLOC_ERROR;
    a->size = theft_random_bits(t, 32);
    a->offset = theft_random_bits(t, 32);
    a->len = theft_random_bits(t, 32);
    *instance = a;
    return THEFT_ALLOC_OK;
}

static void args_free(void *instance, void *env) {
    (void)env;
    free(instance);
}

static void args_print(FILE *f, const void *instance, void *env) {
    (void)env;
    const struct read_args *a = instance;
    fprintf(f, "{size=%u, offset=%u, len=%u}", a->size, a->offset, a->len);
}

static struct theft_type_info args_info = {
    .alloc = args_alloc,
    .free = args_free,
    .print = args_print,
};

/* ------------------------------------------------------------------ */
/* Main: run all properties                                            */
/* ------------------------------------------------------------------ */

static int run_property(const char *name,
                        enum theft_trial_res (*prop)(struct theft *, void *)) {
    struct theft_run_config config = {
        .name = name,
        .prop1 = prop,
        .type_info = { &args_info },
        .trials = 200,
        .seed = theft_seed_of_time(),
    };

    enum theft_run_res res = theft_run(&config);

    int pass = (res == THEFT_RUN_PASS);
    printf("  [%s] %s\n", pass ? "PASS" : "FAIL", name);
    return pass ? 0 : 1;
}

int main(void) {
    int failures = 0;

    printf("inode_read PBT (extent/optimization pattern)\n");
    printf("=============================================\n");

    failures += run_property("len_zero_returns_null_buf", prop_len_zero_null_buf);
    failures += run_property("read_within_bounds", prop_read_within_bounds);
    failures += run_property("offset_beyond_size_returns_empty", prop_offset_beyond_size);
    failures += run_property("read_matches_written_data", prop_read_matches_write);

    printf("\n%d failure(s)\n", failures);
    return failures > 0 ? 1 : 0;
}
