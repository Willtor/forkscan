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

typedef struct unref_config_t unref_config_t;
typedef struct sweeper_work_t sweeper_work_t;

struct unref_config_t
{
    gc_data_t *gc_data;
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

#define WAITING_THRESHOLD 16

static gc_data_t *g_gc_data, *g_uncollected_data;
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

static pthread_mutex_t g_sweeper_mutex;
static pthread_cond_t g_sweeper_cond;
static volatile int g_sweepers_sleeping;
static volatile int g_sweepers_working;
static volatile int g_sweepers_remaining;
static sweeper_work_t g_sweeper_work[MAX_SWEEPER_THREADS];

static volatile size_t g_total_sweep_time = 0;

size_t rdtsc ()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    size_t ret = (size_t)(ts.tv_sec * (1000));
    ret += (size_t)(ts.tv_nsec / (1000 * 1000));
    return ret;
    /*
    unsigned int low, high;
    size_t ret;
    __asm__ ("rdtsc" : "=a" (low), "=d" (high));
    ret = ((size_t)high) << 32;
    return ret | low;
    */
}

static int unref_addr (thread_data_t *td, unref_config_t *unref_config,
                       int n, int max_depth)
{
    int i;
    size_t *addrs = unref_config->gc_data->addrs;
    size_t addr = addrs[n];
    assert(addr & 1);
    size_t *p = (size_t*)PTR_MASK(addr);
    int elements = (int)(MALLOC_USABLE_SIZE(p) / sizeof(size_t));
    int savings = 1;

    for (i = 0; i < elements; ++i) {
        size_t deep_addr = PTR_MASK(p[i]);
        if (deep_addr >= unref_config->min_val
            && deep_addr <= unref_config->max_val) {

            // Found a value within our range of addresses.  See if it's in
            // our set.  Also, null it.
            p[i] = 0;
            gc_data_t *gc_data = unref_config->gc_data;
            // Level 1 search: Find the page the address would be on.
            int v = binary_search(deep_addr, gc_data->minimap, 0,
                                  gc_data->n_minimap);
            // Level 2 search: Find the address within the page.
            int loc = binary_search(deep_addr, gc_data->addrs,
                                    v * (PAGESIZE / sizeof(size_t)),
                                    v == gc_data->n_minimap - 1
                                    ? gc_data->n_addrs
                                    : (v + 1) * (PAGESIZE / sizeof(size_t)));

            if (is_ref(gc_data, loc, deep_addr)) {

                // Found an apparent address.  Unreference it.
                __sync_fetch_and_sub(&gc_data->refs[loc], 1);
                int remaining_refs = gc_data->refs[loc];
                assert(remaining_refs >= 0);
                if (max_depth > 0
                    && remaining_refs == 0
                    && BCAS(&gc_data->addrs[loc], deep_addr, deep_addr | 1)) {

                    // Recurse, if depth permits.  We have a max depth
                    // parameter because in certain cases, the stack could
                    // overflow.
                    savings += unref_addr(td, unref_config, loc, max_depth - 1);
                }
            }
        }
    }

    free_t *f = (free_t*)p;
    f->next = td->free_list;
    td->free_list = f;
    //FREE(p); // Done with it!  Bam! ALEX
    return savings;
}

static void *address_range (sweeper_work_t *work)
{
    thread_data_t *td = threadscan_thread_get_td();
    gc_data_t *gc_data = work->unref_config->gc_data;
    int savings = 0;
    int i;
    for (i = work->range_begin; i < work->range_end; ++i) {
        size_t addr = gc_data->addrs[i];
        assert(addr != 0);
        assert(gc_data->refs[i] >= 0);
        if (0 == (addr & 1) && gc_data->refs[i] == 0) {
            if (BCAS(&gc_data->addrs[i], addr, addr | 1)) {
                savings += unref_addr(td, work->unref_config, i, 64);
            }
        }
    }
    return NULL;
}

#define ADDRS_PER_THREAD (128 * 1024)

static int find_unreferenced_nodes (gc_data_t *gc_data, queue_t *commq)
{
    unref_config_t unref_config;
    int sig_count;
    int addrs_per_thread;
    int i;
    size_t start, end;

    unref_config.gc_data = gc_data;
    unref_config.min_val = gc_data->addrs[0];
    unref_config.max_val = gc_data->addrs[gc_data->n_addrs - 1];

    g_signal_mode = MODE_SWEEP;
    g_received_signal = 0;
    start = rdtsc();
    sig_count = threadscan_proc_signal(SIGTHREADSCAN);
    if (sig_count == 0) return 0; // Program is about to exit.
    addrs_per_thread = gc_data->n_addrs / sig_count;

    // Set up work for the threads.
    for (i = 0; i < sig_count; ++i) {
        g_sweeper_work[i].unref_config = &unref_config;
        g_sweeper_work[i].range_begin = i * addrs_per_thread;
        g_sweeper_work[i].range_end = (i + 1) * addrs_per_thread;
    }
    g_sweeper_work[sig_count - 1].range_end = gc_data->n_addrs;
    g_sweepers_working = 0;
    g_sweepers_remaining = sig_count;

    while (g_received_signal < sig_count) pthread_yield();
    ++g_sweep_counter;

    // Wait for sweepers to complete.
    pthread_mutex_lock(&g_gc_mutex);
    if (g_gc_waiting == GC_DONT_WAIT_FOR_SWEEPERS) {
        // Wow!  Those sweepers finished before we even got here!  Nothing to
        // wait for.
    } else {
        g_gc_waiting = GC_WAITING_FOR_SWEEPERS;
        pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
    }
    g_gc_waiting = GC_NOT_WAITING;
    pthread_mutex_unlock(&g_gc_mutex);
    end = rdtsc();

    // Save time.
    size_t total_time = end > start ? end - start : 0;
    g_total_sweep_time += total_time;

    // Compact the list.
    int write_position = 0;
    int unfinished = 0;
    int saved = 0;
    for (i = 0; i < gc_data->n_addrs; ++i) {
        if (gc_data->addrs[i] & 1) ++saved;
        if ( !(gc_data->addrs[i] & 1)) {
            // Address doesn't have its low bit set: still alive.
            if (write_position != i) {
                gc_data->addrs[write_position] = gc_data->addrs[i];
                gc_data->refs[write_position] = gc_data->refs[i];
                gc_data->alloc_sz[write_position] = gc_data->alloc_sz[i];
            }
            if (gc_data->refs[write_position] == 0) {
                // Node didn't get free'd because the search depth was
                // exceeded.  We'll have to make another pass over the
                // nodes.
                ++unfinished;
            }
            ++write_position;
        }
    }
    gc_data->n_addrs = write_position;

    return 0; // unfinished; // ALEX
}

static void *sweeper_thread (void *arg)
{
    while ((1)) {
        // Wait for sweeping phase.
        pthread_mutex_lock(&g_sweeper_mutex);
        ++g_sweepers_sleeping;
        pthread_cond_wait(&g_sweeper_cond, &g_sweeper_mutex);
        --g_sweepers_sleeping;
        pthread_mutex_unlock(&g_sweeper_mutex);

        threadscan_fatal("Bad news, yo... bad news.\n");

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
    threadscan_fatal("Internal error: Unreachable code.\n");
    return NULL;
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
    g_frees_required = MAX_OF(list_count * 8, 8);

    assert(n_addrs != 0);

    // How many pages of memory are needed to store this many addresses?
    size_t pages_of_addrs = ((n_addrs * sizeof(size_t))
                             + PAGESIZE - sizeof(size_t)) / PAGESIZE;
    // How many pages of memory are needed to store the minimap?
    size_t pages_of_minimap = ((pages_of_addrs * sizeof(size_t))
                               + PAGESIZE - sizeof(size_t)) / PAGESIZE;
    // How many pages are needed to store the allocated size and reference
    // count arrays?
    size_t pages_of_count = ((n_addrs * sizeof(int))
                             + PAGESIZE - sizeof(int)) / PAGESIZE;
    // Total pages needed is the number of pages for the addresses, plus the
    // number of pages needed for the minimap, plus one (for the gc_data_t).
    char *p =
        (char*)threadscan_alloc_mmap_shared((pages_of_addrs     // addr array.
                                             + pages_of_minimap // minimap.
                                             + pages_of_count   // ref count.
                                             + pages_of_count   // alloc size.
                                             + 1)               // struct page.
                                            * PAGESIZE);

    // Perform assignments as offsets into the block that was bulk-allocated.
    size_t offset = 0;
    ret = (gc_data_t*)p;
    offset += PAGESIZE;

    ret->addrs = (size_t*)(p + offset);
    offset += pages_of_addrs * PAGESIZE;

    ret->minimap = (size_t*)(p + offset);
    offset += pages_of_minimap * PAGESIZE;

    ret->refs = (int*)(p + offset);
    offset += pages_of_count * PAGESIZE;

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
        ret->alloc_sz[i] = 0; //MALLOC_USABLE_SIZE((void*)ret->addrs[i]);
        //assert(ret->alloc_sz[i] > 0);
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
    gc_data_t *working_data;
    int sig_count;
    int pipefd[2];

    // Include the addrs from the last collection iteration.
    if (g_uncollected_data) {
        gc_data_t *tmp = g_uncollected_data;
        while (tmp->next) tmp = tmp->next;
        tmp->next = gc_data;
        gc_data = g_uncollected_data;
        g_uncollected_data = NULL;
    }

    working_data = aggregate_gc_data(gc_data);

    // Open a pipe for communication between parent and child.
    if (0 != pipe2(pipefd, O_DIRECT)) {
        threadscan_fatal("GC thread was unable to open a pipe.\n");
    }

    // Send out signals.  When everybody is waiting at the line, fork the
    // process for the snapshot.
    size_t start, end;
    g_signal_mode = MODE_SNAPSHOT;
    g_received_signal = 0;
    start = rdtsc();
    sig_count = threadscan_proc_signal(SIGTHREADSCAN);
    while (g_received_signal < sig_count) pthread_yield();
    child_pid = fork();

    if (child_pid == -1) {
        threadscan_fatal("Collection failed (fork).\n");
    } else if (child_pid == 0) {
        // Child: Scan memory, pass pointers back to the parent to free, pass
        // remaining pointers back, and exit.
        close(pipefd[PIPE_READ]);
        threadscan_child(working_data, pipefd[PIPE_WRITE]);
        close(pipefd[PIPE_WRITE]);
        exit(0);
    }

    ++g_cleanup_counter;
    close(pipefd[PIPE_WRITE]);
    end = rdtsc();
    g_total_fork_time += end - start;

    // Wait for the child to complete the scan.
    size_t bytes_scanned;
    if (sizeof(size_t) != read(pipefd[PIPE_READ], &bytes_scanned,
                               sizeof(size_t))) {
        threadscan_fatal("Failed to read from child.\n");
    }
    if (bytes_scanned > g_scan_max) g_scan_max = bytes_scanned;

    // Identify unreferenced memory and free it.
    int unfinished;
    int iters = 0;
    do {
        ++iters;
        unfinished = find_unreferenced_nodes(working_data, commq);
    } while (unfinished > 0);

    gc_data->n_addrs = 0;
    int i;
    for (i = 0; i < working_data->n_addrs; ++i) {
        if (g_uncollected_data == NULL) g_uncollected_data = gc_data;
        if (gc_data->n_addrs >= gc_data->capacity) {
            gc_data = gc_data->next;
            assert(gc_data != NULL);
            gc_data->n_addrs = 0;
        }
        gc_data->addrs[gc_data->n_addrs++] = working_data->addrs[i];
    }

    close(pipefd[PIPE_READ]);
    threadscan_alloc_munmap(working_data); // FIXME: ...

    // Free up unnecessary space.
    assert(gc_data);
    gc_data_t *tmp;
    if (gc_data->n_addrs) {
        tmp = gc_data;
        gc_data = gc_data->next;
        tmp->next = NULL;
    } else assert(NULL == g_uncollected_data);
    while (gc_data) {
        tmp = gc_data->next;
        threadscan_alloc_munmap(gc_data); // FIXME: Munmap is bad.
        gc_data = tmp;
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
void forkgc_initiate_collection (gc_data_t *gc_data)
{
    pthread_mutex_lock(&g_gc_mutex);
    ++g_waiting_collects;
    gc_data->next = g_gc_data;
    g_gc_data = gc_data;
    if (g_gc_waiting == GC_WAITING_FOR_WORK) {
        pthread_cond_signal(&g_gc_cond);
    }
    pthread_mutex_unlock(&g_gc_mutex);

    while (g_waiting_collects > WAITING_THRESHOLD) {
        //pthread_yield(); // FIXME: Yield.
        pthread_mutex_lock(&g_client_waiting_lock);
        if (g_waiting_collects > WAITING_THRESHOLD) {
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
    gc_data_t *gc_data;

    // FIXME: Warning: Fragile code knows the size of a pointer and a page.
    char *buffer = threadscan_alloc_mmap_shared(PAGESIZE * 9);
    queue_t *commq = (queue_t*)buffer;
    threadscan_queue_init(commq, (size_t*)&buffer[PAGESIZE], PAGESIZE);

    // Start thread pool.
    pthread_mutex_init(&g_sweeper_mutex, NULL);
    pthread_cond_init(&g_sweeper_cond, NULL);
    // FIXME: orig_* functions should get passed in.
    extern int (*orig_pthread_create) (pthread_t *, const pthread_attr_t *,
                                       void *(*) (void *), void *);
    int i;
    for (i = 0; i < g_forkgc_sweeper_thread_count; ++i) {
        pthread_t tid;
        if (0 != orig_pthread_create(&tid, NULL, sweeper_thread, NULL)) {
            threadscan_fatal("Unable to create sweeper thread.\n");
        }
    }

    while ((1)) {
        pthread_mutex_lock(&g_gc_mutex);
        if (NULL == g_gc_data) {
            // Wait for somebody to come up with a set of addresses for us to
            // collect.
            g_gc_waiting = GC_WAITING_FOR_WORK;
            pthread_cond_wait(&g_gc_cond, &g_gc_mutex);
            g_gc_waiting = GC_NOT_WAITING;
        }

        assert(g_gc_data);
        gc_data = g_gc_data;
        g_gc_data = NULL;
        if (g_waiting_collects > WAITING_THRESHOLD) {
            g_waiting_collects = 0;
            pthread_mutex_lock(&g_client_waiting_lock);
            pthread_cond_broadcast(&g_client_waiting_cond);
            pthread_mutex_unlock(&g_client_waiting_lock);
        } else g_waiting_collects = 0;

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
        threadscan_fatal("Unable to open /proc/self/statm.\n");
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
