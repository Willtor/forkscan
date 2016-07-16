/*
Copyright (c) 2016 Forkscan authors

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
#include "buffer.h"
#include "env.h"
#include <pthread.h>

static int g_default_capacity;
static pthread_mutex_t g_reclaimer_list_lock = PTHREAD_MUTEX_INITIALIZER;
static addr_buffer_t *g_reclaimer_list;

static addr_buffer_t *g_first_retiree_buffer;
static addr_buffer_t *g_last_retiree_buffer;
static pthread_mutex_t g_retiree_mutex = PTHREAD_MUTEX_INITIALIZER;

static addr_buffer_t *g_available_aggregates;
static pthread_mutex_t g_aa_mutex = PTHREAD_MUTEX_INITIALIZER;

#include <stdio.h>
addr_buffer_t *forkscan_make_reclaimer_buffer ()
{
    addr_buffer_t *ab = g_reclaimer_list;

    // See if there is already a reclaimer buffer waiting around to be
    // reused.
    if (NULL != ab) {
        pthread_mutex_lock(&g_reclaimer_list_lock);
        ab = g_reclaimer_list;
        // Only one thread should ever be trying to take a reclaimer buffer
        // at a time.
        assert(ab);
        g_reclaimer_list = ab->next;
        pthread_mutex_unlock(&g_reclaimer_list_lock);
        ab->n_addrs = 0;
        assert(ab->ref_count == 0);
        return ab;
    }

    if (0 == g_default_capacity) {
        g_default_capacity = g_forkgc_ptrs_per_thread * MAX_THREAD_COUNT;
    }
    size_t sz = g_default_capacity * sizeof(size_t) + PAGESIZE;
    char *raw_mem = forkgc_alloc_mmap(sz);

    //   0 - 4095: Reserved page for the addr_buffer_t struct.
    //   4096 -  : Address list.
    ab = (addr_buffer_t*)raw_mem;
    ab->addrs = (size_t*)&raw_mem[PAGESIZE];
    ab->n_addrs = 0;
    ab->capacity = g_default_capacity;
    ab->is_aggregate = 0;
    ab->ref_count = 0;

    return ab;
}

addr_buffer_t *forkscan_make_aggregate_buffer (int capacity)
{
    addr_buffer_t *ab;

    // Round capacity up to the nearest page size.
    if (capacity & (PAGESIZE / sizeof(size_t) - 1)) {
        capacity -= capacity & (PAGESIZE / sizeof(size_t) - 1);
        capacity += PAGESIZE / sizeof(size_t);
    }

    if (g_available_aggregates != NULL) {
        pthread_mutex_lock(&g_aa_mutex);
        ab = g_available_aggregates;
        while (ab && ab->capacity < capacity) {
            addr_buffer_t *tmp = ab->next;
            forkgc_alloc_munmap(ab);
            ab = tmp;
        }
        if (ab) {
            g_available_aggregates = ab->next;
            ab->next = NULL;
            pthread_mutex_unlock(&g_aa_mutex);
            ab->n_addrs = 0;
            assert(ab->ref_count == 0);
            return ab;
        }
        // None of the available buffers were big enough, and all were
        // munmap'd.
        g_available_aggregates = NULL;
        pthread_mutex_unlock(&g_aa_mutex);
    }

    // How many pages of memory are needed to store this many addresses?
    size_t pages_of_addrs = ((capacity * sizeof(size_t))
                             + PAGESIZE - sizeof(size_t)) / PAGESIZE;
    // How many pages of memory are needed to store the minimap?
    size_t pages_of_minimap = ((pages_of_addrs * sizeof(size_t))
                               + PAGESIZE - sizeof(size_t)) / PAGESIZE;
    // Total pages needed is the number of pages for the addresses, plus the
    // number of pages needed for the minimap, plus one (for the
    // addr_buffer_t).
    char *p =
        (char*)forkgc_alloc_mmap_shared((pages_of_addrs     // addr array.
                                         + pages_of_minimap // minimap.
                                         + 1)               // struct page.
                                        * PAGESIZE);

    // Perform assignments as offsets into the block that was bulk-allocated.
    size_t offset = 0;
    ab = (addr_buffer_t*)p;
    offset += PAGESIZE;

    ab->addrs = (size_t*)(p + offset);
    offset += pages_of_addrs * PAGESIZE;

    ab->minimap = (size_t*)(p + offset);
    offset += pages_of_minimap * PAGESIZE;

    ab->capacity = capacity;
    ab->is_aggregate = 1;
    ab->ref_count = 0;

    return ab;
}

void forkscan_release_buffer (addr_buffer_t *ab)
{
    assert(ab != g_first_retiree_buffer);
    assert(ab != g_last_retiree_buffer);
    if (ab->is_aggregate == 0) {
        assert(ab->capacity == g_default_capacity);
        pthread_mutex_lock(&g_reclaimer_list_lock);
        ab->next = g_reclaimer_list;
        g_reclaimer_list = ab;
        pthread_mutex_unlock(&g_reclaimer_list_lock);
    } else {
        pthread_mutex_lock(&g_aa_mutex);
        ab->next = g_available_aggregates;
        g_available_aggregates = ab;
        pthread_mutex_unlock(&g_aa_mutex);
    }
}

void forkscan_buffer_push_back (addr_buffer_t *ab)
{
    ab->ref_count = 1;
    ab->free_idx = 0;
    ab->next = NULL;

    pthread_mutex_lock(&g_retiree_mutex);
    if (NULL == g_last_retiree_buffer) {
        g_last_retiree_buffer = g_first_retiree_buffer = ab;
    } else {
        // FIXME: What if when the buff was removed from the list, it wasn't
        // at the front?
        g_last_retiree_buffer->next = ab;
        g_last_retiree_buffer = ab;
    }
    pthread_mutex_unlock(&g_retiree_mutex);
}

void forkscan_buffer_pop_retiree_buffer (addr_buffer_t *ab)
{
    addr_buffer_t *curr, *prev = NULL;
    pthread_mutex_lock(&g_retiree_mutex);
    curr = g_first_retiree_buffer;
    while (curr != NULL && ab != curr) {
        prev = curr;
        curr = curr->next;
    }
    if (curr == ab) {
        // ab was still in the list of retiree buffers.  Splice it out.
        if (prev) prev->next = curr->next;
        else g_first_retiree_buffer = curr->next;

        if (g_first_retiree_buffer == NULL) {
            g_last_retiree_buffer = NULL;
        }
    }
    pthread_mutex_unlock(&g_retiree_mutex);
}

addr_buffer_t *forkscan_buffer_get_retiree_buffer ()
{
    addr_buffer_t *ret = NULL;
    if (NULL == g_first_retiree_buffer) return NULL;
    pthread_mutex_lock(&g_retiree_mutex);
    if (NULL != g_first_retiree_buffer) {
        ret = g_first_retiree_buffer;
        ++ret->ref_count;
    }
    pthread_mutex_unlock(&g_retiree_mutex);
    return ret;
}

void forkscan_buffer_unref_buffer (addr_buffer_t *ab)
{
    pthread_mutex_lock(&g_retiree_mutex);
    if (0 == --ab->ref_count) {
        if (ab->free_idx >= ab->n_addrs) {
            forkscan_release_buffer(ab);
        }
    }
    pthread_mutex_unlock(&g_retiree_mutex);
}
