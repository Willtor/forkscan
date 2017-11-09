/*
Copyright (c) 2015-2017 Forkscan authors

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

#ifndef _FORKSCAN_H_
#define _FORKSCAN_H_

#ifdef __cplusplus
#include <cstddef.h>
extern "C" {
#else
#include <stddef.h>
#endif

/**
 * Allocate memory of the specified size from Forkscan's pool and return it.
 * This memory is untracked by the system.
 */
void *forkscan_malloc (size_t size);

/**
 * Retire a pointer allocated by Forkscan so that it will be free'd for reuse
 * when no remaining references to it exist.
 */
void forkscan_retire (void *ptr);

/**
 * Free a pointer allocated by Forkscan.  The memory may be immediately reused,
 * so if there is any possibility another thread may know about this memory
 * and might read from it, forkscan_retire() should be used instead.
 */
void forkscan_free (void *ptr);

/**
 * Perform an iteration of reclamation.  This is intended for users who have
 * disabled automatic iterations or who otherwise want to override it and
 * force an iteration at a time that is convenient for their application.
 *
 * If this call contends with another thread trying to reclaim, one of them
 * will fail and return a non-zero value.  forkscan_force_reclaim() returns
 * zero on the thread that succeeds.
 */
int forkscan_force_reclaim ();

/**
 * auto_run = 1 (enable) or 0 (disable) automatic iterations of reclamation.
 * If the automatic system is disabled, it is up to the user to force
 * iterations with forkscan_force_reclaim().
 */
void forkscan_set_auto_run (int auto_run);

/**
 * Allocate a buffer of "size" bytes and return a pointer to it.  This memory
 * will be tracked by the garbage collector, so free() should never be called
 * on it.
 */
extern void *forkscan_automalloc (size_t size);

/**
 * Set the allocator for Forkscan to use: malloc, free, malloc_usable_size.
 */
extern void forkscan_set_allocator (void *(*alloc) (size_t),
                                    void (*dealloc) (void *),
                                    size_t (*usable_size) (void *));

#ifdef __cplusplus
}
#endif

#endif // !defined _FORKSCAN_H_
