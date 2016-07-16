/*
Copyright (c) 2015 ForkGC authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "alloc.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "util.h"

/****************************************************************************/
/*                         Defines, typedefs, etc.                          */
/****************************************************************************/

// Block size for allocating internal data structures.
#define ALLOC_BLOCKSIZE PAGESIZE

typedef struct memory_metadata_t memory_metadata_t;

/****************************************************************************/
/*                        Internal memory tracking.                         */
/****************************************************************************/

struct memory_metadata_t
{
    void *addr;
    size_t length;
    memory_metadata_t *next, *prev;
};

static size_t LOW_ADDR (memory_metadata_t *m) {
    return (size_t)m->addr;
}

static size_t HIGH_ADDR (memory_metadata_t *m) {
    return LOW_ADDR(m) + m->length;
}

static memory_metadata_t *free_list = NULL;
static memory_metadata_t *alloc_list = NULL;

static pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Wrap mmap(), since we only really use it as a great big malloc().  This
 * function will terminate the program if it is unable to allocate memory.
 * @return A pointer to the newly allocated memory.  Failure is not an option!
 */
static void *mmap_wrap (size_t size, int shared)
{
    void *ptr = mmap(NULL, size,
                     PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | (shared ? MAP_SHARED : MAP_PRIVATE),
                     -1, 0);
    if (MAP_FAILED == ptr) {
        forkgc_fatal("failed mmap().\n");
    }
    assert(ptr);
    return ptr;
}

/**
 * Wrapper for munmap to be symetrical with mmap/mmap_wrap.
 */
int munmap_wrap (void *addr, size_t length)
{
    return munmap(addr, length);
}

/**
 * Perform the insertion of the metadata into the list.  Why not embed this
 * in metadata_insert()?  Sometimes we want to insert while already holding
 * the lock.  Note: Ensure the lock is held when calling this function.
 */
static void metadata_do_insert (memory_metadata_t *meta)
{
    if (NULL == alloc_list) {
        alloc_list = meta;
        meta->next = meta->prev = meta;
    } else if (HIGH_ADDR(alloc_list->prev) <= LOW_ADDR(meta)) {
        // What's all this?  This is the common case, if mmap() is allocating
        // blocks at increasing addresses.  This optimizes when the block
        // needs to go at the end of the list.
        meta->prev = alloc_list->prev;
        meta->next = alloc_list;
        alloc_list->prev->next = meta;
        alloc_list->prev = meta;
    } else {
        memory_metadata_t *curr = alloc_list;
        while (LOW_ADDR(curr) < HIGH_ADDR(meta)) curr = curr->next;
        meta->next = curr;
        meta->prev = curr->prev;
        curr->prev->next = meta;
        curr->prev = meta;
        if (curr == alloc_list) alloc_list = meta;
    }
}

/**
 * Insert the metadata node into the allocated list.
 */
static void metadata_insert (memory_metadata_t *meta)
{
    /* The list is sorted.  There should not be many entries, so operations
       should be cheap. */

    pthread_mutex_lock(&list_lock);
    metadata_do_insert(meta);
    pthread_mutex_unlock(&list_lock);
}

/**
 * Remove the memory_metadata_t object containing *addr from the allocated
 * list and return it.
 */
static memory_metadata_t *metadata_remove (void *addr)
{
    memory_metadata_t *curr = NULL;

    assert(alloc_list); // Without the lock?!  It _should_ point to something.

    pthread_mutex_lock(&list_lock);
    for (curr = alloc_list; curr->addr != addr; curr = curr->next);
    if (NULL == curr) {
        forkgc_fatal("internal error at %s:%d\n",
                     __FILE__, __LINE__);
    }
    curr->next->prev = curr->prev;
    curr->prev->next = curr->next;
    if (curr == alloc_list) alloc_list = curr->next;
    pthread_mutex_unlock(&list_lock);

    // For sanity's sake:
    curr->next = curr->prev = NULL;

    return curr;
}

/**
 * Return a fresh memory_metadata_t object.
 */
static memory_metadata_t *metadata_new ()
{
    pthread_mutex_lock(&list_lock);
    if (NULL == free_list) {
        // No free nodes.  Make a few.
        size_t offset;
        char *p = (char*)mmap_wrap(ALLOC_BLOCKSIZE, 0);

        // The first entry is this memory block's metadata.
        memory_metadata_t *node = (memory_metadata_t*)p;
        node->addr = (void*)node;
        node->length = ALLOC_BLOCKSIZE;
        metadata_do_insert(node);

        // Turn the rest of the memory into a list of nodes.
        for (offset = sizeof(memory_metadata_t);
             offset < ALLOC_BLOCKSIZE - sizeof(memory_metadata_t);
             offset += sizeof(memory_metadata_t)) {
            node = (memory_metadata_t*)(p + offset);
            node->next = free_list;
            free_list = node;
        }
    }

    // Grab the first "free" entry and return it.
    memory_metadata_t *ret = free_list;
    assert(ret);
    free_list = free_list->next;
    pthread_mutex_unlock(&list_lock);
    return ret;
}

/**
 * Return a memory_metadata_t object to the pool of memory whence it came.
 */
static void metadata_free (memory_metadata_t *meta)
{
    pthread_mutex_lock(&list_lock);
    meta->next = free_list;
    free_list = meta;
    pthread_mutex_unlock(&list_lock);
}

/**
 * See forkgc_alloc_next_subrange().  This is not thread-safe.
 */
static mem_range_t metadata_break_range (mem_range_t *big_range)
{
    mem_range_t ret = *big_range;
    memory_metadata_t *curr;

    assert(alloc_list);

    // Advance to where big_range begins.
    curr = alloc_list;
    while (HIGH_ADDR(curr) <= ret.low) {
        curr = curr->next;
        if (curr == alloc_list) break;
    }

    if (HIGH_ADDR(curr) > ret.low && LOW_ADDR(curr) < ret.high) {
        // Advance the beginning of the range until it reaches something we
        // haven't allocated.
        while (LOW_ADDR(curr) <= ret.low) {
            if (HIGH_ADDR(curr) >= ret.low) {
                ret.low = MIN_OF(HIGH_ADDR(curr), ret.high);
            }
            curr = curr->next;
            if (curr == alloc_list) break;
        }
        if (LOW_ADDR(curr) > ret.low) {
            ret.high = MIN_OF(LOW_ADDR(curr), ret.high);
        }
    } else {
        // The big_range falls entirely outside the range of our allocations.
    }

    big_range->low = ret.high;

    return ret;
}

static void *alloc_mmap (size_t size, int shared)
{
    memory_metadata_t *meta = metadata_new();
    assert(size % PAGESIZE == 0);
    assert(meta);
    meta->length = size;
    meta->addr = mmap_wrap(size, shared);
    assert(meta->addr && meta->addr != MAP_FAILED);
    metadata_insert(meta);
    if (0 != mprotect(meta->addr, size, PROT_READ | PROT_WRITE)) {
        forkgc_diagnostic("mprotect failed %s:%d\n",
                          __FILE__, __LINE__);
    }
    return meta->addr;
}

/****************************************************************************/
/*                                Interface                                 */
/****************************************************************************/

/**
 * mmap() for the ForkGC system.  This call never fails.  But you should
 * only ever ask for big chunks in multiples of the page size.
 * @return The allocated memory.
 */
void *forkgc_alloc_mmap (size_t size)
{
    return alloc_mmap(size, /*shared=*/0);
}

/**
 * mmap() for the ForkGC system.  This call never fails.  But you should
 * only ever ask for big chunks in multiples of the page size.  The mmapped
 * memory is marked as shared among processes.
 * @return The allocated memory.
 */
void *forkgc_alloc_mmap_shared (size_t size)
{
    return alloc_mmap(size, /*shared=*/1);
}

/**
 * munmap() for the ForkGC system.
 */
void forkgc_alloc_munmap (void *ptr)
{
    assert(ptr);

    memory_metadata_t *meta = metadata_remove(ptr);
    if (NULL == meta) {
        forkgc_fatal("lost track of memory.\n");
    }
    if (0 != munmap_wrap(meta->addr, meta->length)) {
        forkgc_fatal("failed munmap().\n");
    }
    metadata_free(meta);
}

/**
 * Given a *big_range, return the first chunk of it that doesn't contain
 * memory that belongs to ForkGC.  *big_range is modified to show the
 * remaining portion of the range.  This is not thread-safe.
 *
 * @return A chunk of memory that does not overlap with memory owned by
 * ForkGC.  This chunk may have zero length if no such chunk could be
 * found in *big_range.
 */
mem_range_t forkgc_alloc_next_subrange (mem_range_t *big_range)
{
    // FIXME: Using mem_range_t creates a circular dependency on util.h.
    return metadata_break_range(big_range);
}
