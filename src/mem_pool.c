#include "micro_sched/mem_pool.h"

#include <stdint.h>
#include <string.h>

/* Ensure blocks are large enough to hold an intrusive next-pointer. */
static size_t align_block_size(size_t bs)
{
    if (bs < sizeof(void *))
        bs = sizeof(void *);
    /* Align to pointer size */
    size_t align = sizeof(void *);
    return (bs + align - 1) & ~(align - 1);
}

void pool_init(MemPool *pool, void *buf, size_t block_size, size_t count)
{
    pool->buf        = buf;
    pool->block_size = align_block_size(block_size);
    pool->count      = count;
    pool->free_count = count;

    /* Build the free list by linking each block to the next. */
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < count; ++i) {
        void *next = (i + 1 < count) ? (void *)(p + pool->block_size) : NULL;
        /* Store next pointer at the start of the block */
        memcpy(p, &next, sizeof(void *));
        p += pool->block_size;
    }
    pool->free_head = buf;
}

void *pool_alloc(MemPool *pool)
{
    if (pool->free_head == NULL)
        return NULL;

    void *block = pool->free_head;
    /* Advance free_head to the next block (stored inside the block) */
    void *next;
    memcpy(&next, block, sizeof(void *));
    pool->free_head = next;
    pool->free_count--;
    return block;
}

void pool_free(MemPool *pool, void *ptr)
{
    /* Push ptr onto the front of the free list */
    void *old_head = pool->free_head;
    memcpy(ptr, &old_head, sizeof(void *));
    pool->free_head = ptr;
    pool->free_count++;
}
