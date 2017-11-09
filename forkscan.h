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

#ifndef _FORKSCAN_H_
#define _FORKSCAN_H_

#include "buffer.h"
#include "child.h"
#include <signal.h>

#define SIGFORKSCAN SIGUSR1

/**
 * Acknowledge the signal sent by the GC thread and perform any work required.
 */
void forkscan_acknowledge_signal ();

/**
 * Pass a list of pointers to the reclamation thread for it to collect.
 */
void forkscan_initiate_collection (addr_buffer_t *ab, int run_iteration);

/**
 * Garbage-collector thread.
 */
void *forkscan_thread (void *ignored);

/**
 * Print program statistics to stdout.
 */
void forkscan_print_statistics ();

#endif // !defined FORKSCAN
