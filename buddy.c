#include "buddy.h"
#include <stdint.h>
#include <string.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)  // 4KB
#define MIN_RANK 1
#define MAX_PAGES 32768  // 128MB / 4KB

// Data structures for buddy system
static void *base_addr = NULL;
static int total_pages = 0;
static int max_rank = 0;

// Free lists for each rank: array of linked list heads
// Each free block stores a pointer to next free block at same rank
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static FreeBlock *free_lists[MAX_RANK + 1];  // Index 1..MAX_RANK

// Allocation status: for each page-sized unit, track which rank it belongs to
// 0 = free, >0 = allocated rank, <0 = part of larger free block (-rank)
static char page_status[MAX_PAGES];
static char page_rank_map[MAX_PAGES];  // Use char to save space

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
    uintptr_t buddy_offset = offset ^ block_size;  // XOR with block size to get buddy
    return (void *)((uintptr_t)base_addr + buddy_offset);
}

static void mark_pages(void *block, int rank, int allocated) {
    int block_pages = get_blocks_per_page(rank);
    uintptr_t offset = (uintptr_t)block - (uintptr_t)base_addr;
    int page_idx = offset / PAGE_SIZE;

    for (int i = 0; i < block_pages; i++) {
        int idx = page_idx + i;
        if (idx < total_pages) {
            page_status[idx] = allocated ? rank : -rank;
            page_rank_map[idx] = rank;
        }
    }
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) {
        return -EINVAL;
    }

    base_addr = p;
    total_pages = pgcount;

    // Calculate maximum possible rank (largest power of 2 <= total_pages)
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

    // Mark all pages as free
    memset(page_status, 0, sizeof(page_status));
    memset(page_rank_map, 0, sizeof(page_rank_map));

    // Create initial free block of maximum rank
    int current_rank = max_rank;
    int block_pages = get_blocks_per_page(current_rank);
    int blocks = total_pages / block_pages;

    // Add each max-rank block to free list
    for (int i = 0; i < blocks; i++) {
        void *block_addr = (void *)((uintptr_t)base_addr + i * get_block_size(current_rank));
        FreeBlock *block = (FreeBlock *)block_addr;
        block->next = free_lists[current_rank];
        free_lists[current_rank] = block;

        // Mark pages in this block
        mark_pages(block_addr, current_rank, 0);
    }

    // Any remaining pages that don't form a full max-rank block?
    // They should be ignored (internal fragmentation)
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
        return ERR_PTR(-ENOSPC);  // No free blocks available
    }

    // Split blocks if necessary
    while (current_rank > rank) {
        // Remove block from current rank free list
        FreeBlock *block = free_lists[current_rank];
        free_lists[current_rank] = block->next;

        // Split block into two buddies
        int buddy_rank = current_rank - 1;
        void *buddy1 = (void *)block;
        void *buddy2 = get_buddy(buddy1, buddy_rank);

        // Add both buddies to lower rank free list
        FreeBlock *b1 = (FreeBlock *)buddy1;
        FreeBlock *b2 = (FreeBlock *)buddy2;
        b2->next = free_lists[buddy_rank];
        b1->next = b2;
        free_lists[buddy_rank] = b1;

        // Update page status for split blocks (now smaller free blocks)
        mark_pages(buddy1, buddy_rank, 0);
        mark_pages(buddy2, buddy_rank, 0);

        current_rank--;
    }

    // Allocate block from current rank (which now equals rank)
    FreeBlock *block = free_lists[rank];
    free_lists[rank] = block->next;

    // Mark pages as allocated
    mark_pages(block, rank, 1);

    return (void *)block;
}

int return_pages(void *p) {
    if (!p || !base_addr || p < base_addr ||
        (uintptr_t)p >= (uintptr_t)base_addr + total_pages * PAGE_SIZE) {
        return -EINVAL;
    }

    // Check if this address is actually allocated
    uintptr_t offset = (uintptr_t)p - (uintptr_t)base_addr;
    if (offset % PAGE_SIZE != 0) {
        return -EINVAL;  // Not page-aligned
    }

    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages || page_status[page_idx] <= 0) {
        return -EINVAL;  // Not allocated or already free
    }

    int rank = page_status[page_idx];
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    // Mark pages as free
    mark_pages(p, rank, 0);

    // Add block to free list
    FreeBlock *block = (FreeBlock *)p;
    block->next = free_lists[rank];
    free_lists[rank] = block;

    // Try to merge with buddy
    int current_rank = rank;
    void *current_block = p;

    while (current_rank < max_rank) {
        void *buddy = get_buddy(current_block, current_rank);

        // Check if buddy is free and in the same rank free list
        uintptr_t buddy_offset = (uintptr_t)buddy - (uintptr_t)base_addr;
        int buddy_page_idx = buddy_offset / PAGE_SIZE;

        // Buddy should be free (negative status with same absolute rank)
        if (buddy_page_idx < 0 || buddy_page_idx >= total_pages ||
            page_status[buddy_page_idx] != -current_rank) {
            break;  // Buddy not free, can't merge
        }

        // Check if buddy is in free list
        int buddy_found = 0;
        FreeBlock **prev = &free_lists[current_rank];
        FreeBlock *curr = free_lists[current_rank];
        while (curr) {
            if (curr == buddy) {
                buddy_found = 1;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        if (!buddy_found) {
            break;  // Buddy not in free list
        }

        // Remove both blocks from free list
        // Remove current_block
        prev = &free_lists[current_rank];
        curr = free_lists[current_rank];
        while (curr) {
            if (curr == current_block) {
                *prev = curr->next;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        // Remove buddy
        prev = &free_lists[current_rank];
        curr = free_lists[current_rank];
        while (curr) {
            if (curr == buddy) {
                *prev = curr->next;
                break;
            }
            prev = &curr->next;
            curr = curr->next;
        }

        // Merge into larger block
        int parent_rank = current_rank + 1;
        void *parent_block = current_block < buddy ? current_block : buddy;

        // Add merged block to parent free list
        FreeBlock *parent = (FreeBlock *)parent_block;
        parent->next = free_lists[parent_rank];
        free_lists[parent_rank] = parent;

        // Update page status for merged block
        mark_pages(parent_block, parent_rank, 0);

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

    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages) {
        return -EINVAL;
    }

    // Return absolute rank (allocated or maximum free rank)
    int status = page_status[page_idx];
    if (status > 0) {
        return status;  // Allocated rank
    } else if (status < 0) {
        return -status;  // Free block rank
    } else {
        // Should not happen
        return -EINVAL;
    }
}

int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) {
        return -EINVAL;
    }

    // Count free blocks of this rank
    int count = 0;
    FreeBlock *block = free_lists[rank];
    while (block) {
        count++;
        block = block->next;
    }

    return count;
}