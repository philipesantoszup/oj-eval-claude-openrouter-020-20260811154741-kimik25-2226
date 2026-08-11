#include "buddy.h"
#include <string.h>
#include <stdint.h>
#define NULL ((void *)0)

#define MIN_RANK 1
#define MAX_RANK 16
#define PAGE_SHIFT 12

static void *g_base = NULL;
static int g_total_pages = 0;

struct Block { struct Block *next, *prev; };

static struct Block *free_list[MAX_RANK + 1];
static uint8_t alloc_rank[32768];
static uint8_t free_start_rank[32768];

static inline int addr_to_idx(void *p) {
    return ((unsigned long)p - (unsigned long)g_base) >> PAGE_SHIFT;
}

static inline void *idx_to_addr(int idx) {
    return (void *)((unsigned long)g_base + ((unsigned long)idx << PAGE_SHIFT));
}

static inline int is_valid_addr(void *p) {
    unsigned long o = (unsigned long)p - (unsigned long)g_base;
    return (o < ((unsigned long)g_total_pages << PAGE_SHIFT)) && !(o & 4095);
}

int init_page(void *p, int pgcount) {
    int max_rank;
    g_base = p; g_total_pages = pgcount;
    memset(free_list, 0, sizeof(free_list));
    if (pgcount <= 0) return OK;
    max_rank = 32 - __builtin_clz(pgcount - 1) + 1;
    if (max_rank > MAX_RANK) max_rank = MAX_RANK;
    memset(alloc_rank, 0, pgcount);
    memset(free_start_rank, 0, pgcount);
    struct Block *b = (struct Block *)p;
    b->next = b->prev = NULL; free_list[max_rank] = b;
    free_start_rank[0] = max_rank;
    return OK;
}

static inline void remove_blk(int r, struct Block *b) {
    if (b->prev) b->prev->next = b->next; else free_list[r] = b->next;
    if (b->next) b->next->prev = b->prev;
}

static inline void add_blk(int r, struct Block *b) {
    b->next = free_list[r]; b->prev = NULL;
    if (free_list[r]) free_list[r]->prev = b;
    free_list[r] = b;
}

void *alloc_pages(int rank) {
    int j, idx, buddy;
    struct Block *b, *bd;
    if (rank < MIN_RANK || rank > MAX_RANK) return ERR_PTR(-EINVAL);
    for (j = rank; j <= MAX_RANK && !free_list[j]; j++);
    if (j > MAX_RANK) return ERR_PTR(-ENOSPC);
    b = free_list[j]; remove_blk(j, b);
    idx = addr_to_idx((void *)b); free_start_rank[idx] = 0;
    while (j > rank) {
        j--; buddy = idx + (1 << (j - 1)); bd = (struct Block *)idx_to_addr(buddy);
        add_blk(j, bd); free_start_rank[buddy] = j;
    }
    alloc_rank[idx] = rank;
    return (void *)b;
}

int return_pages(void *p) {
    int idx, rank, buddy, sz;
    struct Block *b;
    if (!p || !is_valid_addr(p)) return -EINVAL;
    idx = addr_to_idx(p); rank = alloc_rank[idx];
    if (!rank) return -EINVAL;
    alloc_rank[idx] = 0; b = (struct Block *)p;
    add_blk(rank, b); free_start_rank[idx] = rank; sz = 1 << (rank - 1);
    while (rank < MAX_RANK) {
        buddy = idx ^ sz;
        if (free_start_rank[buddy] != rank) break;
        remove_blk(rank, (struct Block *)idx_to_addr(buddy));
        free_start_rank[buddy] = 0; remove_blk(rank, b); free_start_rank[idx] = 0;
        if (buddy < idx) { idx = buddy; b = (struct Block *)idx_to_addr(idx); }
        rank++; sz <<= 1;
        add_blk(rank, b); free_start_rank[idx] = rank;
    }
    return OK;
}

int query_ranks(void *p) {
    int idx, rank;
    struct Block *b;
    if (!p || !is_valid_addr(p)) return -EINVAL;
    idx = addr_to_idx(p);
    if ((rank = alloc_rank[idx])) return rank;
    if ((rank = free_start_rank[idx])) return rank;
    for (rank = MAX_RANK; rank >= MIN_RANK; rank--) {
        int pc = 1 << (rank - 1);
        for (b = free_list[rank]; b; b = b->next) {
            int si = (int)((((unsigned long)b - (unsigned long)g_base) >> PAGE_SHIFT));
            if ((unsigned int)(idx - si) < (unsigned int)pc) return rank;
        }
    }
    return -EINVAL;
}

int query_page_counts(int rank) {
    int c = 0;
    struct Block *b;
    if (rank < MIN_RANK || rank > MAX_RANK) return -EINVAL;
    for (b = free_list[rank]; b; b = b->next) c++;
    return c;
}
