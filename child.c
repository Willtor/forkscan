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
#include <assert.h>
#include "alloc.h"
#include "child.h"
#include <jemalloc/jemalloc.h>
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

#define MAX_TRACE_DEPTH 64
#define BINARY_THRESHOLD 32
#define MAX_RANGES 2048
#define MAX_RANGE_SIZE (256 * 1024 * 1024)
#define MAX_CHILDREN 8
#define MEMORY_THRESHOLD (1024 * 1024 * 128)

typedef struct trace_stats_t trace_stats_t;

struct trace_stats_t
{
    size_t min, max;
};

static mem_range_t g_ranges[MAX_RANGES];
static int g_n_ranges;
static size_t g_bytes_to_scan;

int is_ref (gc_data_t *gc_data, int loc, size_t cmp)
{
    assert(loc >= 0);
    return PTR_MASK(gc_data->addrs[loc]) == cmp;
}

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

/**
 * Return the index to the location in the address list closest to val without
 * exceeding it.  The bounds on the return value are [0, num_addrs).
 */
static int addr_find (size_t val, gc_data_t *gc_data)
{
    // Level 1 search: Find the page the address would be on.
    int v = binary_search(val, gc_data->minimap, 0,
                          gc_data->n_minimap);
    // Level 2 search: Find the address within the page.
    int loc = binary_search(val, gc_data->addrs,
                            v * (PAGESIZE / sizeof(size_t)),
                            v == gc_data->n_minimap - 1
                            ? gc_data->n_addrs
                            : (v + 1) * (PAGESIZE / sizeof(size_t)));
    return loc;
}

/**
 * Search a node for references to other nodes in our address list.  If any
 * exist, search those nodes for references, recursively, up to a maximum
 * "depth."  If there are references that are deeper than this function is
 * willing to go, return 1 to indicate a cut-off.  Return 0 otherwise.
 */
static inline int recursive_trace (gc_data_t *gc_data,
                                   trace_stats_t *ts,
                                   int idx, int depth)
{
    int cutoff = 0;

    size_t *ptr = (size_t*)PTR_MASK(gc_data->addrs[idx]);
    size_t n_vals = MALLOC_USABLE_SIZE(ptr) / sizeof(size_t);
    size_t i;

    for (i = 0; i < n_vals; ++i) {
        size_t val = PTR_MASK(ptr[i]);
        if (val < ts->min || val > ts->max) continue;
        int loc = addr_find(val, gc_data);
        if (is_ref(gc_data, loc, val)) {
            // Found a reference.  That ties val to a root somewhere outside
            // the tracking region.  Mark it as not free'able if it isn't
            // already.
            size_t addr = gc_data->addrs[loc];
            if (addr & 0x2) {
                // Already traced.
                continue;
            }
            if (depth <= 0) {
                // We can't trace it, alas.  But we should mark it as seen.
                if (!(addr & 1)) {
                    BCAS(&gc_data->addrs[loc], addr, addr | 0x1);
                }
                cutoff = 1;
            } else {
                // Recursively trace it.
                if (!BCAS(&gc_data->addrs[loc], addr, addr | 0x3)) {
                    // Somebody else got to it first.  That's okay.
                    continue;
                }
                cutoff |= recursive_trace(gc_data, ts, loc, depth - 1);
            }
        }
    }
    return cutoff;
}

/**
 * Perform a trace on the tracked addresses.  This is only necessary if the
 * cutoff was reached by any of the sub-processes.
 */
static void cleanup_trace (gc_data_t *gc_data)
{
    trace_stats_t ts;
    int i;
    ts.min = PTR_MASK(gc_data->addrs[0]);
    ts.max = PTR_MASK(gc_data->addrs[gc_data->n_addrs - 1]);

    for (i = 0; i < gc_data->n_addrs; ++i) {
        size_t addr = gc_data->addrs[i];
        if (!(addr & 0x1) || (addr & 0x2)) {
            // It is not marked as being connected to a root, or it has been
            // marked and a sub-trace has been run.  Either way, nothing for
            // us to do here.
            continue;
        }
        if (!BCAS(&gc_data->addrs[i], addr, addr | 0x3)) {
            // Another thread got to it before we did.  That's okay, too.
            continue;
        }
        recursive_trace(gc_data, &ts, i, MAX_TRACE_DEPTH);
    }
}

/**
 * Search through the given chunk of memory looking for references into the
 * memory we're tracking from outside the memory we're tracking.  These roots
 * will later be used as a basis for determining reachability of the rest of
 * the nodes.
 */
static void find_roots (size_t *mem, size_t range_size, gc_data_t *gc_data)
{
    size_t i;
    int guarded_idx;
    size_t guarded_addr;
    trace_stats_t ts;

    ts.min = PTR_MASK(gc_data->addrs[0]);
    ts.max = PTR_MASK(gc_data->addrs[gc_data->n_addrs - 1]);

    assert(ts.min <= ts.max);

    // Figure out where to start the search.  Any memory is a potential ptr
    // to one of our addresses, but we avoid searching memory we're tracking
    // because that will be done during mark and sweep.
    i = 0;
    guarded_idx = addr_find((size_t)mem, gc_data);
    guarded_addr = PTR_MASK(gc_data->addrs[guarded_idx]);
    if (guarded_addr <= (size_t)mem) {
        size_t sz = MALLOC_USABLE_SIZE((void*)guarded_addr);
        assert(sz > 0);
        if ((size_t)mem < guarded_addr + sz) {
            size_t diff = guarded_addr + sz - PTR_MASK((size_t)mem);
            i += ((int)diff) / sizeof(size_t);
        }
    }
    if (gc_data->n_addrs - 1 > guarded_idx) {
        ++guarded_idx;
        guarded_addr = PTR_MASK(gc_data->addrs[guarded_idx]);
    }

    for ( ; i < range_size; ++i) {
        if (guarded_addr == (size_t)(&mem[i])) {
            // Skip a little.  This area will be searched during the mark-and-
            // sweep phase.
            size_t sz = MALLOC_USABLE_SIZE((void*)guarded_addr);
            assert(sz > 0);
            i += ((int)sz) / sizeof(size_t) - 1;
            if (gc_data->n_addrs - 1 > guarded_idx) {
                ++guarded_idx;
                guarded_addr = PTR_MASK(gc_data->addrs[guarded_idx]);
            }
            continue;
        }

        size_t cmp = PTR_MASK(mem[i]);
        // PTR_MASK catches pointers that have been hidden through overloading
        // the two low-order bits.

        if (cmp < ts.min || cmp > ts.max) continue; // Out-of-range.

        int loc = addr_find(cmp, gc_data);
        if (is_ref(gc_data, loc, cmp)) {
            // It's a pointer somewhere into the allocated region of memory.
            size_t addr = gc_data->addrs[loc];
            if ((addr & 0x3) == 0x3) continue; // Already checked.

            if (BCAS(&gc_data->addrs[loc], addr, addr | 0x3)) {
                // Do recursive search of this node to look for deeper nodes.
                if (0 != recursive_trace(gc_data, &ts, loc, MAX_TRACE_DEPTH)) {
                    // Reached a cutoff -- there are unsearched nodes in the
                    // address list.
                    if (0 == gc_data->cutoff_reached) {
                        gc_data->cutoff_reached = 1;
                    }
                }
            }
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
    find_roots(mem, (range->high - range->low) / sizeof(size_t), gc_data);
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

static int collect_ranges (void *p,
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
        // Part of the ForkGC module memory.  It's clean.
        return 1;
    }
    if (is_lib("libdl", path)) {
        // Part of the ForkGC module memory.  It's clean.
        return 1;
    }
    if (is_lib("libforkgc", path)) {
        // Part of the ForkGC module memory.  It's clean.
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

    mem_range_t big_range = { low, high };
    while (big_range.low != big_range.high) {
        mem_range_t next = forkgc_alloc_next_subrange(&big_range);
        if (next.low != next.high) {
            // This is a region of memory we want to scan.
            g_bytes_to_scan += next.high - next.low;
            while (next.low + MAX_RANGE_SIZE < next.high) {
                g_ranges[g_n_ranges] = next;
                g_ranges[g_n_ranges].high = next.low + MAX_RANGE_SIZE;
                next.low += MAX_RANGE_SIZE;
                ++g_n_ranges;
            }
            g_ranges[g_n_ranges++] = next;
            if (g_n_ranges >= MAX_RANGES) {
                forkgc_fatal("Too many memory ranges.\n");
            }
        }
    }

    return 1;
}

void forkgc_child (gc_data_t *gc_data, int fd)
{
    // Scan memory for references.
    g_bytes_to_scan = 0;
    forkgc_proc_map_iterate(collect_ranges, NULL);
    gc_data->completed_children = 0;
    gc_data->cutoff_reached = 0;

    int n_siblings = MIN_OF(MAX_CHILDREN,
                            g_bytes_to_scan / MEMORY_THRESHOLD);
    n_siblings = MIN_OF(n_siblings, g_n_ranges);
    // n_siblings could be zero, in which case we don't fork.
    int child_id = 0;
    for (child_id = 0; child_id < n_siblings; ++child_id) {
        if (fork() == 0) break;
    }
    ++n_siblings;

    // Scan this child's ranges.
    int range_block = g_n_ranges / n_siblings;
    int max_range = child_id == n_siblings - 1
        ? g_n_ranges : (child_id + 1) * range_block;
    int i;
    for (i = child_id * range_block; i < max_range; ++i) {
        search_range(&g_ranges[i], gc_data);
    }

    if (n_siblings - 1
        == __sync_fetch_and_add(&gc_data->completed_children, 1)) {
        // Last process out.  See if there are any remaining untraced refs.
        if (gc_data->cutoff_reached > 0) {
            fprintf(stderr, "Doing extra cleanup.\n");
            cleanup_trace(gc_data);
        }

        // Alert the uber parent.
        if (sizeof(size_t) != write(fd, &g_bytes_to_scan, sizeof(size_t))) {
            forkgc_fatal("Failed to write to parent.\n");
        }
    }
}
