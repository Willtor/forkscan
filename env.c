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

#include "env.h"
#include <stdlib.h>
#include "util.h"

#define DEFAULT_SWEEPER_THREADS 8

#define DEFAULT_THROTTLING_QUEUE 16
#define MAX_THROTTLING_QUEUE 32

#define MAX_PTRS_PER_THREAD (1024 * 1024)
#define MIN_PTRS_PER_THREAD 1024

static const char env_ptrs_per_thread[] = "FORKSCAN_PTRS_PER_THREAD";

static const char env_report_statistics[] = "FORKSCAN_REPORT_STATS";

static const char env_sweeper_threads[] = "FORKSCAN_SWEEPER_THREAD_COUNT";

static const char env_throttling_queue[] = "FORKSCAN_THROTTLING_QUEUE";

// # of ptrs a thread can "save up" before initiating a collection run.
// The number of pointers per thread should be a power of 2 because we use
// this number to do masking (to avoid the costly modulo operation).
int g_forkgc_ptrs_per_thread;

// Whether to report application statistics before the program terminates.
int g_forkgc_report_statistics;

// How many sweeper threads get spun up by the garbage collector.
int g_forkgc_sweeper_thread_count;

// How many collects can queue up before user threads get throttled.
int g_forkscan_throttling_queue;

/** Parse an integer from a string.  0 if val is NULL.
 */
static int get_int (const char *val, int default_val)
{
    if (NULL == val) return default_val;
    return atoi(val);
}

__attribute__((constructor))
static void env_init ()
{
    // Pointers per thread -- how many pointers a thread can track before a
    // collection run occurs.  This should be a power of 2.  To avoid
    // complicated numbers the environment variable,
    // FORKGC_PTRS_PER_THREAD, is multiplied by 1024 so that a user can
    // think in terms of small powers of 2.
    {
        int ptrs_per_thread;
        // Default is ~32000 pointers per thread, derived from trial data.
        ptrs_per_thread = get_int(getenv(env_ptrs_per_thread), 32);
        ptrs_per_thread *= 1024;

        // Round up to power of 2 bit trick:
        --ptrs_per_thread;
        ptrs_per_thread |= ptrs_per_thread >> 1;
        ptrs_per_thread |= ptrs_per_thread >> 2;
        ptrs_per_thread |= ptrs_per_thread >> 4;
        ptrs_per_thread |= ptrs_per_thread >> 8;
        ptrs_per_thread |= ptrs_per_thread >> 16;
        ++ptrs_per_thread;

        // Bounds-checking.
        if (ptrs_per_thread < MIN_PTRS_PER_THREAD) {
            forkscan_diagnostic("warning: %s = %s\n"
                                "  But min value is %d\n",
                                env_ptrs_per_thread,
                                getenv(env_ptrs_per_thread),
                                MIN_PTRS_PER_THREAD / 1024);
            ptrs_per_thread = MIN_PTRS_PER_THREAD;
        } else if (ptrs_per_thread > MAX_PTRS_PER_THREAD) {
            forkscan_diagnostic("warning: %s = %s\n"
                                "  But max value is %d\n",
                                env_ptrs_per_thread,
                                getenv(env_ptrs_per_thread),
                                MAX_PTRS_PER_THREAD / 1024);
            ptrs_per_thread = MAX_PTRS_PER_THREAD;
        }

        g_forkgc_ptrs_per_thread = ptrs_per_thread;
    }

    {
        int report_statistics;
        // Whether to report application statistics before the program exits.
        report_statistics = get_int(getenv(env_report_statistics), 0);
        if (report_statistics != 0) g_forkgc_report_statistics = 1;
    }

    {
        int sweeper_threads;
        sweeper_threads = get_int(getenv(env_sweeper_threads),
                                  DEFAULT_SWEEPER_THREADS);
        if (sweeper_threads <= 0) {
            sweeper_threads = 1;
        }
        if (sweeper_threads > MAX_SWEEPER_THREADS) {
            sweeper_threads = MAX_SWEEPER_THREADS;
        }
        g_forkgc_sweeper_thread_count = sweeper_threads;
    }

    {
        int throttling_queue;
        throttling_queue = get_int(getenv(env_throttling_queue),
                                   DEFAULT_THROTTLING_QUEUE);
        if (throttling_queue <= 0) {
            throttling_queue = 1;
        }
        if (throttling_queue > MAX_THROTTLING_QUEUE) {
            throttling_queue = MAX_THROTTLING_QUEUE;
        }
        g_forkscan_throttling_queue = throttling_queue;
    }
}
