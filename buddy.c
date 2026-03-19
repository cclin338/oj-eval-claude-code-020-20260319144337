#include "buddy.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)  // 4KB
#define MIN_RANK 1
#define MAX_PAGES 32768  // 128MB / 4KB

// Data structures
static void *base_addr = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Free lists
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static FreeBlock *free_lists[MAX_RANK + 1];  // Index 1..MAX_RANK

// Block status: for each possible block at each rank
// status > 0: allocated at that rank
// status < 0: free at that rank (absolute value is rank)
// status == 0: not a valid block (e.g., beyond memory)
// Index: rank 1..16, block index 0..(total_pages/2^{rank-1}-1)
static char *block_status[MAX_RANK + 1];

// Helper functions
static int get_block_size(int rank) {
    return PAGE_SIZE * (1 << (rank - 1));
}

static int get_blocks_per_page(int rank) {
    return 1 << (rank - 1);
}

static int is_valid_rank(int rank) {
    return rank >= MIN_RANK && rank <= MAX_RANK;
}

static void *get_buddy(void *addr, int rank) {
    uintptr_t block_size = get_block_size(rank);
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)base_addr;
    uintptr_t buddy_offset = offset ^ block_size;
    return (void *)((uintptr_t)base_addr + buddy_offset);
}

// Convert address to block index for given rank
static int addr_to_block_idx(void *addr, int rank) {
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)base_addr;
    uintptr_t block_size = get_block_size(rank);
    return offset / block_size;
}

// Convert block index to address
static void *block_idx_to_addr(int idx, int rank) {
    uintptr_t offset = idx * get_block_size(rank);
    return (void *)((uintptr_t)base_addr + offset);
}

// Set block status
static void set_block_status(void *addr, int rank, int allocated) {
    int idx = addr_to_block_idx(addr, rank);
    if (idx >= 0 && block_status[rank]) {
        block_status[rank][idx] = allocated ? rank : -rank;
    }
}

// Get block status
static char get_block_status(void *addr, int rank) {
    int idx = addr_to_block_idx(addr, rank);
    if (idx >= 0 && block_status[rank]) {
        return block_status[rank][idx];
    }
    return 0;
}

// Helper to remove block from free list
static void remove_from_free_list(void *addr, int rank);

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) {
        return -EINVAL;
    }

    base_addr = p;
    total_pages = pgcount;

    // Calculate maximum possible rank
    int pages = pgcount;
    max_rank = MIN_RANK;
    while ((1 << (max_rank - 1)) * 2 <= pages) {
        max_rank++;
    }
    if (max_rank > MAX_RANK) max_rank = MAX_RANK;

    // Initialize free lists
    for (int i = MIN_RANK; i <= MAX_RANK; i++) {
        free_lists[i] = NULL;
    }

    // Allocate block status arrays
    for (int rank = MIN_RANK; rank <= MAX_RANK; rank++) {
        int blocks = total_pages / get_blocks_per_page(rank);
        if (blocks > 0) {
            block_status[rank] = (char *)calloc(blocks, sizeof(char));
        } else {
            block_status[rank] = NULL;
        }
    }

    // Create initial free blocks of maximum rank
    int current_rank = max_rank;
    int block_pages = get_blocks_per_page(current_rank);
    int blocks = total_pages / block_pages;

    for (int i = 0; i < blocks; i++) {
        void *block_addr = block_idx_to_addr(i, current_rank);
        FreeBlock *block = (FreeBlock *)block_addr;
        block->next = free_lists[current_rank];
        free_lists[current_rank] = block;
        set_block_status(block_addr, current_rank, 0);  // Free
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) {
        return ERR_PTR(-EINVAL);
    }

    // Find a free block of appropriate size
    int current_rank = rank;
    while (current_rank <= max_rank && free_lists[current_rank] == NULL) {
        current_rank++;
    }

    if (current_rank > max_rank) {
        return ERR_PTR(-ENOSPC);
    }

    // Split blocks if necessary
    void *alloc_addr = NULL;
    while (current_rank > rank) {
        // Remove block from current rank free list
        FreeBlock *block = free_lists[current_rank];
        free_lists[current_rank] = block->next;
        void *block_addr = (void *)block;

        // Clear status for this block (it's being split)
        set_block_status(block_addr, current_rank, 0);

        // Split block into two buddies
        int buddy_rank = current_rank - 1;
        void *buddy1 = block_addr;
        void *buddy2 = get_buddy(buddy1, buddy_rank);

        // Add both buddies to lower rank free list
        FreeBlock *b1 = (FreeBlock *)buddy1;
        FreeBlock *b2 = (FreeBlock *)buddy2;
        b2->next = free_lists[buddy_rank];
        b1->next = b2;
        free_lists[buddy_rank] = b1;

        // Set status for both buddies as free
        set_block_status(buddy1, buddy_rank, 0);
        set_block_status(buddy2, buddy_rank, 0);

        current_rank--;
    }

    // Allocate block from current rank (which now equals rank)
    FreeBlock *block = free_lists[rank];
    free_lists[rank] = block->next;
    alloc_addr = (void *)block;

    // Mark as allocated
    set_block_status(alloc_addr, rank, 1);

    return alloc_addr;
}

int return_pages(void *p) {
    if (!p || !base_addr || p < base_addr ||
        (uintptr_t)p >= (uintptr_t)base_addr + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    uintptr_t offset = (uintptr_t)p - (uintptr_t)base_addr;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;
    }

    // Find the rank of this block
    int rank = 0;
    for (int r = MIN_RANK; r <= max_rank; r++) {
        char status = get_block_status(p, r);
        if (status == r) {  // Allocated at this rank
            rank = r;
            break;
        }
    }

    if (rank == 0) {
        return -EINVAL;  // Not allocated
    }

    // Mark as free
    set_block_status(p, rank, 0);

    // Add to free list
    FreeBlock *block = (FreeBlock *)p;
    block->next = free_lists[rank];
    free_lists[rank] = block;

    // Try to merge with buddy
    int current_rank = rank;
    void *current_block = p;

    while (current_rank < max_rank) {
        void *buddy = get_buddy(current_block, current_rank);

        // Check if buddy is free at same rank
        char buddy_status = get_block_status(buddy, current_rank);
        if (buddy_status != -current_rank) {  // Not free
            break;
        }

        // Check if buddy is in free list
        int buddy_in_list = 0;
        FreeBlock *curr = free_lists[current_rank];
        while (curr) {
            if (curr == buddy) {
                buddy_in_list = 1;
                break;
            }
            curr = curr->next;
        }

        if (!buddy_in_list) {
            break;
        }

        // Remove both blocks from free list
        remove_from_free_list(current_block, current_rank);
        remove_from_free_list(buddy, current_rank);

        // Clear their status
        set_block_status(current_block, current_rank, 0);
        set_block_status(buddy, current_rank, 0);

        // Merge into larger block
        int parent_rank = current_rank + 1;
        void *parent_block = current_block < buddy ? current_block : buddy;

        // Add merged block to parent free list
        FreeBlock *parent = (FreeBlock *)parent_block;
        parent->next = free_lists[parent_rank];
        free_lists[parent_rank] = parent;

        // Set status as free
        set_block_status(parent_block, parent_rank, 0);

        // Continue merging
        current_block = parent_block;
        current_rank = parent_rank;
    }

    return OK;
}


int query_ranks(void *p) {
    if (!p || !base_addr || p < base_addr ||
        (uintptr_t)p >= (uintptr_t)base_addr + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    uintptr_t offset = (uintptr_t)p - (uintptr_t)base_addr;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;
    }

    // Check all ranks from highest to lowest
    for (int rank = max_rank; rank >= MIN_RANK; rank--) {
        // Compute block start address for this rank
        uintptr_t block_size = get_block_size(rank);
        uintptr_t block_offset = offset & ~(block_size - 1);
        void *block_addr = (void *)((uintptr_t)base_addr + block_offset);

        char status = get_block_status(block_addr, rank);
        if (status != 0) {
            // Found a block (allocated or free) containing this address
            return status > 0 ? status : -status;
        }
    }

    // Should not happen
    return -EINVAL;
}

int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    int count = 0;
    FreeBlock *block = free_lists[rank];
    while (block) {
        count++;
        block = block->next;
    }

    return count;
}

// Helper to remove block from free list
static void remove_from_free_list(void *addr, int rank) {
    FreeBlock **prev = &free_lists[rank];
    FreeBlock *curr = free_lists[rank];
    while (curr) {
        if (curr == addr) {
            *prev = curr->next;
            return;
        }
        prev = &curr->next;
        curr = curr->next;
    }
}