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
#include "env.h"
#include <fcntl.h> // FIXME: needed?
#include "forkgc.h"
#include <malloc.h>
#include "proc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thread.h"
#include "util.h"

/****************************************************************************/
/*                                  Macros                                  */
/****************************************************************************/

#define SET_LOW_BIT(p) do {                      \
        size_t v = (size_t)*(p);                 \
        if ((v & 1) == 0) { *(p) = (v + 1); }    \
    } while (0)

/****************************************************************************/
/*                           Typedefs and structs                           */
/****************************************************************************/

typedef struct threadscan_data_t threadscan_data_t;

struct threadscan_data_t {
    int max_ptrs; // Max pointer count that can be tracked during reclamation.

    // Size of the BIG buffer used to store pointers for a collection run.
    size_t working_buffer_sz;
};

/****************************************************************************/
/*                                 Globals                                  */
/****************************************************************************/

__attribute__((visibility("default")))
void threadscan_collect (void *ptr);

static threadscan_data_t g_tsdata;

static volatile __thread int g_in_malloc = 0;
static __thread int g_waiting_to_fork = 0;

/****************************************************************************/
/*                            Pointer tracking.                             */
/****************************************************************************/

static void generate_working_pointers_list (gc_data_t *gc_data)
{
    int n = 0;
    thread_list_t *thread_list = threadscan_proc_get_thread_list();
    thread_data_t *td;

    // Add the pointers from each of the individual thread buffers.
    FOREACH_IN_THREAD_LIST(td, thread_list)
        assert(td);
        n += threadscan_queue_pop_bulk(&gc_data->addrs[n],
                                       g_tsdata.max_ptrs - n,
                                       &td->ptr_list);
    ENDFOREACH_IN_THREAD_LIST(td, thread_list);

    gc_data->n_addrs = n;
    assert(!threadscan_queue_is_full(&threadscan_thread_get_td()->ptr_list));
}

/****************************************************************************/
/*                             Cleanup thread.                              */
/****************************************************************************/

static void threadscan_reclaim ()
{
    char *working_memory;   // Block of memory to free.
    gc_data_t *gc_data;

    // Get memory to store the list of pointers:
    //   0 - 4095: Reserved page for the gc_data_t struct.
    //   4096 -  : Address list.
    working_memory = threadscan_alloc_mmap(g_tsdata.working_buffer_sz);
    gc_data = (gc_data_t*)working_memory;
    gc_data->addrs = (size_t*)&working_memory[PAGE_SIZE];
    gc_data->n_addrs = 0;
    gc_data->capacity = g_tsdata.max_ptrs;

    // Copy the pointers into the list.
    generate_working_pointers_list(gc_data);

    // Give the list to the gc thread, signaling it if it's asleep.
    forkgc_initiate_collection(gc_data);
    threadscan_thread_cleanup_release();
}

/**
 * Interface for applications.  "Collecting" a pointer registers it with
 * threadscan.  When a sweep of memory occurs, all registered pointers are
 * sought in memory.  Any that can't be found are free()'d because no
 * remaining threads have pointers to them.
 */
__attribute__((visibility("default")))
void threadscan_collect (void *ptr)
{
    if (NULL == ptr) {
        threadscan_diagnostic("Tried to collect NULL.\n");
        return;
    }

    thread_data_t *td = threadscan_thread_get_td();
    threadscan_queue_push(&td->ptr_list, (size_t)ptr); // Add the pointer.
    while (threadscan_queue_is_full(&td->ptr_list)) {
        // While this thread's local queue of pointers is full, try to initiate
        // reclamation.

        threadscan_thread_cleanup_try_acquire()
            ? threadscan_reclaim() // reclaim() will release the cleanup lock.
            : pthread_yield();
    }
}

/****************************************************************************/
/*                            Bystander threads.                            */
/****************************************************************************/

__attribute__((visibility("default")))
void *automalloc (size_t size)
{
    void *p;
    g_in_malloc = 1;
    p = malloc(size);
    g_in_malloc = 0;

    if (g_waiting_to_fork) {
        // Sadly, TC-Malloc has a deadlock bug when interacting with fork().
        // We need to make sure it isn't holding the global lock when we
        // initiate cleanup.
        forkgc_wait_for_snapshot();
        g_waiting_to_fork = 0;
    }

    threadscan_collect(p);
    return p;
}

/**
 * Got a signal from a thread wanting to do cleanup.
 */
static void signal_handler (int sig)
{
    assert(SIGTHREADSCAN == sig);
    if (g_in_malloc) {
        g_waiting_to_fork = 1;
        return;
    }
    forkgc_wait_for_snapshot();
}

/**
 * Like it sounds.
 */
__attribute__((constructor))
static void register_signal_handlers ()
{
    /* We signal threads to get them to stop while we prepare a snapshot
       on the cleanup thread. */
    if (signal(SIGTHREADSCAN, signal_handler) == SIG_ERR) {
        threadscan_fatal("threadscan: Unable to register signal handler.\n");
    }

    g_tsdata.max_ptrs = g_threadscan_ptrs_per_thread * MAX_THREAD_COUNT;

    // Calculate reserved space for stored addresses.
    g_tsdata.working_buffer_sz = g_tsdata.max_ptrs * sizeof(size_t)
        + PAGE_SIZE;
}
