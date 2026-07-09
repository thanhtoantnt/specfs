#ifndef INLINE_DATA_LOC_COMMON_H
#define INLINE_DATA_LOC_COMMON_H

#include <stdint.h>

#define PG_SIZE 64U
#define INDEXTB_NUM 8U
#define INLINE_DATA_SIZE (PG_SIZE - 16U)
#define MAX_FILE_SIZE (INLINE_DATA_SIZE + (INDEXTB_NUM * PG_SIZE))

struct mcs_mutex;
struct mcs_node;
struct entry;

typedef struct indextb {
    unsigned char *index[INDEXTB_NUM];
} indextb;

typedef struct dirtb {
    struct entry *tb[1];
} dirtb;

typedef struct inode {
    int mutex;
    struct mcs_mutex *impl;
    struct mcs_node *hd;
    unsigned maj;
    unsigned min;
    unsigned int mode;
    unsigned int size;
    struct dirtb *dir;
    struct indextb *file;
    char inline_data[INLINE_DATA_SIZE];
} inode;

#endif
