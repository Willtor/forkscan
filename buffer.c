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

    return ab;
}

addr_buffer_t *forkscan_make_aggregate_buffer (int capacity)
{
    addr_buffer_t *ab;

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

    return ab;
}

void forkscan_release_buffer (addr_buffer_t *ab)
{
    if (ab->capacity == g_default_capacity) {
        pthread_mutex_lock(&g_reclaimer_list_lock);
        ab->next = g_reclaimer_list;
        g_reclaimer_list = ab;
        pthread_mutex_unlock(&g_reclaimer_list_lock);
    } else {
        forkgc_alloc_munmap(ab); // FIXME: Munmap is bad.
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
        g_last_retiree_buffer->next = ab;
        g_last_retiree_buffer = ab;
    }
    pthread_mutex_unlock(&g_retiree_mutex);
}

void forkscan_buffer_pop_retiree_buffer (addr_buffer_t *ab)
{
    if (g_first_retiree_buffer != ab) return;
    pthread_mutex_lock(&g_retiree_mutex);
    if (g_first_retiree_buffer == ab) {
        if (g_last_retiree_buffer == ab) {
            g_last_retiree_buffer = NULL;
            g_first_retiree_buffer = NULL;
        } else {
            g_first_retiree_buffer = ab->next;
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
