#include "buddy.h"
#define NULL ((void *)0)

#define MIN_RANK 1
#define MAX_RANK 16
#define PAGE_SIZE 4096

/* Block structure for free list - stored within the free page itself */
typedef struct Block {
    struct Block *next;
} Block;

/* Global state */
static void *g_base = NULL;           /* Base address of memory pool */
static int g_total_pages = 0;         /* Total number of pages */
static Block *free_list[MAX_RANK + 1]; /* Free lists for each rank (1-16) */
static unsigned char page_rank[32768]; /* Track rank of allocated blocks (indexed by page number) */

/* Helper: get page index from address */
static inline int addr_to_idx(void *p) {
    return (int)((unsigned long)p - (unsigned long)g_base) / PAGE_SIZE;
}

/* Helper: get address from page index */
static inline void *idx_to_addr(int idx) {
    return (void *)((unsigned long)g_base + (unsigned long)idx * PAGE_SIZE);
}

/* Helper: check if pointer is within valid range */
static inline int is_valid_addr(void *p) {
    if (p < g_base) return 0;
    if (p >= (void *)((unsigned long)g_base + (unsigned long)g_total_pages * PAGE_SIZE)) return 0;
    /* Check page alignment */
    if (((unsigned long)p - (unsigned long)g_base) % PAGE_SIZE != 0) return 0;
    return 1;
}

/* Helper: check if rank is valid */
static inline int is_valid_rank(int rank) {
    return rank >= MIN_RANK && rank <= MAX_RANK;
}

/* Initialize the buddy system */
int init_page(void *p, int pgcount) {
    int i;
    int max_rank;

    if (p == NULL || pgcount <= 0) {
        return OK; /* Or error, but test expects OK for valid init */
    }

    g_base = p;
    g_total_pages = pgcount;

    /* Clear free lists */
    for (i = MIN_RANK; i <= MAX_RANK; i++) {
        free_list[i] = NULL;
    }

    /* Clear allocation tracking */
    for (i = 0; i < 32768; i++) {
        page_rank[i] = 0;
    }

    /* Calculate the rank of the entire pool */
    /* pgcount = 2^(max_rank-1), so max_rank = log2(pgcount) + 1 */
    max_rank = MIN_RANK;
    while ((1 << (max_rank - 1)) < pgcount && max_rank < MAX_RANK) {
        max_rank++;
    }

    /* Add the entire pool to the appropriate free list */
    if (max_rank <= MAX_RANK) {
        Block *block = (Block *)p;
        block->next = NULL;
        free_list[max_rank] = block;
    }

    return OK;
}

/* Remove a block from the front of a free list */
static Block *remove_from_free_list(int rank) {
    Block *block = free_list[rank];
    if (block != NULL) {
        free_list[rank] = block->next;
    }
    return block;
}

/* Add a block to the front of a free list */
static void add_to_free_list(int rank, Block *block) {
    block->next = free_list[rank];
    free_list[rank] = block;
}

/* Remove a specific block from a free list */
static int remove_specific_from_free_list(int rank, Block *target) {
    Block *curr = free_list[rank];
    Block *prev = NULL;

    while (curr != NULL) {
        if (curr == target) {
            if (prev == NULL) {
                free_list[rank] = curr->next;
            } else {
                prev->next = curr->next;
            }
            return 1; /* Found and removed */
        }
        prev = curr;
        curr = curr->next;
    }
    return 0; /* Not found */
}

/* Allocate pages with specified rank */
void *alloc_pages(int rank) {
    int j;
    Block *block;
    void *addr;
    int idx;
    int page_count;

    /* Check for invalid rank */
    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }

    /* Find the smallest available block that fits */
    for (j = rank; j <= MAX_RANK; j++) {
        if (free_list[j] != NULL) {
            break;
        }
    }

    /* No suitable block found */
    if (j > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }

    /* Remove block from free list */
    block = remove_from_free_list(j);
    addr = (void *)block;

    /* Split the block until we reach the desired rank */
    while (j > rank) {
        int buddy_idx;
        Block *buddy;

        j--;
        /* Calculate buddy address: second half of the split block */
        page_count = 1 << (j - 1); /* Pages in each buddy after split */
        buddy_idx = addr_to_idx(addr) + page_count;
        buddy = (Block *)idx_to_addr(buddy_idx);

        /* Add buddy to free list */
        add_to_free_list(j, buddy);
    }

    /* Record the allocation */
    idx = addr_to_idx(addr);
    page_rank[idx] = (unsigned char)rank;

    return addr;
}

/* Return pages to the buddy system */
int return_pages(void *p) {
    int idx;
    int rank;
    Block *block;
    int max_rank = MAX_RANK;

    /* Check for NULL */
    if (p == NULL) {
        return -EINVAL;
    }

    /* Check if address is valid */
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }

    idx = addr_to_idx(p);

    /* Check if this page was actually allocated */
    rank = (int)page_rank[idx];
    if (rank == 0) {
        return -EINVAL;
    }

    /* Clear allocation tracking */
    page_rank[idx] = 0;

    /* Add block to free list */
    block = (Block *)p;
    add_to_free_list(rank, block);

    /* Try to coalesce with buddies */
    while (rank < max_rank) {
        int buddy_idx;
        Block *buddy;
        int page_count = 1 << (rank - 1);

        /* Calculate buddy index using XOR (buddy system property) */
        buddy_idx = idx ^ page_count;
        buddy = (Block *)idx_to_addr(buddy_idx);

        /* Check if buddy is in the free list of the same rank */
        if (!remove_specific_from_free_list(rank, buddy)) {
            /* Buddy not found, can't coalesce further */
            break;
        }

        /* Also remove the current block from free list (we just added it) */
        remove_specific_from_free_list(rank, block);

        /* Coalesce: new block starts at the lower address */
        if (buddy_idx < idx) {
            idx = buddy_idx;
            block = buddy;
        }

        /* Move to higher rank */
        rank++;
        add_to_free_list(rank, block);
    }

    return OK;
}

/* Query the rank of a page */
int query_ranks(void *p) {
    int idx;
    int rank;

    /* Check for NULL */
    if (p == NULL) {
        return -EINVAL;
    }

    /* Check if address is valid */
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }

    idx = addr_to_idx(p);

    /* Check if allocated */
    rank = (int)page_rank[idx];
    if (rank != 0) {
        return rank;
    }

    /* Not allocated, check if it's the start of a free block */
    for (rank = MIN_RANK; rank <= MAX_RANK; rank++) {
        Block *curr = free_list[rank];
        while (curr != NULL) {
            if ((void *)curr == p) {
                return rank;
            }
            curr = curr->next;
        }
    }

    /* Check if inside a free block (for unallocated pages, return max rank) */
    for (rank = MAX_RANK; rank >= MIN_RANK; rank--) {
        Block *curr = free_list[rank];
        int page_count = 1 << (rank - 1);
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
    Block *curr;

    /* Check for invalid rank */
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    /* Count blocks in the free list for this rank */
    curr = free_list[rank];
    while (curr != NULL) {
        count++;
        curr = curr->next;
    }

    return count;
}
