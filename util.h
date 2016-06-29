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

#ifndef _UTIL_H_
#define _UTIL_H_

#include <pthread.h>
#include "queue.h"
#include <signal.h>

/****************************************************************************/
/*                         Defines, typedefs, etc.                          */
/****************************************************************************/

#define MALLOC(sz) je_malloc(sz)
#define FREE(ptr) je_free(ptr)
#define MALLOC_USABLE_SIZE(ptr) je_malloc_usable_size(ptr)

#define FOREACH_IN_THREAD_LIST(td, tl) do { \
    pthread_mutex_lock(&(tl)->lock);        \
    (td) = (tl)->head;                      \
    while (NULL != (td)) {

#define ENDFOREACH_IN_THREAD_LIST(td, tl) (td) = (td)->next; }     \
    pthread_mutex_unlock(&(tl)->lock);                             \
    } while (0)

#define FOREACH_BREAK_THREAD_LIST(tl) do {      \
    pthread_mutex_unlock(&(tl)->lock);          \
    } while (0)

#define PTR_MASK(v) ((v) & ~3) // Mask off the low two bits.

#define PAGESIZE ((size_t)0x1000)

#define PAGEALIGN(addr) ((addr) & ~(PAGESIZE - 1))

#define MIN_OF(a, b) ((a) < (b) ? (a) : (b))
#define MAX_OF(a, b) ((a) < (b) ? (b) : (a))

#define BCAS(ptr, compare, swap)                        \
    __sync_bool_compare_and_swap((ptr), (compare), (swap))

#define _TIMESTAMP_MASK 0x7FFFFFFFFFFFFFFF
#define _TIMESTAMP_FLAG 0x8000000000000000

#define TIMESTAMP(field) ((field) & _TIMESTAMP_MASK)
#define TIMESTAMP_RAISE_FLAG(field) ((field) | _TIMESTAMP_FLAG)
#define TIMESTAMP_IS_ACTIVE(field) ((field) & _TIMESTAMP_FLAG)
#define TIMESTAMP_SET_ACTIVE(field) TIMESTAMP_RAISE_FLAG(field)

typedef struct mem_range_t mem_range_t;

typedef struct free_t free_t;

typedef struct thread_data_t thread_data_t;

typedef struct thread_list_t thread_list_t;

/****************************************************************************/
/*                 Memory range data for write protection.                  */
/****************************************************************************/

struct mem_range_t {
    size_t low;
    size_t high;
};

/****************************************************************************/
/*                       Storage for per-thread data.                       */
/****************************************************************************/

struct free_t {
    free_t *next;
};

struct thread_data_t {

    // User parameters for creating a new thread.
    void *(*user_routine) (void *);
    void *user_arg;

    // Thread metadata fields.
    thread_data_t *next;      // Linked list of thread metadata.
    pthread_t self;           // That's me!
    char *user_stack_low;     // Low address on the user stack.
    char *user_stack_high;    // Actually, just the high address to lock.

    int stack_is_ours;        // Whether ForkGC allocated the stack.
    int is_active;            // The thread is running user code.

    queue_t ptr_list;         // Local list of pointers to be collected.
    free_t *free_list;

    size_t local_timestamp;
    int times_without_update;

    mem_range_t local_block;  // Non-stack memory local to this thread.

    // Reference count prevents premature free'ing of the structure while
    // other threads are looking at it.
    int ref_count;
};

struct thread_list_t {
    thread_data_t *head;
    pthread_mutex_t lock;
    unsigned int count; // Number of threads.
};

thread_data_t *forkgc_util_thread_data_new ();
void forkgc_util_thread_data_decr_ref (thread_data_t *td);
void forkgc_util_thread_data_free (thread_data_t *td);
void forkgc_util_thread_data_cleanup (pthread_t tid);

void forkgc_util_thread_list_init (thread_list_t *tl);
void forkgc_util_thread_list_add (thread_list_t *tl, thread_data_t *td);
void forkgc_util_thread_list_remove (thread_list_t *tl, thread_data_t *td);
thread_data_t *forkgc_util_thread_list_find (thread_list_t *tl,
                                             size_t addr);
void forkgc_util_push_free_list (free_t *free_list);
free_t *forkgc_util_pop_free_list ();
void forkscan_util_free_ptrs (thread_data_t *td);

/****************************************************************************/
/*                              I/O functions.                              */
/****************************************************************************/

int forkgc_diagnostic (const char *format, ...);
void forkgc_fatal (const char *format, ...);

/****************************************************************************/
/*                              Sort utility.                               */
/****************************************************************************/

void forkgc_util_randomize (size_t *addrs, int n);
void forkgc_util_sort (size_t *a, int length);
int forkgc_util_compact (size_t *a, int length);

/**
 * Get a timestamp in ms.
 */
size_t forkscan_rdtsc ();

#endif // !defined _UTIL_H_
