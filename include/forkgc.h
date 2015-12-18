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

#ifndef _FORKGC_H_
#define _FORKGC_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Allocate memory of the specified size from ForkGC's pool and return it.
 * This memory is untracked by the system.
 */
void *forkgc_malloc (size_t size);

/**
 * Retire a pointer allocated by ForkGC so that it will be free'd for reuse
 * when no remaining references to it exist.
 */
void forkgc_retire (void *ptr);

/**
 * Free a pointer allocated by ForkGC.  The memory may be immediately reused,
 * so if there is any possibility another thread may know about this memory
 * and might read from it, forkgc_retire() should be used instead.
 */
void forkgc_free (void *ptr);

/**
 * Allocate a buffer of "size" bytes and return a pointer to it.  This memory
 * will be tracked by the garbage collector, so free() should never be called
 * on it.
 */
extern void *forkgc_automalloc (size_t size);

#ifdef __cplusplus
}
#endif

#endif // !defined _FORKGC_H_
