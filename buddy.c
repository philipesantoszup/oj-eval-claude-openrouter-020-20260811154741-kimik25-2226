#include "buddy.h"
#define NULL ((void *)0)

#define MIN_RANK 1
#define MAX_RANK 16
#define PAGE_SIZE 4096
#define MAX_PAGES 32768

/* Global state */
static void *g_base = NULL;
static int g_total_pages = 0;

/* Free list heads for each rank */
struct Block {
    struct Block *next;
    struct Block *prev;
};

static struct Block *free_list[MAX_RANK + 1];

/* Track rank of allocated block starting at each page (0 = not allocated start) */
static unsigned char alloc_rank[MAX_PAGES];

/* Track if a page is the start of a FREE block and its rank */
static unsigned char free_block_rank[MAX_PAGES];

/* Helper: get page index from address */
static inline int addr_to_idx(void *p) {
    return ((unsigned long)p - (unsigned long)g_base) >> 12;
}

/* Helper: get address from page index */
static inline void *idx_to_addr(int idx) {
    return (void *)((unsigned long)g_base + ((unsigned long)idx << 12));
}

/* Helper: check if pointer is valid */
static inline int is_valid_addr(void *p) {
    unsigned long offset = (unsigned long)p - (unsigned long)g_base;
    if (offset >= ((unsigned long)g_total_pages << 12)) return 0;
    if (offset & (PAGE_SIZE - 1)) return 0;
    return 1;
}

/* Helper: check if rank is valid */
static inline int is_valid_rank(int rank) {
    return (rank >= MIN_RANK) & (rank <= MAX_RANK);
}

/* Initialize the buddy system */
int init_page(void *p, int pgcount) {
    int i;
    int max_rank = MIN_RANK;

    g_base = p;
    g_total_pages = pgcount;

    /* Clear free lists */
    for (i = MIN_RANK; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }

    /* Clear tracking arrays */
    for (i = 0; i < pgcount; i++) {
        alloc_rank[i] = 0;
        free_block_rank[i] = 0;
    }

    /* Calculate the rank of the entire pool */
    while ((1 << (max_rank - 1)) < pgcount && max_rank < MAX_RANK) {
        max_rank++;
    }

    /* Add the entire pool to the appropriate free list */
    if (max_rank <= MAX_RANK && pgcount > 0) {
        struct Block *block = (struct Block *)p;
        block->next = NULL;
        block->prev = NULL;
        free_list[max_rank] = block;
        free_block_rank[0] = (unsigned char)max_rank;
    }

    return OK;
}

/* Remove block from free list - O(1) */
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

/* Add block to front of free list - O(1) */
static inline void add_to_free_list(int rank, struct Block *block) {
    block->next = free_list[rank];
    block->prev = NULL;
    if (free_list[rank]) {
        free_list[rank]->prev = block;
    }
    free_list[rank] = block;
}

/* Allocate pages with specified rank */
void *alloc_pages(int rank) {
    int j;
    struct Block *block;
    void *addr;
    int idx;

    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }

    /* Find the smallest available block that fits */
    for (j = rank; j <= MAX_RANK; j++) {
        if (free_list[j] != NULL) {
            break;
        }
    }

    if (j > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    /* Remove block from free list */
    block = free_list[j];
    remove_from_free_list(j, block);
    addr = (void *)block;
    idx = addr_to_idx(addr);
    free_block_rank[idx] = 0;

    /* Split the block until we reach the desired rank */
    while (j > rank) {
        int buddy_idx;
        struct Block *buddy;
        int page_count;

        j--;
        page_count = 1 << (j - 1);
        buddy_idx = idx + page_count;
        buddy = (struct Block *)idx_to_addr(buddy_idx);

        add_to_free_list(j, buddy);
        free_block_rank[buddy_idx] = (unsigned char)j;
    }

    alloc_rank[idx] = (unsigned char)rank;

    return addr;
}

/* Return pages to the buddy system */
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

    /* Try to coalesce with buddies */
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

/* Query the rank of a page */
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

    /* Check if allocated */
    rank = (int)alloc_rank[idx];
    if (rank != 0) {
        return rank;
    }

    /* Check if start of free block */
    rank = (int)free_block_rank[idx];
    if (rank != 0) {
        return rank;
    }

    /* Find which free block contains this page */
    /* Search from largest rank down for efficiency */
    for (rank = MAX_RANK; rank >= MIN_RANK; rank--) {
        int page_count = 1 << (rank - 1);
        struct Block *curr = free_list[rank];
        while (curr != NULL) {
            int start_idx = addr_to_idx((void *)curr);
            if ((unsigned int)(idx - start_idx) < (unsigned int)page_count) {
                return rank;
            }
            curr = curr->next;
        }
    }

    return -EINVAL;
}

/* Query how many unallocated pages remain for the specified rank */
int query_page_counts(int rank) {
    int count = 0;
    struct Block *curr;

    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    curr = free_list[rank];
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
