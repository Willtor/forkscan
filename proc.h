/*
Copyright (c) 2015 Forkscan authors

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

#ifndef _PROC_H_
#define _PROC_H_

#include <stddef.h>
#include "util.h"

/**
 * Return the list of thread metadata objects for all the threads known to
 * ForkGC.
 */
thread_list_t *forkgc_proc_get_thread_list ();

/**
 * Given an address, find the bounds of the stack on which it lives.  The
 * mem_range is populated based on the results.
 */
void forkgc_proc_stack_from_addr (mem_range_t *mem_range, size_t addr);

/**
 * Iterate over the memory map, applying *f to each range.  *f can cause this
 * function to exit early by returning 0.
 */
void forkgc_proc_map_iterate (int (*f) (void *arg,
                                            size_t begin,
                                            size_t end,
                                            const char *bits,
                                            const char *path),
                                  void *user_arg);

/****************************************************************************/
/*                             Per-thread data                              */
/****************************************************************************/

/**
 * Threads call this to register themselves with ForkGC when they start.
 */
void forkgc_proc_add_thread_data (thread_data_t *td);

/**
 * Threads call this when they are going away.  It unregisters them with the
 * ForkGC.
 */
void forkgc_proc_remove_thread_data (thread_data_t *td);

/**
 * Send a signal to all threads in the process (except the calling thread)
 * using pthread_kill().
 */
int forkgc_proc_signal_all_except (int sig, thread_data_t *except);

/**
 * Send a signal to all threads in the process using pthread_kill().
 */
int forkgc_proc_signal (int sig);

/**
 * Wait for all threads that are trying to help out to discover the
 * current timestamp.
 */
void forkgc_proc_wait_for_timestamp (size_t curr);

#endif // !defined _PROC_H_
