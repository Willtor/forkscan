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

#include <assert.h>
#include "queue.h"
#include <string.h>

#define MAX_OF(a, b) ((a) > (b) ? (a) : (b))
#define MIN_OF(a, b) ((a) > (b) ? (b) : (a))

// Convert an absolute index into an array offset.  Note: capacity _must_ be
// a power of 2!
#define INDEXIFY(abs_idx, capacity) ((abs_idx) & ((capacity) - 1))

/**
 * Initialize a queue object.  Queues are implemented as circular buffers.
 */
void forkscan_queue_init (queue_t *q, size_t *buf, size_t capacity)
{
    q->e = buf;
    q->capacity = capacity;
    q->idx_head = 0;
    q->idx_tail = capacity;
}

/**
 * Return 1 if the queue is empty, zero otherwise.
 */
int forkscan_queue_is_empty (queue_t *q)
{
    assert(q->idx_head < q->idx_tail);
    return q->idx_head + q->capacity == q->idx_tail;
}

/**
 * Return 1 if the queue is full, zero otherwise.
 */
int forkscan_queue_is_full (queue_t *q)
{
    assert(q->idx_head < q->idx_tail);
    return q->idx_head + 1 >= q->idx_tail ? 1 : 0;
}

/**
 * Return the number of empty slots in the queue.
 */
int forkscan_queue_available (queue_t *q)
{
    assert(q->idx_head < q->idx_tail);
    // We can use an int since the two values are actually very close.
    int ret = (int)(q->idx_tail - q->idx_head - 1);
    assert(ret >= 0);
    return ret;
}

/**
 * Push a value onto the head of the queue.  Caller must verify there is
 * space on the queue.
 */
void forkscan_queue_push (queue_t *q, size_t value)
{
    q->e[INDEXIFY(q->idx_head, q->capacity)] = value;
    ++q->idx_head;
    assert(q->idx_head < q->idx_tail);
}

/**
 * Remove a value from the tail of the queue and return it.
 */
size_t forkscan_queue_pop (queue_t *q)
{
    size_t ret = q->e[INDEXIFY(q->idx_tail, q->capacity)];
    ++q->idx_tail;
    return ret;
}

/**
 * Push a block of values onto the queue of count "len".  Caller must verify
 * there is space on the queue.
 */
void forkscan_queue_push_bulk (queue_t *q, size_t values[], size_t len)
{
    // Perform a whole bunch of reads up-front in case this is a high-
    // contention operation.
    size_t idx_head = q->idx_head;
    size_t capacity = q->capacity;

    size_t head = INDEXIFY(idx_head, capacity);

    // Up to two copies may be performed: One to the end of the circular
    // buffer, and the second from the beginning.  The second copy may have
    // zero elements.
    size_t first_copy = MIN_OF(len, capacity - head);
    size_t second_copy = len - first_copy;

    if (first_copy > 0) {
        memcpy(&q->e[head], values, first_copy * sizeof(size_t));
        idx_head += first_copy;
    }

    if (second_copy > 0) {
        head = INDEXIFY(idx_head, capacity);
        memcpy(&q->e[head], &values[first_copy], second_copy * sizeof(size_t));
        idx_head += second_copy;
    }

    q->idx_head = idx_head;
}

/**
 * Pop a block of values from the queue, up to "len" in count.  The values
 * buffer is populated with the removed values.  The return value is the
 * count of values that were pop'd.
 */
int forkscan_queue_pop_bulk (size_t values[], size_t len, queue_t *q)
{
    size_t idx_head = q->idx_head;    // Cache idx_head which may be changing.
    size_t size =
        idx_head - (q->idx_tail - q->capacity);   // idx_tail is not changing.
    size_t popped;                    // # elements popped; return value.

    assert(len < 0xF00000000000000ULL);

    if (size == 0) return 0;          // Short-circuit.
    if (size > len) {
        // Prevent overflowing the output buffer.  "Fake" the head index so
        // it looks like fewer elements have been pushed.
        idx_head -= size - len;
        size = len;
    }

    popped = size;

    // Copy values out of the queue.  Since it is a circular buffer, the logic
    // is a little complex: the start point may be ahead of the end point.  If
    // that is so, two memcpy's have to be performed.
    size_t head = INDEXIFY(idx_head, q->capacity);
    size_t start = INDEXIFY(q->idx_tail, q->capacity);
    size_t values_offset = 0;
    size_t elements;

    if (head < start) {
        // Have to perform two memcpy's.  First, drain to the end of the
        // buffer.
        elements = q->capacity - start;
        memcpy(values, &q->e[start], elements * sizeof(size_t));
        values_offset = elements;
        start = 0;
    }

    // Drain the buffer for the [remaining] length of the target buffer..
    elements = head - start;
    if (elements > 0) {
        memcpy(&values[values_offset], &q->e[start],
               elements * sizeof(size_t));
    }
    q->idx_tail = idx_head + q->capacity;

#ifndef NDEBUG
    {
        int i;
        for (i = 0; i < popped; ++i) assert(values[i] != 0);
    }
#endif

    return popped;
}
