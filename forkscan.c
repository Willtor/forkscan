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
#include <fcntl.h>
#include "forkscan.h"
#include <jemalloc/jemalloc.h>
#include <malloc.h>
#include "proc.h"
#include <pthread.h>
#include "queue.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "thread.h"
#include <unistd.h>

#define MAX_FREE_LIST_LENGTH 128

#define SAVINGS_THRESHOLD 2048

#define PIPE_READ 0
#define PIPE_WRITE 1

#ifndef NDEBUG
#define assert_monotonicity(a, n)                       \
    __assert_monotonicity(a, n, __FILE__, __LINE__)
static void __assert_monotonicity (size_t *a, int n, const char *f, int line)
{
    size_t last = 0;
    int i;
    for (i = 0; i < n; ++i) {
        if (a[i] <= last) {
            forkgc_diagnostic("Error at %s:%d\n", f, line);
            forkgc_fatal("The list is not monotonic at position %d "
                         "out of %d (%llu, last: %llu)\n",
                         i, n, a[i], last);
        }
        last = a[i];
    }
}
#else
#define assert_monotonicity(a, b) /* nothing. */
#endif

typedef struct unref_config_t unref_config_t;

struct unref_config_t
{
    addr_buffer_t *ab;
    size_t min_val, max_val;
};

int g_frees_required = 8;

// For signaling the garbage collector with work to do.
static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gc_cond = PTHREAD_COND_INITIALIZER;

static addr_buffer_t *g_addr_buffer, *g_uncollected_data;
static volatile int g_waiting_collects;
static pthread_mutex_t g_client_waiting_lock;
static pthread_cond_t g_client_waiting_cond;

static volatile int g_received_signal;
static volatile size_t g_cleanup_counter;
static enum { GC_NOT_WAITING,
              GC_WAITING_FOR_WORK } g_gc_waiting = GC_WAITING_FOR_WORK;
static size_t g_scan_max;
static double g_total_fork_time;
static pid_t child_pid;

size_t g_total_wait_time_ms = 0;

static void generate_minimap (addr_buffer_t *ab)
{
    size_t i;

    assert(ab);
    assert(ab->addrs);
    assert(ab->minimap);

    ab->n_minimap = 0;
    for (i = 0; i < ab->n_addrs; i += (PAGESIZE / sizeof(size_t))) {
        ab->minimap[ab->n_minimap] = ab->addrs[i];
        ++ab->n_minimap;
    }
}

static addr_buffer_t *aggregate_addrs (addr_buffer_t *old,
                                       addr_buffer_t *data_list)
{
    addr_buffer_t *ret, *tmp;
    size_t n_addrs = 0;
    int list_count = 0;

    if (old) {
        n_addrs = old->n_addrs;
    }

    tmp = data_list;
    do {
        n_addrs += tmp->n_addrs;
        ++list_count;
    } while ((tmp = tmp->next));
    // FIXME: This g_frees_required calculation no longer works as planned.
    g_frees_required = MAX_OF(list_count * 8, 8);

    assert(n_addrs != 0);

    if (old && old->capacity > n_addrs) {
        ret = old;
    } else {
        ret = forkscan_make_aggregate_buffer(n_addrs > data_list->capacity ?
                                             n_addrs : data_list->capacity);
        ret->next = NULL;
        if (old) {
            memcpy(ret->addrs, old->addrs, old->n_addrs * sizeof(size_t));
            ret->n_addrs = old->n_addrs;
            forkscan_release_buffer(old);
        }
    }

    // Copy the addresses into the aggregate buffer.
    while (data_list) {
        memcpy(&ret->addrs[ret->n_addrs],
               data_list->addrs,
               data_list->n_addrs * sizeof(size_t));
        ret->n_addrs += data_list->n_addrs;
        data_list = data_list->next;
    }
    assert(ret->n_addrs == n_addrs);

    return ret;
}

static void garbage_collect (addr_buffer_t *ab)
{
    addr_buffer_t *working_data;
    addr_buffer_t *deadrefs = NULL;
    int sig_count;
    int pipefd[2];

    working_data = aggregate_addrs(g_uncollected_data, ab);
    g_uncollected_data = NULL;

    // Open a pipe for communication between parent and child.
    if (0 != pipe2(pipefd, O_DIRECT)) {
        forkgc_fatal("GC thread was unable to open a pipe.\n");
    }

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    size_t start, end;
    g_received_signal = 0;
    start = forkscan_rdtsc();
    sig_count = forkgc_proc_signal(SIGFORKGC);
    while (g_received_signal < sig_count) pthread_yield();
    deadrefs = forkscan_buffer_get_dead_references();
    child_pid = fork();

    if (child_pid == -1) {
        forkgc_fatal("Collection failed (fork).\n");
    } else if (child_pid == 0) {
        // Sort the addresses and generate the minimap for the scanner.
        forkgc_util_sort(working_data->addrs, working_data->n_addrs);
        assert_monotonicity(working_data->addrs, working_data->n_addrs);
        generate_minimap(working_data);
        if (deadrefs->n_addrs > 1) {
            // No minimap for deadrefs.
            forkgc_util_sort(deadrefs->addrs, deadrefs->n_addrs);
            assert_monotonicity(deadrefs->addrs, deadrefs->n_addrs);
        }

        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        close(pipefd[PIPE_READ]);
        forkgc_child(working_data, deadrefs, pipefd[PIPE_WRITE]);
        close(pipefd[PIPE_WRITE]);
        exit(0);
    }

    ++g_cleanup_counter;
    close(pipefd[PIPE_WRITE]);
    end = forkscan_rdtsc();
    g_total_fork_time += end - start;

    // Free up unnecessary space.
    assert(ab);
    while (ab) {
        addr_buffer_t *tmp = ab->next;
        forkscan_release_buffer(ab);
        ab = tmp;
    }

    // Wait for the child to complete the scan.
    size_t bytes_scanned;
    if (sizeof(size_t) != read(pipefd[PIPE_READ], &bytes_scanned,
                               sizeof(size_t))) {
        forkgc_fatal("Failed to read from child.\n");
    }
    if (bytes_scanned > g_scan_max) g_scan_max = bytes_scanned;
    close(pipefd[PIPE_READ]);

    // Make the unreferenced nodes, here, available for free'ing.
    forkscan_buffer_push_back(working_data);

    // Pull out all the externally-referenced addresses so they can be
    // included in the next collection round.
    assert(g_uncollected_data == NULL);
    g_uncollected_data =
        forkscan_make_aggregate_buffer(working_data->capacity);
    int i;
    for (i = 0; i < working_data->n_addrs; ++i) {
        if ((working_data->addrs[i] & 0x1) == 0) continue;
        g_uncollected_data->addrs[g_uncollected_data->n_addrs++] =
            PTR_MASK(working_data->addrs[i]);
    }

    forkscan_buffer_unref_buffer(working_data);
}

/****************************************************************************/
/*                            Exported functions                            */
/****************************************************************************/

/**
 * Acknowledge the signal sent by the GC thread and perform any work required.
 */
void forkgc_acknowledge_signal ()
{
    size_t old_counter;

    // Acknowledge the signal and wait for the snapshot to complete.
    old_counter = g_cleanup_counter;
    __sync_fetch_and_add(&g_received_signal, 1);
    while (old_counter == g_cleanup_counter) usleep(1);
}

/**
 * Pass a list of pointers to the GC thread for it to collect.
 */
void forkgc_initiate_collection (addr_buffer_t *ab)
{
    pthread_mutex_lock(&g_gc_mutex);
    ++g_waiting_collects;
    ab->next = g_addr_buffer;
    g_addr_buffer = ab;
    if (g_gc_waiting == GC_WAITING_FOR_WORK) {
        pthread_cond_signal(&g_gc_cond);
    }
    pthread_mutex_unlock(&g_gc_mutex);

    while (g_waiting_collects >= g_forkscan_throttling_queue) {
        //pthread_yield(); // FIXME: Yield?
        pthread_mutex_lock(&g_client_waiting_lock);
        if (g_waiting_collects >= g_forkscan_throttling_queue) {
            pthread_cond_wait(&g_client_waiting_cond, &g_client_waiting_lock);
        }
        pthread_mutex_unlock(&g_client_waiting_lock);
    }
}

/**
 * Garbage-collector thread.
 */
void *forkgc_thread (void *ignored)
{
    addr_buffer_t *ab;

    while ((1)) {
        pthread_mutex_lock(&g_gc_mutex);
        if (NULL == g_addr_buffer) {
            // Wait for somebody to come up with a set of addresses for us to
            // collect.
            g_gc_waiting = GC_WAITING_FOR_WORK;
            pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
            g_gc_waiting = GC_NOT_WAITING;
        }

        assert(g_addr_buffer);
        ab = g_addr_buffer;
        g_addr_buffer = NULL;
        if (g_waiting_collects >= g_forkscan_throttling_queue) {
            g_waiting_collects = 0;
            pthread_mutex_lock(&g_client_waiting_lock);
            pthread_cond_broadcast(&g_client_waiting_cond);
            pthread_mutex_unlock(&g_client_waiting_lock);
        } else g_waiting_collects = 0;

        pthread_mutex_unlock(&g_gc_mutex);

        garbage_collect(ab);
    }

    return NULL;
}

/**
 * Set the allocator for Forkscan to use: malloc, free, malloc_usable_size.
 */
void forkscan_set_allocator (void *(*alloc) (size_t),
                             void (*dealloc) (void *),
                             size_t (*usable_size) (void *))
{
    __forkscan_alloc = alloc;
    __forkscan_free = dealloc;
    __forkscan_usable_size = usable_size;
}

/**
 * Print program statistics to stdout.
 */
void forkgc_print_statistics ()
{
    char statm[256];
    size_t bytes_read;
    FILE *fp;

    fp = fopen("/proc/self/statm", "r");
    if (NULL == fp) {
        forkgc_fatal("Unable to open /proc/self/statm.\n");
    }
    bytes_read = fread(statm, 1, 255, fp);
    statm[statm[bytes_read - 1] == '\n'
          ? bytes_read - 1
          : bytes_read
          ] = '\0';
    fclose(fp);

    printf("statm: %s\n", statm);
    printf("fork-count: %zu\n", g_cleanup_counter);
    printf("scan-max: %zu\n", g_scan_max);
    printf("ave-fork-time: %d\n",
           g_cleanup_counter == 0 ? 0
           : ((int)(g_total_fork_time / g_cleanup_counter)));
    printf("wait-time: %zu\n", g_total_wait_time_ms);
}

__attribute__((destructor))
static void process_death ()
{
    if (child_pid > 0) {
        // There's still an outstanding child.  Kill it.
        kill(child_pid, 9);
    }
}
