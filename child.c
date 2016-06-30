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

#define MAX_MARK_AND_SWEEP_RANGES 4096
#define LOOKASIDE_SZ 0x2000
#define MAX_TRACE_DEPTH 32
#define BINARY_THRESHOLD 32
#define MAX_RANGE_SIZE (4 * 1024 * 1024)
#define MAX_CHILDREN 16
#define MEMORY_THRESHOLD (1024 * 1024 * 128)

typedef struct trace_stats_t trace_stats_t;

struct trace_stats_t
{
    size_t min, max;
};

static mem_range_t g_ranges[MAX_MARK_AND_SWEEP_RANGES];
static int g_n_ranges;
static size_t g_bytes_to_scan;
static size_t g_lookaside_list[LOOKASIDE_SZ];
static int g_lookaside_count = 0;

int is_ref (addr_buffer_t *ab, int loc, size_t cmp)
{
    assert(loc >= 0);
    return PTR_MASK(ab->addrs[loc]) == cmp;
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
static int addr_find (size_t val, addr_buffer_t *ab)
{
    // Level 1 search: Find the page the address would be on.
    int v = binary_search(val, ab->minimap, 0,
                          ab->n_minimap);
    // Level 2 search: Find the address within the page.
    int loc = binary_search(val, ab->addrs,
                            v * (PAGESIZE / sizeof(size_t)),
                            v == ab->n_minimap - 1
                            ? ab->n_addrs
                            : (v + 1) * (PAGESIZE / sizeof(size_t)));
    return loc;
}

static int addr_find_hint (size_t val, addr_buffer_t *ab, int hint)
{
    size_t cmp = ab->addrs[hint];
    if (val <= cmp) return hint;

    // FIXME: Should parameterize cache-line size, etc.  Not good code.
    size_t cmp_addr = (size_t)&ab->addrs[hint];
    cmp_addr &= ~(size_t)63;
    cmp_addr += 56;
    int remaining_line =
        (cmp_addr - (size_t)&ab->addrs[hint]) / sizeof(size_t);
    remaining_line += 8;

    int i;
    for (i = 0; i < remaining_line; ++i) {
        hint = MIN_OF(ab->n_addrs, hint + 1);
        cmp = ab->addrs[hint];
        if (val <= cmp) return hint;
    }

    return addr_find(val, ab);
}

static inline int recursive_mark (size_t addr,
                                  addr_buffer_t *ab,
                                  trace_stats_t *ts,
                                  int depth)
{
    size_t *ptr = (size_t*)PTR_MASK(addr);
    size_t n_vals = MALLOC_USABLE_SIZE(ptr) / sizeof(size_t);
    size_t i;
    int ret = 0;

    for (i = 0; i < n_vals; ++i) {
        size_t val = PTR_MASK(ptr[i]);
        if (val < ts->min || val > ts->max) continue;
        int loc = addr_find(val, ab);
        if (is_ref(ab, loc, val)) {
            // Found a hit inside our pool.
            size_t target = ab->addrs[loc];
            if (target & 0x2) {
                // Already recursively searched.
                continue;
            }

            if (target & 0x3 && depth < 1) {
                // Already marked and we can't go any deeper.
                continue;
            }

            if (depth < 1) {
                // Not marked, but we can't go any deeper.  Mark it and
                // continue.
                BCAS(&ab->addrs[loc], target, target | 0x1);
                ret = 1;
            }

            if (BCAS(&ab->addrs[loc], target, target | 0x3)) {
                // We will do a recursive search.
                ret |= recursive_mark(PTR_MASK(addr), ab, ts, depth - 1);
            }
        }
    }

    return ret;
}

/**
 * Search a node for references to other nodes in our address list.  If any
 * exist, search those nodes for references, recursively, up to a maximum
 * "depth."  If there are references that are deeper than this function is
 * willing to go, return 1 to indicate a cut-off.  Return 0 otherwise.
 */
static inline int recursive_trace (addr_buffer_t *ab,
                                   trace_stats_t *ts,
                                   int idx, int depth)
{
    int cutoff = 0;

    size_t *ptr = (size_t*)PTR_MASK(ab->addrs[idx]);
    size_t n_vals = MALLOC_USABLE_SIZE(ptr) / sizeof(size_t);
    size_t i;

    for (i = 0; i < n_vals; ++i) {
        size_t val = PTR_MASK(ptr[i]);
        if (val < ts->min || val > ts->max) continue;
        int loc = addr_find(val, ab);
        if (is_ref(ab, loc, val)) {
            // Found a reference.  That ties val to a root somewhere outside
            // the tracking region.  Mark it as not free'able if it isn't
            // already.
            size_t addr = ab->addrs[loc];
            if (addr & 0x2) {
                // Already traced.
                continue;
            }
            if (depth <= 0) {
                // We can't trace it, alas.  But we should mark it as seen.
                if (!(addr & 1)) {
                    BCAS(&ab->addrs[loc], addr, addr | 0x1);
                }
                cutoff = 1;
            } else {
                // Recursively trace it.
                if (!BCAS(&ab->addrs[loc], addr, addr | 0x3)) {
                    // Somebody else got to it first.  That's okay.
                    continue;
                }
                cutoff |= recursive_trace(ab, ts, loc, depth - 1);
            }
        }
    }
    return cutoff;
}

size_t g_total_sort; // FIXME: For debugging -- get rid of this.

static void lookup_lookaside_list (addr_buffer_t *ab)
{
    int i;
    size_t cmp = 0;
    int savings;

    size_t start_sort, end_sort;
    start_sort = forkscan_rdtsc();
    forkgc_util_sort(g_lookaside_list, g_lookaside_count);
    end_sort = forkscan_rdtsc();
    g_total_sort += end_sort - start_sort;

    savings = forkgc_util_compact(g_lookaside_list, g_lookaside_count);
    g_lookaside_count -= savings;

    int cached_loc = 0;
    for (i = 0; i < g_lookaside_count; ++i) {
        if (cmp == g_lookaside_list[i]) continue; // Possible duplicates.
        cmp = g_lookaside_list[i];
        int loc = addr_find_hint(cmp, ab, cached_loc);
        cached_loc = loc;
        if (is_ref(ab, loc, cmp)) {
            // It's a pointer somewhere into the allocated region of memory.
            size_t addr = ab->addrs[loc];
            if (!(addr & 0x1)) {
                // No need to be atomic.  Any processes racing with us are
                // trying to write the same value.
                ab->addrs[loc] = addr | 0x1;
            }
        }
#ifndef NDEBUG
        else {
            int loc2 = binary_search(cmp, ab->addrs,
                                     0, ab->n_addrs);
            // FIXME: Assert does not catch all bad cases.
            assert(ab->addrs[loc2] != cmp);
        }
#endif
    }

    g_lookaside_count = 0;
}

/**
 * Search through the given chunk of memory looking for references into the
 * memory we're tracking from outside the memory we're tracking.  These roots
 * will later be used as a basis for determining reachability of the rest of
 * the nodes.
 */
static void find_roots (size_t *mem, size_t range_size, addr_buffer_t *ab)
{
    size_t i;
    int guarded_idx;
    size_t guarded_addr;
    trace_stats_t ts;

    ts.min = PTR_MASK(ab->addrs[0]);
    ts.max = PTR_MASK(ab->addrs[ab->n_addrs - 1]);

    assert(ts.min <= ts.max);

    // Figure out where to start the search.  Any memory is a potential ptr
    // to one of our addresses, but we avoid searching memory we're tracking
    // because that will be done during mark and sweep.
    i = 0;
    guarded_idx = addr_find((size_t)mem, ab);
    guarded_addr = PTR_MASK(ab->addrs[guarded_idx]);
    if (guarded_addr <= (size_t)mem) {
        size_t sz = MALLOC_USABLE_SIZE((void*)guarded_addr);
        assert(sz > 0);
        if ((size_t)mem < guarded_addr + sz) {
            size_t diff = guarded_addr + sz - PTR_MASK((size_t)mem);
            i += ((int)diff) / sizeof(size_t);
        }
    }
    if (ab->n_addrs - 1 > guarded_idx) {
        ++guarded_idx;
        guarded_addr = PTR_MASK(ab->addrs[guarded_idx]);
    }

    for ( ; i < range_size; ++i) {
        if (guarded_addr == (size_t)(&mem[i])) {
            // Skip a little.  This area will be searched during the marking
            // phase.
            size_t sz = MALLOC_USABLE_SIZE((void*)guarded_addr);
            assert(sz > 0);
            i += ((int)sz) / sizeof(size_t) - 1;
            if (ab->n_addrs - 1 > guarded_idx) {
                ++guarded_idx;
                guarded_addr = PTR_MASK(ab->addrs[guarded_idx]);
            }
            continue;
        }

        size_t cmp = PTR_MASK(mem[i]);
        // PTR_MASK catches pointers that have been hidden through overloading
        // the two low-order bits.

        if (cmp < ts.min || cmp > ts.max) continue; // Out-of-range.

        // Put the address aside for future lookup.  By aggregating, we can
        // reduce the number of cache misses.
        g_lookaside_list[g_lookaside_count++] = cmp;
        if (g_lookaside_count < LOOKASIDE_SZ) continue;

        // The lookaside list is full.
        lookup_lookaside_list(ab);
    }
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
            if (g_n_ranges >= MAX_MARK_AND_SWEEP_RANGES) {
                forkgc_fatal("Too many memory ranges.\n");
            }
        }
    }

    return 1;
}

void forkgc_child (addr_buffer_t *ab, int fd)
{
    // Scan memory for references.
    g_bytes_to_scan = 0;
    forkgc_proc_map_iterate(collect_ranges, NULL);
    ab->completed_children = 0;
    ab->cutoff_reached = 0;

    trace_stats_t ts;
    ts.min = PTR_MASK(ab->addrs[0]);
    ts.max = PTR_MASK(ab->addrs[ab->n_addrs - 1]);

    int n_siblings = MIN_OF(MAX_CHILDREN,
                            g_bytes_to_scan / MEMORY_THRESHOLD);
    n_siblings = MIN_OF(n_siblings, g_n_ranges);
    n_siblings = MAX_OF(n_siblings, 1);

    fprintf(stderr, "Forking %d mark processes.\n", n_siblings);

    ab->sibling_mode = SIBLING_MODE_MARKING;

    int sibling_id = 0;
    for (sibling_id = 0; sibling_id < n_siblings - 1; ++sibling_id) {
        if (fork() == 0) break;
    }

    size_t start, end;

    start = forkscan_rdtsc();

    // Scan this child's ranges of memory, looking for roots into our pool.
    int i;
    size_t total_memory = 0;
    for (i = sibling_id; i < g_n_ranges; i += n_siblings) {
        // Stride memory in this for-loop.  Many chunks will have few or no
        // references and get scanned quickly.  Others have lots of refs and
        // are much slower.  It's important that the fork'd siblings break
        // up the work evenly, or some will sit around waiting while there's
        // work to be done.

        size_t *mem = (size_t*)g_ranges[i].low;
        find_roots(mem,
                   (g_ranges[i].high - g_ranges[i].low)
                   / sizeof(size_t), ab);
        total_memory += g_ranges[i].high - g_ranges[i].low;
    }

    if (g_lookaside_count > 0) {
        // Catch any remainders.
        lookup_lookaside_list(ab);
    }

    end = forkscan_rdtsc();
    fprintf(stderr, "find_roots took %zu ms.  (mem: 0x%zx, sort: %zu)\n",
            end - start,
            total_memory,
            g_total_sort);

    start = end;

    // Now, break up the pool into chunks for each process to handle, BFS-
    // style.
    int addrs_range_size, addrs_start, addrs_end;
    addrs_range_size = ab->n_addrs / n_siblings;
    addrs_start = sibling_id * addrs_range_size;
    addrs_end = sibling_id == n_siblings - 1 ?
        ab->n_addrs : (sibling_id + 1) * addrs_range_size;
    while (ab->sibling_mode != SIBLING_MODE_DONE) {
        int round = ab->round;

        // Run through this process's chunk looking for things to mark.
        for (i = addrs_start; i < addrs_end; ++i) {
            size_t addr = ab->addrs[i];
            if ((addr & 0x3) == 0x1) {
                // Node that has been marked but not recursively searched.
                if (BCAS(&ab->addrs[i], addr, addr | 0x2)) {
                    if (recursive_mark(addr, ab, &ts, 1)) {
                        // Other nodes were found and marked during this round.
                        // There will need to be another round.
                        if (0 == ab->more_marking_tbd) {
                            ab->more_marking_tbd = 1;
                        }
                    }
                }
            }
        }

        if (n_siblings - 1
            == __sync_fetch_and_add(&ab->completed_children, 1)) {

            // Last one done with this round.  Check to see if anything more
            // needs to be done.

            if (!ab->more_marking_tbd) {
                // Nobody marked anything.  All done.
                ab->sibling_mode = SIBLING_MODE_DONE;
            } else {
                ab->more_marking_tbd = 0;
            }
            ab->completed_children = 0;
            ab->round++;
        } else {
            while (round == ab->round) pthread_yield();
        }
    }

    end = forkscan_rdtsc();
    fprintf(stderr, "  chunk time: %zu ms in %d rounds.\n", end - start,
            ab->round);

    if (n_siblings - 1
        == __sync_fetch_and_add(&ab->completed_children, 1)) {

        // Alert the uber parent.
        if (sizeof(size_t) != write(fd, &g_bytes_to_scan, sizeof(size_t))) {
            forkgc_fatal("Failed to write to parent.\n");
        }
    }
}
