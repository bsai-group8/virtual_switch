#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stdatomic.h>

#include "frame.h"

#define QCAP 64
struct qent
{
    struct frame *f;
    long long     t;
};

struct port_queue
{
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    struct qent     q[NUM_PRIO][QCAP];
    int             head[NUM_PRIO];
    int             cnt[NUM_PRIO];
    int             credit[NUM_PRIO];
    int             tokens;
    int             closing;
    int             scheduling_started;
    int             initial_wait_done;
    int             since_aged;
};

void queue_init(struct port_queue *q);
void queue_destroy(struct port_queue *q);
void queue_open(struct port_queue *q, int rate);
void queue_close(struct port_queue *q);
void queue_drain(struct port_queue *q);
void queue_refill(struct port_queue *q, int rate, int tick_ms);

int queue_push(struct port_queue *q, struct frame *f);
struct frame *queue_pop(struct port_queue *q, atomic_int *rate);

#endif