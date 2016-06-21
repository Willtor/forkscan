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

#ifndef _CHILD_H_
#define _CHILD_H_

#include "queue.h"

#define MAX_MARK_AND_SWEEP_RANGES 4096

typedef struct gc_data_t gc_data_t;

struct gc_data_t {
    gc_data_t *next;
    size_t *addrs;
    /////////////////////////////////////////// FIXME: unnecessary?
    size_t *minimap;
    int *refs;
    ///////////////////////////////////////////
    int n_addrs;
    int n_minimap; // unnecessary?
    int capacity;
    int completed_children;
    int cutoff_reached;
};

int is_ref (gc_data_t *gc_data, int loc, size_t cmp);

int binary_search (size_t val, size_t *a, int min, int max);

void forkgc_child (gc_data_t *gc_data, int fd);

#endif // !defined _CHILD_H_
