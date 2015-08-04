/*
Copyright (c) 2015 ThreadScan authors

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

#define _GNU_SOURCE // For pthread_yield().
#include "alloc.h"
#include <assert.h>
#include "child.h"
#include "forkgc.h"
#include <malloc.h>
#include "proc.h"
#include <pthread.h>
#include "queue.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef NDEBUG
#define assert_monotonicity(a, n)                       \
    __assert_monotonicity(a, n, __FILE__, __LINE__)
static void __assert_monotonicity (size_t *a, int n, const char *f, int line)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            threadscan_diagnostic("Error at %s:%d\n", f, line);
            threadscan_fatal("The list is not monotonic at position %d "
                             "out of %d (%llu, last: %llu)\n",
                             i, n, a[i], last);
        }
        last = a[i];
    }
}
#else
#define assert_monotonicity(a, b) /* nothing. */
#endif

// For signaling the garbage collector with work to do.
static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gc_cond = PTHREAD_COND_INITIALIZER;

static gc_data_t *g_gc_data, *g_uncollected_data;
static volatile int g_received_signal;
static volatile size_t g_cleanup_counter;
static int g_gc_waiting;

static pid_t child_pid;

static size_t read_from_child (queue_t *commq)
{
    while (threadscan_queue_is_empty(commq)) {
        pthread_yield();
    }
    return threadscan_queue_pop(commq);
}

static void generate_minimap (gc_data_t *gc_data)
{
    size_t i;

    assert(gc_data);
    assert(gc_data->addrs);
    assert(gc_data->minimap);

    gc_data->n_minimap = 0;
    for (i = 0; i < gc_data->n_addrs; i += (PAGESIZE / sizeof(size_t))) {
        gc_data->minimap[gc_data->n_minimap] = gc_data->addrs[i];
        ++gc_data->n_minimap;
    }
}

static gc_data_t *aggregate_gc_data (gc_data_t *data_list)
{
    gc_data_t *ret, *tmp;
    size_t n_addrs = 0;
    int list_count = 0;

    tmp = data_list;
    do {
        n_addrs += tmp->n_addrs;
        ++list_count;
    } while ((tmp = tmp->next));
    //threadscan_diagnostic("List of %d dealies.\n", list_count);

    assert(n_addrs != 0);

    // How many pages of memory are needed to store this many addresses?
    size_t pages_of_addrs = ((n_addrs * sizeof(size_t))
                             + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages of memory are needed to store the minimap?
    size_t pages_of_minimap = ((pages_of_addrs * sizeof(size_t))
                               + PAGE_SIZE - sizeof(size_t)) / PAGE_SIZE;
    // How many pages are needed to store the allocated size and reference
    // count arrays?
    size_t pages_of_count = ((n_addrs * sizeof(int))
                             + PAGE_SIZE - sizeof(int)) / PAGE_SIZE;
    // Total pages needed is the number of pages for the addresses, plus the
    // number of pages needed for the minimap, plus one (for the gc_data_t).
    char *p =
        (char*)threadscan_alloc_mmap((pages_of_addrs     // addr array.
                                      + pages_of_minimap // minimap.
                                      + pages_of_count   // ref count.
                                      + pages_of_count   // alloc size.
                                      + 1)               // struct page.
                                     * PAGE_SIZE);

    // Perform assignments as offsets into the block that was bulk-allocated.
    size_t offset = 0;
    ret = (gc_data_t*)p;
    offset += PAGE_SIZE;

    ret->addrs = (size_t*)(p + offset);
    offset += pages_of_addrs * PAGE_SIZE;

    ret->minimap = (size_t*)(p + offset);
    offset += pages_of_minimap * PAGE_SIZE;

    ret->refs = (int*)(p + offset);
    offset += pages_of_count * PAGE_SIZE;

    ret->alloc_sz = (int*)(p + offset);

    ret->n_addrs = n_addrs;

    // Copy the addresses over.
    char *dest = (char*)ret->addrs;
    tmp = data_list;
    do {
        memcpy(dest, tmp->addrs, tmp->n_addrs * sizeof(size_t));
        dest += tmp->n_addrs * sizeof(size_t);
    } while ((tmp = tmp->next));

    // Sort the addresses and generate the minimap for the scanner.
    threadscan_util_sort(ret->addrs, ret->n_addrs);
    assert_monotonicity(ret->addrs, ret->n_addrs);
    generate_minimap(ret);

    // Get the size of each malloc'd block.
    int i;
    for (i = 0; i < ret->n_addrs; ++i) {
        assert(ret->alloc_sz[i] == 0);
        ret->alloc_sz[i] = malloc_usable_size((void*)ret->addrs[i]);
        assert(ret->alloc_sz[i] > 0);
    }

#ifndef NDEBUG
    for (i = 0; i < ret->n_addrs; ++i) {
        assert(ret->refs[i] == 0);
    }
#endif

    return ret;
}

static void garbage_collect (gc_data_t *gc_data, queue_t *commq)
{
    static int fork_count = 0;
    gc_data_t *working_data;
    int sig_count;

    // Include the addrs from the last collection iteration.
    if (g_uncollected_data) {
        gc_data_t *tmp = g_uncollected_data;
        while (tmp->next) tmp = tmp->next;
        tmp->next = gc_data;
        gc_data = g_uncollected_data;
    }

    working_data = aggregate_gc_data(gc_data);

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    g_received_signal = 0;
    sig_count = threadscan_proc_signal(SIGTHREADSCAN);
    while (g_received_signal < sig_count) pthread_yield();
    child_pid = fork();

    if (child_pid == -1) {
        threadscan_fatal("Collection failed (fork).\n");
    } else if (child_pid == 0) {
        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        threadscan_child(working_data, commq);
        exit(0);
    }

    threadscan_diagnostic("Fork count: %d\n", ++fork_count);

    // Parent: Listen to the child process.  It will report pointers to free
    // followed by pointers that could not be collected.
    size_t addr;

    ++g_cleanup_counter;

    gc_data->n_addrs = 0;
    g_uncollected_data = NULL;
    // Collect nodes to free.
    while (0 != (addr = read_from_child(commq))) {
        void *p = (void*)addr;
        memset(p, 0, malloc_usable_size(p));
        free(p);
    }
    // Collect nodes that could not be free'd.
    while (0 != (addr = read_from_child(commq))) {
        if (g_uncollected_data == NULL) g_uncollected_data = gc_data;
        if (gc_data->n_addrs >= gc_data->capacity) {
            gc_data = gc_data->next;
            assert(gc_data != NULL);
            gc_data->n_addrs = 0;
        }
        gc_data->addrs[gc_data->n_addrs++] = addr;
    }

    // Free up unnecessary space.
    assert(gc_data);
    gc_data_t *tmp;
    if (gc_data->n_addrs) {
        tmp = gc_data;
        gc_data = gc_data->next;
        tmp->next = NULL;
    } else {
        assert(NULL == g_uncollected_data);
    }
    while (gc_data) {
        tmp = gc_data->next;
        threadscan_alloc_munmap(gc_data); // FIXME: Munmap is bad.
        gc_data = tmp;
    }
    threadscan_alloc_munmap(working_data); // FIXME: ...
}

/****************************************************************************/
/*                            Exported functions                            */
/****************************************************************************/

/**
 * Wait for the GC routine to complete its snapshot.
 */
void forkgc_wait_for_snapshot ()
{
    size_t old_counter;
    jmp_buf env; // Spilled registers.

    // Acknowledge the signal and wait for the snapshot to complete.
    old_counter = g_cleanup_counter;
    setjmp(env);
    __sync_fetch_and_add(&g_received_signal, 1);
    while (old_counter == g_cleanup_counter) pthread_yield();
}

/**
 * Pass a list of pointers to the GC thread for it to collect.
 */
void forkgc_initiate_collection (gc_data_t *gc_data)
{
    pthread_mutex_lock(&g_gc_mutex);
    gc_data->next = g_gc_data;
    g_gc_data = gc_data;
    if (g_gc_waiting != 0) {
        pthread_cond_signal(&g_gc_cond);
    }
    pthread_mutex_unlock(&g_gc_mutex);
}

/**
 * Garbage-collector thread.
 */
void *forkgc_thread (void *ignored)
{
    gc_data_t *gc_data;

    // FIXME: Warning: Fragile code knows the size of a pointer and a page.
    char *buffer = threadscan_alloc_mmap_shared(PAGE_SIZE * 9);
    queue_t *commq = (queue_t*)buffer;
    threadscan_queue_init(commq, (size_t*)&buffer[PAGE_SIZE], PAGE_SIZE);

    while ((1)) {
        pthread_mutex_lock(&g_gc_mutex);
        if (NULL == g_gc_data) {
            // Wait for somebody to come up with a set of addresses for us to
            // collect.
            g_gc_waiting = 1;
            pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
            g_gc_waiting = 0;
        }

        assert(g_gc_data);
        gc_data = g_gc_data;
        g_gc_data = NULL;
        pthread_mutex_unlock(&g_gc_mutex);

#ifndef NDEBUG
        int n = 1;
        gc_data_t *tmp = gc_data;
        while (NULL != (tmp = tmp->next)) ++n;
        threadscan_diagnostic("%d collects waiting.\n", n);
#endif

        garbage_collect(gc_data, commq);
    }

    return NULL;
}

__attribute__((destructor))
static void process_death ()
{
    if (child_pid > 0) {
        // There's still an outstanding child.  Kill it.
        kill(child_pid, 9);
    }
}
