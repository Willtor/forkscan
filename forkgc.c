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
#include "forkgc.h"
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
typedef struct sweeper_work_t sweeper_work_t;

struct unref_config_t
{
    addr_buffer_t *ab;
    size_t min_val, max_val;
};

struct sweeper_work_t
{
    unref_config_t *unref_config;
    int range_begin, range_end;
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
static volatile enum { MODE_SNAPSHOT, MODE_SWEEP } g_signal_mode;
static volatile size_t g_cleanup_counter;
static volatile size_t g_sweep_counter;
static enum { GC_NOT_WAITING,
              GC_WAITING_FOR_WORK, 
              GC_WAITING_FOR_SWEEPERS,
              GC_DONT_WAIT_FOR_SWEEPERS } g_gc_waiting = GC_WAITING_FOR_WORK;
static size_t g_scan_max;
static double g_total_fork_time;
static pid_t child_pid;

static volatile int g_sweepers_working;
static volatile int g_sweepers_remaining;
static sweeper_work_t g_sweeper_work[MAX_SWEEPER_THREADS];

static volatile size_t g_total_sweep_time = 0;

static void address_range (sweeper_work_t *work)
{
    addr_buffer_t *ab = work->unref_config->ab;
    free_t *free_list = NULL;
    int free_list_length = 0;
    int i;
    for (i = work->range_begin; i < work->range_end; ++i) {
        size_t addr = ab->addrs[i];
        assert(addr != 0);
        if (0 == (addr & 1)) {
            // Memory to be freed.
            free_t *ptr = (free_t*)addr;
#ifndef NDEBUG
            memset(ptr, 0xBB, MALLOC_USABLE_SIZE(ptr));
#endif
            ptr->next = free_list;
            free_list = ptr;
            ++free_list_length;
            if (free_list_length >= MAX_FREE_LIST_LENGTH) {
                // Free list has gotten long enough.  We give a free list a
                // certain size that represents a sizable chunk for a thread
                // to grab and gradually free.
                forkgc_util_push_free_list(free_list);
                free_list = NULL;
                free_list_length = 0;
            }
        }
    }
    if (free_list) {
        forkgc_util_push_free_list(free_list);
    }
}

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

static addr_buffer_t *aggregate_addrs (addr_buffer_t *data_list)
{
    addr_buffer_t *ret, *tmp;
    size_t n_addrs = 0;
    int list_count = 0;

    tmp = data_list;
    do {
        n_addrs += tmp->n_addrs;
        ++list_count;
    } while ((tmp = tmp->next));
    g_frees_required = MAX_OF(list_count * 8, 8);

    assert(n_addrs != 0);

    ret = forkscan_make_aggregate_buffer(n_addrs);
    ret->n_addrs = n_addrs;

    // Copy the addresses over.
    char *dest = (char*)ret->addrs;
    tmp = data_list;
    do {
        memcpy(dest, tmp->addrs, tmp->n_addrs * sizeof(size_t));
        dest += tmp->n_addrs * sizeof(size_t);
    } while ((tmp = tmp->next));

    // Sort the addresses and generate the minimap for the scanner.
    forkgc_util_sort(ret->addrs, ret->n_addrs);
    assert_monotonicity(ret->addrs, ret->n_addrs);
    generate_minimap(ret);

    return ret;
}

static void garbage_collect (addr_buffer_t *ab)
{
    addr_buffer_t *working_data;
    int sig_count;
    int pipefd[2];

    // Include the addrs from the last collection iteration.
    if (g_uncollected_data) {
        addr_buffer_t *tmp = g_uncollected_data;
        while (tmp->next) tmp = tmp->next;
        tmp->next = ab;
        ab = g_uncollected_data;
        g_uncollected_data = NULL;
    }

    working_data = aggregate_addrs(ab);

    // Open a pipe for communication between parent and child.
    if (0 != pipe2(pipefd, O_DIRECT)) {
        forkgc_fatal("GC thread was unable to open a pipe.\n");
    }

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    size_t start, end;
    g_signal_mode = MODE_SNAPSHOT;
    g_received_signal = 0;
    start = forkscan_rdtsc();
    sig_count = forkgc_proc_signal(SIGFORKGC);
    while (g_received_signal < sig_count) pthread_yield();
    child_pid = fork();

    if (child_pid == -1) {
        forkgc_fatal("Collection failed (fork).\n");
    } else if (child_pid == 0) {
        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        close(pipefd[PIPE_READ]);
        forkgc_child(working_data, pipefd[PIPE_WRITE]);
        close(pipefd[PIPE_WRITE]);
        exit(0);
    }

    ++g_cleanup_counter;
    close(pipefd[PIPE_WRITE]);
    end = forkscan_rdtsc();
    g_total_fork_time += end - start;

    // Wait for the child to complete the scan.
    size_t bytes_scanned;
    if (sizeof(size_t) != read(pipefd[PIPE_READ], &bytes_scanned,
                               sizeof(size_t))) {
        forkgc_fatal("Failed to read from child.\n");
    }
    if (bytes_scanned > g_scan_max) g_scan_max = bytes_scanned;

    // Make the unreferenced nodes, here, available for free'ing.
    forkscan_buffer_push_back(working_data);

    // Pull out all the externally-referenced addresses so they can be
    // included in the next collection round.
    ab->n_addrs = 0;
    int i;
    for (i = 0; i < working_data->n_addrs; ++i) {
        if (g_uncollected_data == NULL) g_uncollected_data = ab;
        if ((working_data->addrs[i] & 0x1) == 0) continue;
        ab->addrs[ab->n_addrs++] = working_data->addrs[i];
        if (ab->n_addrs >= ab->capacity) {
            ab = ab->next;
            assert(ab != NULL);
            ab->n_addrs = 0;
        }
    }

    forkscan_buffer_unref_buffer(working_data);

    close(pipefd[PIPE_READ]);

    // Free up unnecessary space.
    assert(ab);
    addr_buffer_t *tmp;
    if (ab->n_addrs) {
        tmp = ab;
        ab = ab->next;
        tmp->next = NULL;
    } else assert(NULL == g_uncollected_data);
    while (ab) {
        tmp = ab->next;
        forkscan_release_buffer(ab);
        ab = tmp;
    }
}

/****************************************************************************/
/*                            Exported functions                            */
/****************************************************************************/

/**
 * Acknowledge the signal sent by the GC thread and perform any work required.
 */
void forkgc_acknowledge_signal ()
{
    if (g_signal_mode == MODE_SNAPSHOT) {
        size_t old_counter;
        jmp_buf env; // Spilled registers.

        // Acknowledge the signal and wait for the snapshot to complete.
        old_counter = g_cleanup_counter;
        setjmp(env);
        __sync_fetch_and_add(&g_received_signal, 1);
        while (old_counter == g_cleanup_counter) pthread_yield();
    } else {
        // FIXME: No longer sweeping.
        assert(g_signal_mode == MODE_SWEEP);
        size_t old_counter = g_sweep_counter;
        __sync_fetch_and_add(&g_received_signal, 1);
        while (old_counter == g_sweep_counter) pthread_yield();

        // Perform sweep.
        int id = __sync_fetch_and_add(&g_sweepers_working, 1);
        address_range(&g_sweeper_work[id]);
        int done = __sync_fetch_and_sub(&g_sweepers_remaining, 1) - 1;

        // See if we're the last one.
        if (done == 0) {
            // Last one out.  Signal the GC thread.
            pthread_mutex_lock(&g_gc_mutex);
            assert(g_gc_waiting != GC_WAITING_FOR_WORK);
            if (g_gc_waiting == GC_WAITING_FOR_SWEEPERS) {
                pthread_cond_signal(&g_gc_cond);
            } else {
                g_gc_waiting = GC_DONT_WAIT_FOR_SWEEPERS;
            }
            pthread_mutex_unlock(&g_gc_mutex);
        }
    }
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

    while (g_waiting_collects > g_forkscan_throttling_queue) {
        //pthread_yield(); // FIXME: Yield?
        pthread_mutex_lock(&g_client_waiting_lock);
        if (g_waiting_collects > g_forkscan_throttling_queue) {
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
        if (g_waiting_collects > g_forkscan_throttling_queue) {
            g_waiting_collects = 0;
            pthread_mutex_lock(&g_client_waiting_lock);
            pthread_cond_broadcast(&g_client_waiting_cond);
            pthread_mutex_unlock(&g_client_waiting_lock);
        } else g_waiting_collects = 0;

        pthread_mutex_unlock(&g_gc_mutex);

#ifndef NDEBUG
        int n = 1;
        addr_buffer_t *tmp = ab;
        int b_unfreed_data = 0;
        while (NULL != (tmp = tmp->next)) ++n;
        free_t *f = forkgc_util_pop_free_list();
        if (NULL != f) {
            b_unfreed_data = 1;
            forkgc_util_push_free_list(f);
        }
        forkgc_diagnostic("%d collects waiting%s.\n", n,
                          b_unfreed_data != 0
                          ? " (unfreed data)"
                          : "");
#endif

        garbage_collect(ab);
    }

    return NULL;
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
    printf("ave-sweep-time: %zu\n", g_sweep_counter > 0
           ? g_total_sweep_time / g_sweep_counter : 0);
}

__attribute__((destructor))
static void process_death ()
{
    if (child_pid > 0) {
        // There's still an outstanding child.  Kill it.
        kill(child_pid, 9);
    }
}
