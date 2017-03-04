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

#define _GNU_SOURCE // For pthread_yield().
#include "alloc.h"
#include <assert.h>
#include "child.h"
#include "env.h"
#include "forkscan.h"
#include <jemalloc/jemalloc.h>
#include "proc.h"
#include <pthread.h>
#include <string.h>
#include "thread.h"
#include <unistd.h>
#include "util.h"

/****************************************************************************/
/*                           Typedefs and structs                           */
/****************************************************************************/

typedef struct config_t config_t;

struct config_t {
    int max_ptrs; // Max pointer count that can be tracked during reclamation.

    // Size of the BIG buffer used to store pointers for a collection run.
    size_t working_buffer_sz;
};

/****************************************************************************/
/*                                 Globals                                  */
/****************************************************************************/

static config_t g_config;

static volatile __thread int g_in_malloc = 0;
static __thread int g_waiting_to_fork = 0;

/****************************************************************************/
/*                                Reclaimer.                                */
/****************************************************************************/

static void generate_working_pointers_list (addr_buffer_t *ab)
{
    int n = 0;
    thread_list_t *thread_list = forkgc_proc_get_thread_list();
    thread_data_t *td;

    // Add the pointers from each of the individual thread buffers.
    FOREACH_IN_THREAD_LIST(td, thread_list)
        assert(td);
        n += forkgc_queue_pop_bulk(&ab->addrs[n],
                                   g_config.max_ptrs - n,
                                   &td->ptr_list);
    ENDFOREACH_IN_THREAD_LIST(td, thread_list);

    ab->n_addrs = n;
    assert(!forkgc_queue_is_full(&forkgc_thread_get_td()->ptr_list));
}

static void become_reclaimer ()
{
    addr_buffer_t *ab;

    // Get memory to store the list of pointers:
    ab = forkscan_make_reclaimer_buffer();

    // Copy the pointers into the list.
    generate_working_pointers_list(ab);

    // Give the list to the gc thread, signaling it if it's asleep.
    forkgc_initiate_collection(ab);
    forkgc_thread_cleanup_release();
}

/****************************************************************************/
/*                            Bystander threads.                            */
/****************************************************************************/

static void yield (size_t n_yields)
{
    // FIXME: There's performance here... sure of it!
    //if (n_yields > 10) usleep(MIN_OF(n_yields, 100));
    //else pthread_yield();
    g_in_malloc = 1;
    forkscan_util_free_ptrs(forkgc_thread_get_td());
    g_in_malloc = 0;
    if (g_waiting_to_fork) {
        g_waiting_to_fork = 0;
        forkgc_acknowledge_signal();
    }
    pthread_yield();
}

/**
 * Got a signal from a thread wanting to do cleanup.
 */
static void signal_handler (int sig)
{
    assert(SIGFORKGC == sig);
    if (g_in_malloc) {
        g_waiting_to_fork = 1;
        return;
    }
    forkgc_acknowledge_signal();
}

/**
 * Like it sounds.
 */
__attribute__((constructor))
static void register_signal_handlers ()
{
    /* We signal threads to get them to stop while we prepare a snapshot
       on the cleanup thread. */
    if (signal(SIGFORKGC, signal_handler) == SIG_ERR) {
        forkgc_fatal("Unable to register signal handler.\n");
    }

    g_config.max_ptrs = g_forkgc_ptrs_per_thread * MAX_THREAD_COUNT;

    // Calculate reserved space for stored addresses.
    g_config.working_buffer_sz = g_config.max_ptrs * sizeof(size_t)
        + PAGESIZE;
}

/****************************************************************************/
/*                            Exported Functions                            */
/****************************************************************************/

/**
 * Allocate memory of the specified size from ForkGC's pool and return it.
 * This memory is untracked by the system.
 */
__attribute__((visibility("default")))
void *forkgc_malloc (size_t size)
{
    void *p;
    g_in_malloc = 1;
    p = MALLOC(size);

    // Free a couple pointers, if we have them.
    forkscan_util_free_ptrs(forkgc_thread_get_td());
    g_in_malloc = 0;

    if (g_waiting_to_fork) {
        // Sadly, TC-Malloc has a deadlock bug when interacting with fork().
        // We need to make sure it isn't holding the global lock when we
        // initiate cleanup.
        g_waiting_to_fork = 0;
        forkgc_acknowledge_signal();
    }
    return p;
}

/**
 * Retire a pointer allocated by ForkGC so that it will be free'd for reuse
 * when no remaining references to it exist.
 */
__attribute__((visibility("default")))
void forkgc_retire (void *ptr)
{
    if (NULL == ptr) {
        forkgc_diagnostic("Tried to collect NULL.\n");
        return;
    }

    thread_data_t *td = forkgc_thread_get_td();
    forkgc_queue_push(&td->ptr_list, (size_t)ptr); // Add the pointer.
    if (forkgc_queue_is_full(&td->ptr_list)) {
        size_t start, end;
        size_t n_loops = 0;

        start = forkscan_rdtsc();
        do {
            // While this thread's local queue of pointers is full, try to
            // initiate reclamation.

            forkgc_thread_cleanup_try_acquire()
                ? become_reclaimer() // this will release the cleanup lock.
                : yield(n_loops);
        } while (forkgc_queue_is_full(&td->ptr_list));
        end = forkscan_rdtsc();
        td->wait_time_ms += end - start;
    }
}

/**
 * Free a pointer allocated by ForkGC.  The memory may be immediately reused,
 * so if there is any possibility another thread may know about this memory
 * and might read from it, forkgc_retire() should be used instead.
 */
__attribute__((visibility("default")))
void forkgc_free (void *ptr)
{
    g_in_malloc = 1;
    FREE(ptr);
    g_in_malloc = 0;

    if (g_waiting_to_fork) {
        g_waiting_to_fork = 0;
        forkgc_acknowledge_signal();
    }
}

/**
 * Allocate a buffer of "size" bytes and return a pointer to it.  This memory
 * will be tracked by the garbage collector, so free() should never be called
 * on it.
 */
__attribute__((visibility("default")))
void *forkgc_automalloc (size_t size)
{
    void *p = forkgc_malloc(size);
    forkgc_retire(p);
    return p;
}
