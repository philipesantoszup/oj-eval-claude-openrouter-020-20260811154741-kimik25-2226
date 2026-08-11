#include "buddy.h"
#define NULL ((void *)0)

#define MIN_RANK 1
#define MAX_RANK 16
#define PAGE_SHIFT 12
#define PAGE_SIZE (1 << PAGE_SHIFT)
#define MAX_PAGES 32768

static void *g_base = NULL;
static int g_total_pages = 0;

struct Block {
    struct Block *next;
    struct Block *prev;
};

static struct Block *free_list[MAX_RANK + 1];
static unsigned char alloc_rank[MAX_PAGES];
static unsigned char free_block_rank[MAX_PAGES];

static inline int addr_to_idx(void *p) {
    return ((unsigned long)p - (unsigned long)g_base) >> PAGE_SHIFT;
}

static inline void *idx_to_addr(int idx) {
    return (void *)((unsigned long)g_base + ((unsigned long)idx << PAGE_SHIFT));
}

static inline int is_valid_addr(void *p) {
    unsigned long offset = (unsigned long)p - (unsigned long)g_base;
    if (offset >= ((unsigned long)g_total_pages << PAGE_SHIFT)) return 0;
    if (offset & (PAGE_SIZE - 1)) return 0;
    return 1;
}

int init_page(void *p, int pgcount) {
    int i;
    int max_rank = MIN_RANK;

    g_base = p;
    g_total_pages = pgcount;

    for (i = MIN_RANK; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }

    for (i = 0; i < pgcount; i++) {
        alloc_rank[i] = 0;
        free_block_rank[i] = 0;
    }

    while ((1 << (max_rank - 1)) < pgcount && max_rank < MAX_RANK) {
        max_rank++;
    }

    if (max_rank <= MAX_RANK && pgcount > 0) {
        struct Block *block = (struct Block *)p;
        block->next = NULL;
        block->prev = NULL;
        free_list[max_rank] = block;
        free_block_rank[0] = (unsigned char)max_rank;
    }

    return OK;
}

static inline void remove_from_free_list(int rank, struct Block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list[rank] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
}

static inline void add_to_free_list(int rank, struct Block *block) {
    block->next = free_list[rank];
    block->prev = NULL;
    if (free_list[rank]) {
        free_list[rank]->prev = block;
    }
    free_list[rank] = block;
}

void *alloc_pages(int rank) {
    int j;
    struct Block *block;
    void *addr;
    int idx;

    if (rank < MIN_RANK || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }

    for (j = rank; j <= MAX_RANK; j++) {
        if (free_list[j] != NULL) {
            break;
        }
    }

    if (j > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    block = free_list[j];
    remove_from_free_list(j, block);
    addr = (void *)block;
    idx = addr_to_idx(addr);
    free_block_rank[idx] = 0;

    while (j > rank) {
        int buddy_idx;
        struct Block *buddy;

        j--;
        buddy_idx = idx + (1 << (j - 1));
        buddy = (struct Block *)idx_to_addr(buddy_idx);

        add_to_free_list(j, buddy);
        free_block_rank[buddy_idx] = (unsigned char)j;
    }

    alloc_rank[idx] = (unsigned char)rank;

    return addr;
}

int return_pages(void *p) {
    int idx;
    int rank;
    struct Block *block;

    if (p == NULL) {
        return -EINVAL;
    }

    if (!is_valid_addr(p)) {
        return -EINVAL;
    }

    idx = addr_to_idx(p);
    rank = (int)alloc_rank[idx];

    if (rank == 0) {
        return -EINVAL;
    }

    alloc_rank[idx] = 0;

    block = (struct Block *)p;
    add_to_free_list(rank, block);
    free_block_rank[idx] = (unsigned char)rank;

    while (rank < MAX_RANK) {
        int buddy_idx;
        int page_count = 1 << (rank - 1);

        buddy_idx = idx ^ page_count;

        if (free_block_rank[buddy_idx] != rank) {
            break;
        }

        remove_from_free_list(rank, (struct Block *)idx_to_addr(buddy_idx));
        free_block_rank[buddy_idx] = 0;
        remove_from_free_list(rank, block);
        free_block_rank[idx] = 0;

        if (buddy_idx < idx) {
            idx = buddy_idx;
            block = (struct Block *)idx_to_addr(idx);
        }

        rank++;
        add_to_free_list(rank, block);
        free_block_rank[idx] = (unsigned char)rank;
    }

    return OK;
}

/* Check if idx is inside a free block of given rank */
static inline int is_inside_free_block(int idx, int rank) {
    int page_count = 1 << (rank - 1);
    struct Block *curr = free_list[rank];
    while (curr != NULL) {
        int start_idx = addr_to_idx((void *)curr);
        if ((unsigned int)(idx - start_idx) < (unsigned int)page_count) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

int query_ranks(void *p) {
    int idx;
    int rank;

    if (p == NULL) {
        return -EINVAL;
    }

    if (!is_valid_addr(p)) {
        return -EINVAL;
    }

    idx = addr_to_idx(p);

    rank = (int)alloc_rank[idx];
    if (rank != 0) {
        return rank;
    }

    rank = (int)free_block_rank[idx];
    if (rank != 0) {
        return rank;
    }

    for (rank = MAX_RANK; rank >= MIN_RANK; rank--) {
        if (is_inside_free_block(idx, rank)) {
            return rank;
        }
    }

    return -EINVAL;
}

int query_page_counts(int rank) {
    int count = 0;
    struct Block *curr;

    if (rank < MIN_RANK || rank > MAX_RANK) {
        return -EINVAL;
    }

    curr = free_list[rank];
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
