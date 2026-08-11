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
static struct Block {
    struct Block *next;
    struct Block *prev;
} *free_list[MAX_RANK + 1];

/* Track rank of allocated block starting at each page (0 = not allocated start) */
static unsigned char alloc_rank[MAX_PAGES];

/* Track if a page is the start of a FREE block and its rank */
static unsigned char free_block_rank[MAX_PAGES];

/* Helper: get page index from address */
static inline int addr_to_idx(void *p) {
    return (int)((unsigned long)p - (unsigned long)g_base) / PAGE_SIZE;
}

/* Helper: get address from page index */
static inline void *idx_to_addr(int idx) {
    return (void *)((unsigned long)g_base + (unsigned long)idx * PAGE_SIZE);
}

/* Helper: check if pointer is valid (aligned and in range) */
static inline int is_valid_addr(void *p) {
    if (p < g_base) return 0;
    if (((unsigned long)p - (unsigned long)g_base) % PAGE_SIZE != 0) return 0;
    int idx = addr_to_idx(p);
    if (idx >= g_total_pages) return 0;
    return 1;
}

/* Helper: check if rank is valid */
static inline int is_valid_rank(int rank) {
    return rank >= MIN_RANK && rank <= MAX_RANK;
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
    for (i = 0; i < MAX_PAGES; i++) {
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
static struct Block *remove_from_free_list(int rank, struct Block *block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_list[rank] = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    block->next = NULL;
    block->prev = NULL;
    return block;
}

/* Add block to front of free list - O(1) */
static void add_to_free_list(int rank, struct Block *block) {
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
    free_block_rank[idx] = 0; /* No longer a free block start */

    /* Split the block until we reach the desired rank */
    while (j > rank) {
        int buddy_idx;
        struct Block *buddy;
        int page_count;

        j--;
        page_count = 1 << (j - 1);
        buddy_idx = idx + page_count;
        buddy = (struct Block *)idx_to_addr(buddy_idx);

        /* Add buddy to free list and mark it */
        add_to_free_list(j, buddy);
        free_block_rank[buddy_idx] = (unsigned char)j;
    }

    /* Record the allocation */
    alloc_rank[idx] = (unsigned char)rank;

    return addr;
}

/* Return pages to the buddy system */
int return_pages(void *p) {
    int idx;
    int rank;
    struct Block *block;
    int max_rank = MAX_RANK;

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

    /* Clear allocation tracking */
    alloc_rank[idx] = 0;

    /* Add block to free list */
    block = (struct Block *)p;
    add_to_free_list(rank, block);
    free_block_rank[idx] = (unsigned char)rank;

    /* Try to coalesce with buddies */
    while (rank < max_rank) {
        int buddy_idx;
        int page_count = 1 << (rank - 1);

        /* Calculate buddy index using XOR */
        buddy_idx = idx ^ page_count;

        /* Check if buddy is a free block of the same rank - O(1) lookup */
        if (free_block_rank[buddy_idx] != rank) {
            break;
        }

        /* Buddy found - remove it from its free list */
        remove_from_free_list(rank, (struct Block *)idx_to_addr(buddy_idx));
        free_block_rank[buddy_idx] = 0;

        /* Also remove current block from free list */
        remove_from_free_list(rank, block);
        free_block_rank[idx] = 0;

        /* Coalesce: new block starts at the lower address */
        if (buddy_idx < idx) {
            idx = buddy_idx;
            block = (struct Block *)idx_to_addr(idx);
        }

        /* Move to higher rank */
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

    /* Check if it's the start of a free block */
    rank = (int)free_block_rank[idx];
    if (rank != 0) {
        return rank;
    }

    /* Check if inside a free block - return the containing block's rank */
    for (rank = MAX_RANK; rank >= MIN_RANK; rank--) {
        int page_count = 1 << (rank - 1);
        /* Check all free blocks of this rank */
        struct Block *curr = free_list[rank];
        while (curr != NULL) {
            int start_idx = addr_to_idx((void *)curr);
            if (idx >= start_idx && idx < start_idx + page_count) {
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
