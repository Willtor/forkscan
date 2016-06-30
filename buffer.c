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

#include "alloc.h"
#include "buffer.h"
#include "env.h"
#include "util.h"

addr_buffer_t *forkscan_make_reclaimer_buffer ()
{
    int capacity = g_forkgc_ptrs_per_thread * MAX_THREAD_COUNT;
    size_t sz = capacity * sizeof(size_t) + PAGESIZE;
    char *raw_mem = forkgc_alloc_mmap(sz);

    //   0 - 4095: Reserved page for the addr_buffer_t struct.
    //   4096 -  : Address list.
    addr_buffer_t *ab = (addr_buffer_t*)raw_mem;
    ab->addrs = (size_t*)&raw_mem[PAGESIZE];
    ab->n_addrs = 0;
    ab->capacity = capacity;

    return ab;
}

void forkscan_release_reclaimer_buffer (addr_buffer_t *ab)
{

}
