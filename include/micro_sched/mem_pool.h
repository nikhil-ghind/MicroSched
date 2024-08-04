#ifndef MICRO_SCHED_MEM_POOL_H
#define MICRO_SCHED_MEM_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Fixed-block memory pool.
 *
 * The pool is backed by a caller-supplied flat buffer.  Each block is at
 * least sizeof(void*) bytes so that free blocks can store the next-free
 * pointer as an intrusive linked list inside the block itself.  Allocation
 * and deallocation are both O(1).
 */
typedef struct MemPool {
    void   *buf;            /* backing buffer supplied by caller */
    size_t  block_size;     /* bytes per block (>= sizeof(void*)) */
    size_t  count;          /* total number of blocks */
    size_t  free_count;     /* number of blocks currently available */
    void   *free_head;      /* head of free-list (stored inside blocks) */
} MemPool;

/**
 * pool_init - initialise a memory pool.
 * @pool:       pool descriptor to initialise.
 * @buf:        backing buffer (must be at least block_size * count bytes).
 * @block_size: size of each allocation unit in bytes.  Will be rounded up
 *              to at least sizeof(void*).
 * @count:      total number of blocks in the pool.
 */
void pool_init(MemPool *pool, void *buf, size_t block_size, size_t count);

/**
 * pool_alloc - allocate one block.
 * Returns a pointer to the block on success, NULL if the pool is
 * exhausted.  O(1).
 */
void *pool_alloc(MemPool *pool);

/**
 * pool_free - return a block to the pool.
 * @ptr must be a pointer previously returned by pool_alloc() on this
 * pool.  Behaviour is undefined if @ptr is invalid.  O(1).
 */
void pool_free(MemPool *pool, void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* MICRO_SCHED_MEM_POOL_H */
