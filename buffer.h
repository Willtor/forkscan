/*
Copyright (c) 2016 Forkscan authors

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

#ifndef _BUFFER_H_
#define _BUFFER_H_

typedef enum sibling_mode_t sibling_mode_t;

enum sibling_mode_t { SIBLING_MODE_MARKING,
                      SIBLING_MODE_DONE };

typedef struct addr_buffer_t addr_buffer_t;

struct addr_buffer_t {
    addr_buffer_t *next;
    size_t *addrs;
    size_t *minimap;
    int is_aggregate; // Has minimap space.
    int n_addrs;
    int n_minimap;
    int capacity;
    int cutoff_reached;
    sibling_mode_t sibling_mode;
    int completed_children;
    int more_marking_tbd;
    volatile int round; // Trust we won't need more than 2 billion rounds.

    // After marking has been done, these fields are used by threads that
    // want to free the unreferenced nodes.
    volatile int ref_count;
    volatile int free_idx;
};

addr_buffer_t *forkscan_make_reclaimer_buffer ();

addr_buffer_t *forkscan_make_aggregate_buffer (int capacity);

void forkscan_release_buffer (addr_buffer_t *ab);

void forkscan_buffer_push_back (addr_buffer_t *ab);

void forkscan_buffer_pop_retiree_buffer (addr_buffer_t *ab);

addr_buffer_t *forkscan_buffer_get_retiree_buffer ();

void forkscan_buffer_unref_buffer (addr_buffer_t *ab);

addr_buffer_t *forkscan_buffer_get_dead_references ();

void *forkscan_buffer_makestack (size_t *stacksize);

void forkscan_buffer_freestack (void *p);

#endif // !defined _BUFFER_H_

