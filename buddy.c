#include "buddy.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define NULL ((void *)0)
#define MAX_RANK 16
#define PAGE_SIZE (4 * 1024)
#define MIN_RANK 1
#define MAX_PAGES 32768

static void *base_addr = NULL;
static int total_pages = 0;
static int max_rank = 0;

typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static FreeBlock *free_lists[MAX_RANK + 1];
static char page_rank[MAX_PAGES];  // 0=free but unknown, >0=allocated rank, <0=free rank

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

static void mark_block(void *addr, int rank, int allocated) {
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)base_addr;
    int page_idx = offset / PAGE_SIZE;
    int block_pages = get_blocks_per_page(rank);

    // Mark first page only
    page_rank[page_idx] = allocated ? rank : -rank;

    // Mark other pages as 0 (will be found via first page)
    for (int i = 1; i < block_pages; i++) {
        if (page_idx + i < total_pages) {
            page_rank[page_idx + i] = 0;
        }
    }
}

static int find_block_rank(void *addr) {
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)base_addr;
    int page_idx = offset / PAGE_SIZE;

    if (page_idx < 0 || page_idx >= total_pages) return 0;

    // Check if this page has rank info (first page of some block)
    if (page_rank[page_idx] != 0) {
        return page_rank[page_idx];
    }

    // Not first page, find containing block by checking alignment
    for (int rank = max_rank; rank >= MIN_RANK; rank--) {
        int block_pages = get_blocks_per_page(rank);
        int first_page = page_idx & ~(block_pages - 1);
        if (first_page >= 0 && first_page < total_pages && page_rank[first_page] != 0) {
            // Check if this block contains our page
            int r = page_rank[first_page];
            if (r < 0) r = -r;
            if (r == rank) {
                return page_rank[first_page];
            }
        }
    }

    return 0;
}

int init_page(void *p, int pgcount) {
    if (!p || pgcount <= 0 || pgcount > MAX_PAGES) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Calculate max rank
    int pages = pgcount;
    max_rank = MIN_RANK;
    while ((1 << (max_rank - 1)) * 2 <= pages) max_rank++;
    if (max_rank > MAX_RANK) max_rank = MAX_RANK;

    // Init free lists
    for (int i = MIN_RANK; i <= MAX_RANK; i++) free_lists[i] = NULL;

    // Clear page ranks
    memset(page_rank, 0, sizeof(page_rank));

    // Create initial free blocks
    int block_pages = get_blocks_per_page(max_rank);
    int blocks = total_pages / block_pages;

    for (int i = 0; i < blocks; i++) {
        void *block_addr = (void *)((uintptr_t)base_addr + i * get_block_size(max_rank));
        FreeBlock *block = (FreeBlock *)block_addr;
        block->next = free_lists[max_rank];
        free_lists[max_rank] = block;
        mark_block(block_addr, max_rank, 0);
    }

    return OK;
}

void *alloc_pages(int rank) {
    if (!is_valid_rank(rank)) return ERR_PTR(-EINVAL);

    int current_rank = rank;
    while (current_rank <= max_rank && free_lists[current_rank] == NULL) current_rank++;
    if (current_rank > max_rank) return ERR_PTR(-ENOSPC);

    void *alloc_addr = NULL;
    while (current_rank > rank) {
        FreeBlock *block = free_lists[current_rank];
        free_lists[current_rank] = block->next;
        void *block_addr = (void *)block;

        // Clear parent marking
        uintptr_t offset = (uintptr_t)block_addr - (uintptr_t)base_addr;
        int page_idx = offset / PAGE_SIZE;
        page_rank[page_idx] = 0;

        // Split
        int buddy_rank = current_rank - 1;
        void *buddy1 = block_addr;
        void *buddy2 = get_buddy(buddy1, buddy_rank);

        // Add buddies to free list
        FreeBlock *b1 = (FreeBlock *)buddy1;
        FreeBlock *b2 = (FreeBlock *)buddy2;
        b2->next = free_lists[buddy_rank];
        b1->next = b2;
        free_lists[buddy_rank] = b1;

        // Mark buddies as free
        mark_block(buddy1, buddy_rank, 0);
        mark_block(buddy2, buddy_rank, 0);

        current_rank--;
    }

    FreeBlock *block = free_lists[rank];
    free_lists[rank] = block->next;
    alloc_addr = (void *)block;
    mark_block(alloc_addr, rank, 1);

    return alloc_addr;
}

int return_pages(void *p) {
    if (!p || !base_addr || p < base_addr ||
        (uintptr_t)p >= (uintptr_t)base_addr + total_pages * PAGE_SIZE)
        return -EINVAL;

    uintptr_t offset = (uintptr_t)p - (uintptr_t)base_addr;
    if (offset % PAGE_SIZE != 0) return -EINVAL;

    // Find rank by checking page_rank
    int page_idx = offset / PAGE_SIZE;
    if (page_idx < 0 || page_idx >= total_pages || page_rank[page_idx] <= 0)
        return -EINVAL;

    int rank = page_rank[page_idx];
    if (!is_valid_rank(rank)) return -EINVAL;

    // Mark as free
    mark_block(p, rank, 0);

    // Add to free list
    FreeBlock *block = (FreeBlock *)p;
    block->next = free_lists[rank];
    free_lists[rank] = block;

    // Merge with buddy
    int current_rank = rank;
    void *current_block = p;

    while (current_rank < max_rank) {
        void *buddy = get_buddy(current_block, current_rank);

        // Check if buddy is free
        int buddy_rank = find_block_rank(buddy);
        if (buddy_rank != -current_rank) break;  // Buddy not free at same rank

        // Remove both from free lists
        FreeBlock **prev = &free_lists[current_rank];
        FreeBlock *curr = free_lists[current_rank];
        int removed = 0;

        while (curr) {
            if (curr == current_block || curr == buddy) {
                *prev = curr->next;
                removed++;
                if (removed == 2) break;
                curr = *prev;
            } else {
                prev = &curr->next;
                curr = curr->next;
            }
        }

        if (removed != 2) break;

        // Clear markings
        uintptr_t off1 = (uintptr_t)current_block - (uintptr_t)base_addr;
        uintptr_t off2 = (uintptr_t)buddy - (uintptr_t)base_addr;
        page_rank[off1 / PAGE_SIZE] = 0;
        page_rank[off2 / PAGE_SIZE] = 0;

        // Merge
        int parent_rank = current_rank + 1;
        void *parent_block = current_block < buddy ? current_block : buddy;

        FreeBlock *parent = (FreeBlock *)parent_block;
        parent->next = free_lists[parent_rank];
        free_lists[parent_rank] = parent;

        mark_block(parent_block, parent_rank, 0);

        current_block = parent_block;
        current_rank = parent_rank;
    }

    return OK;
}

int query_ranks(void *p) {
    if (!p || !base_addr || p < base_addr ||
        (uintptr_t)p >= (uintptr_t)base_addr + total_pages * PAGE_SIZE)
        return -EINVAL;

    uintptr_t offset = (uintptr_t)p - (uintptr_t)base_addr;
    if (offset % PAGE_SIZE != 0) return -EINVAL;

    int rank = find_block_rank(p);
    if (rank == 0) return -EINVAL;

    return rank > 0 ? rank : -rank;
}

int query_page_counts(int rank) {
    if (!is_valid_rank(rank)) return -EINVAL;

    int count = 0;
    FreeBlock *block = free_lists[rank];
    while (block) {
        count++;
        block = block->next;
    }

    return count;
}