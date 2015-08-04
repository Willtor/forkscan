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
#include <assert.h>
#include "alloc.h"
#include "child.h"
#include <malloc.h>
#include "proc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"

/****************************************************************************/
/*                                  Macros                                  */
/****************************************************************************/

#define BINARY_THRESHOLD 32

int is_ref (gc_data_t *gc_data, int loc, size_t cmp)
{
#ifdef DEEP_REFERENCES
    return gc_data->addrs[loc] == cmp
        || (gc_data->addrs[loc] < cmp
            && gc_data->addrs[loc] + gc_data->alloc_sz[loc] > cmp);
#else
    return gc_data->addrs[loc] == cmp;
#endif
}

static size_t n_bytes_searched;

/****************************************************************************/
/*                            Search utilities.                             */
/****************************************************************************/

static int iterative_search (size_t val, size_t *a, int min, int max)
{
    if (a[min] > val || min == max) return min;
    for ( ; min < max; ++min) {
        size_t cmp = a[min];
        if (cmp == val) return min;
        if (cmp > val) break;
    }
    return min - 1;
}

int binary_search (size_t val, size_t *a, int min, int max)
{
    while (max - min >= BINARY_THRESHOLD) {
        int mid = (max + min) / 2;
        size_t cmp = a[mid];
        if (cmp == val) return mid;

        if (cmp > val) max = mid;
        else min = mid;
    }

    return iterative_search(val, a, min, max);
}

static void do_search (size_t *mem, size_t range_size, gc_data_t *gc_data)
{
    size_t i;
    size_t min_ptr, max_ptr;

    min_ptr = gc_data->addrs[0];
    max_ptr = gc_data->addrs[gc_data->n_addrs - 1]
        + gc_data->alloc_sz[gc_data->n_addrs - 1] - sizeof(size_t);

    assert(min_ptr <= max_ptr);

    for (i = 0; i < range_size; ++i) {
        size_t cmp = PTR_MASK(mem[i]);
        // PTR_MASK catches pointers that have been hidden through overloading
        // the two low-order bits.

        if (cmp < min_ptr || cmp > max_ptr) continue; // Out-of-range.

        // Level 1 search: Find the page the address would be on.
        int v = binary_search(cmp, gc_data->minimap, 0,
                              gc_data->n_minimap);
        // Level 2 search: Find the address within the page.
        int loc = binary_search(cmp, gc_data->addrs,
                                v * (PAGESIZE / sizeof(size_t)),
                                v == gc_data->n_minimap - 1
                                ? gc_data->n_addrs
                                : (v + 1) * (PAGESIZE / sizeof(size_t)));
        if (is_ref(gc_data, loc, cmp)) {
            // It's a pointer somewhere into the allocated region of memory.
            ++gc_data->refs[loc];
        }
#ifndef NDEBUG
        else {
            int loc2 = binary_search(cmp, gc_data->addrs,
                                     0, gc_data->n_addrs);
            // FIXME: Assert does not catch all bad cases.
            assert(gc_data->addrs[loc2] != cmp);
        }
#endif
    }
}

static void search_range (mem_range_t *range, gc_data_t *gc_data)
{
    size_t *mem;

    assert(range);

    mem = (size_t*)range->low;
    do_search(mem, (range->high - range->low) / sizeof(size_t), gc_data);
    n_bytes_searched += range->high - range->low;
    return;
}

/**
 * Determine whether a "path" is the location of the given library.
 *
 * @return 1 if it is the library location, zero otherwise.
 */
static int is_lib (const char *library, const char *path)
{
    if ('/' != path[0]) return 0;

    int len = strlen(library);
    ++path;
    while (1) {
        if ('\0' == path[0]) return 0;
        ++path;
        if (0 == strncmp(library, path, len)
            && ('.' == path[len] || '-' == path[len])) {
            return 1;
        }
        while ('\0' != path[0] && '/' != path[0]) ++path;
    }
}

static int scan_memory (void *p,
                        size_t low,
                        size_t high,
                        const char *bits,
                        const char *path)
{
    // Decide whether this is a region we want to look at.

    if (bits[1] == '-') {
        // Memory is not writable.
        return 1;
    }
    if (bits[2] == 'x') {
        // Executable memory.  This is only writable if it is a
        // relocation table, so don't worry about it.
        return 1;
    }
    if (low == high) {
        // The wha?  But, you know... it happens.
        return 1;
    }
    if (low <= (size_t)stderr->_lock && high >= (size_t)stderr->_lock) {
        // FIXME: This is bad!  Bad, bad, bad!  What is a general way
        // to find the stuff statically allocated by libc?  stderr,
        // declared right next to its lock, lives in a different
        // section. (See: libio/stdfiles.c in glibc)
        return 1;
    }
    if (is_lib("libc", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (is_lib("libdl", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (is_lib("libthreadscan", path)) {
        // Part of the threadscan module memory.  It's clean.
        return 1;
    }
    if (bits[3] == 's') {
        // Shared writable memory.  This is probably the commq.
        return 1;
    }
    if (0 == memcmp(path, "[stack:", 7)) {
        // Our stack.  Don't check that.  Note: This is not one of the other
        // threads' stacks because in the child process, there is only one
        // thread.
        return 1;
    }

    /* It looks like we've applied all of the criteria and have found a range
       that we want to scan, right?  Not quite.  What about memory allocated
       by _this_ module?  Unfortunately, we cannot apply a simple comparison
       of this range with any specific memory we've mmap'd.
       The /proc/<pid>/maps file consolidates ranges if it can so we
       (potentially) have a range that needs to be turned into Swiss Cheese of
       sub-ranges that we actually want to look at. */

    gc_data_t *gc_data = (gc_data_t*)p;
    mem_range_t big_range = { low, high };
    while (big_range.low != big_range.high) {
        mem_range_t next = threadscan_alloc_next_subrange(&big_range);
        if (next.low != next.high) {
            // This is a region of memory we want to scan.
            search_range(&next, gc_data);
        }
    }

    return 1;
}

void threadscan_child (gc_data_t *gc_data, int fd)
{
    // Scan memory for references.
    n_bytes_searched = 0;
    threadscan_proc_map_iterate(scan_memory, (void*)gc_data);
    threadscan_diagnostic("Scanned %d KB.\n", n_bytes_searched / 1024);
    if (sizeof(size_t) != write(fd, &n_bytes_searched, sizeof(size_t))) {
        threadscan_fatal("Failed to write to parent.\n");
    }
}
