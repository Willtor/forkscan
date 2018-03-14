/*
Copyright (c) 2018 Forkscan authors

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

#include <time.h>
#include <unistd.h>
#include <stdio.h>

/**
 * Robust sleep with microsecond intervals.  This won't exit when there's
 * an interrupt, as commonly occurs in Forkscan.
 */
void forkscan_usleep (unsigned long long usec)
{
    unsigned long long begin_us, current_us, end_us;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    current_us = (ts.tv_sec * 1000 * 1000) + (ts.tv_nsec / 1000);
    begin_us = current_us;
    end_us = begin_us + usec;

    while (current_us < end_us) {
        usleep(end_us - current_us);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        current_us = (ts.tv_sec * 1000 * 1000) + (ts.tv_nsec / 1000);
    }
}

/**
 * Robust sleep with whole-second intervals.  This won't exit when there's
 * an interrupt, as commonly occurs in Forkscan.
 */
void forkscan_sleep (unsigned int seconds)
{
    forkscan_usleep(seconds * 1000 * 1000);
}
