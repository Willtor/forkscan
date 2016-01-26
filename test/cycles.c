#include <assert.h>
#include <forkgc.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

typedef struct obj obj;

struct obj
{
    obj *next;
    size_t data;
};

static volatile int forked;

static void *waiter (void *ignored)
{
    usleep(1000 * 1000); // should be interrupted by the fork.
    forked = 1;
}

obj *hide_or_unhide (obj *ptr)
{
    size_t addr = (size_t)ptr;
    addr ^= 0xFFFFFFFFFFFFFFFF;
    return (obj*)addr;
}

static obj *new_obj (size_t data)
{
    obj *ret = forkgc_malloc(sizeof(obj));
    ret->next = NULL;
    ret->data = data;
    return ret;
}

static obj *make_loop (int sz, size_t data)
{
    obj *head, *tail;
    int i;

    head = tail = new_obj(data);
    for (i = 1; i < sz; ++i) {
        tail->next = new_obj(data);
        tail = tail->next;
    }
    tail->next = head;
    return head;
}

static void retire_loop (obj *loop)
{
    obj *tmp = loop;
    forkgc_retire(tmp);
    tmp = tmp->next;
    while (tmp != loop) {
        forkgc_retire(tmp);
        tmp = tmp->next;
    }
}

static void verify_loop_okay (obj *loop)
{
    obj *tmp = loop;
    assert(tmp->data == 1);
    tmp = tmp->next;
    while (tmp != loop) {
        assert(tmp->data == 1);
        tmp = tmp->next;
    }
}

static void verify_destroyed (obj *loop)
{
    // ForkGC zeroes the memory.
    assert(loop->data != 1);
}

int main ()
{
    pthread_t tid;
    obj *local_connected;
    obj *local_disconnected;
    obj *tmp;
    int alloc = 0;

    local_connected = make_loop(5, 1);
    alloc += 5;
    local_disconnected = make_loop(5, 1);
    alloc += 5;

    retire_loop(local_connected);
    retire_loop(local_disconnected);

    local_disconnected = hide_or_unhide(local_disconnected);

    forked = 0;
    pthread_create(&tid, NULL, waiter, NULL);

    while (0 == forked) {
        tmp = make_loop(5, 1);
        alloc += 5;
        retire_loop(tmp);
    }

    // Give the collector some time to do its thing.
    usleep(1000 * 1000);
    usleep(1000 * 1000);

    // Do some allocation to assure objects get freed.
    while (alloc > 0) {
        make_loop(5, 0);
        alloc -= 5;
    }

    verify_loop_okay(local_connected);
    verify_destroyed(hide_or_unhide(local_disconnected));

    printf("Done.\n");

    return 0;
}
