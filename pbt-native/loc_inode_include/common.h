#ifndef LOC_INODE_PBT_COMMON_H
#define LOC_INODE_PBT_COMMON_H

#include <stdint.h>

#define PG_SIZE 64U
#define INDEXTB_NUM 8U
#define DIRTB_NUM 8U
#define MAX_FILE_SIZE ((unsigned)(INDEXTB_NUM * PG_SIZE))
#define PAGE_SIZE PG_SIZE

#define FILE_MODE 1
#define DIR_MODE 2
#define CHR_MODE 3
#define BLK_MODE 4
#define SOCK_MODE 5
#define FIFO_MODE 6

struct mcs_mutex;
struct mcs_node;
struct entry;

typedef struct indextb {
    unsigned char *index[INDEXTB_NUM];
} indextb;

typedef struct dirtb {
    struct entry *tb[DIRTB_NUM];
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
} inode;

typedef struct read_ret {
    char *buf;
    unsigned num;
} read_ret;

typedef struct getattr_ret {
    struct inode *inum;
    unsigned mode;
    unsigned size;
    unsigned maj;
    unsigned min;
} getattr_ret;

#endif
