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

#ifndef _METAUTIL_H_
#define _METAUTIL_H_

#define DEFINE_POOL_ALLOC(pool, dtsize, batch_sz, mmap)                 \
    typedef struct pool##_node_t pool##_node_t;                         \
    struct pool##_node_t { pool##_node_t *next; };                      \
    static pool##_node_t *g_##pool##_pool;                              \
    static pthread_mutex_t g_##pool##_lock = PTHREAD_MUTEX_INITIALIZER; \
    static void *pool_alloc_##pool ()                                   \
    {                                                                   \
        pool##_node_t *ret = NULL;                                      \
        pthread_mutex_lock(&g_##pool##_lock);                           \
        if (g_##pool##_pool) {                                          \
            ret = g_##pool##_pool;                                      \
            g_##pool##_pool = ret->next;                                \
            ret->next = NULL;                                           \
        }                                                               \
        pthread_mutex_unlock(&g_##pool##_lock);                         \
        if (ret) return (void*)ret;                                     \
        char *arr =                                                     \
            (char*)mmap(dtsize * batch_sz, #pool);                      \
        pthread_mutex_lock(&g_##pool##_lock);                           \
        int i;                                                          \
        for (i = 1; i < batch_sz; ++i) {                                \
            pool##_node_t *node = (pool##_node_t*)&arr[i * dtsize];     \
            node->next = g_##pool##_pool;                               \
            g_##pool##_pool = node;                                     \
        }                                                               \
        pthread_mutex_unlock(&g_##pool##_lock);                         \
        return (void*)&arr[0];                                          \
    }                                                                   \
    static void pool_free_##pool (void *p)                              \
    {                                                                   \
        pool##_node_t *node = (pool##_node_t*)p;                        \
        pthread_mutex_lock(&g_##pool##_lock);                           \
        node->next = g_##pool##_pool;                                   \
        g_##pool##_pool = node;                                         \
        pthread_mutex_unlock(&g_##pool##_lock);                         \
    }

#endif
