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

#ifndef _FORKGC_H_
#define _FORKGC_H_

#include "child.h" // FIXME: Should define gc_data_t in forkgc.h
#include <signal.h>

#define SIGTHREADSCAN SIGUSR1

/**
 * Acknowledge the signal sent by the GC thread and perform any work required.
 */
void forkgc_acknowledge_signal ();

/**
 * Pass a list of pointers to the GC thread for it to collect.
 */
void forkgc_initiate_collection (gc_data_t *gc_data);

/**
 * Garbage-collector thread.
 */
void *forkgc_thread (void *ignored);

/**
 * Print program statistics to stdout.
 */
void forkgc_print_statistics ();

#endif
