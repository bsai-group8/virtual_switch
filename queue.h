#ifndef QUEUE_H
#define QUEUE_H

#include <pthread.h>
#include <stdint.h>

#include "frame.h"

/*
 * One bounded output queue per port (items 2, 3, 6, 7).
 *
 * item 2  No head of line blocking.  queue_push() NEVER blocks: if the
 *         queue is full the frame is dropped and counted.  A receiver
 *         thread therefore cannot be held up by a frozen host on some
 *         other port.  Only that port's own sender thread blocks, in
 *         write(), which is exactly where it should block.
 *
 * item 3  Frames are stored by reference.  queue_push() takes over one
 *         reference from the caller and queue_pop() hands it on.  A
 *         broadcast is one frame in N queues, not N copies.
 *
 * item 6  Three priority queues (high, normal, low) served by weighted
 *         round robin 5:3:1, with ageing so low priority never starves.
 *
 * item 7  A token bucket.  When a port is out of tokens its sender waits
 *         on a condition variable with pthread_cond_timedwait, never on
 *         sleep() and never spinning.  A timer thread calls
 *         queue_refill().
 */

#define WRR_HIGH     5
#define WRR_NORMAL   3
#define WRR_LOW      1
/* after this many dequeues without being served, a queue jumps ahead */
#define STARVE_LIMIT 16
/* how long a tokenless sender waits before looking again */
#define TOKEN_WAIT_MS 50

struct pq
{
    struct frame **slot;
    int            cap;
    int            head;
    int            count;
};

struct port_queue
{
    pthread_mutex_t lock;
    pthread_cond_t  not_empty; /* something to send            */
    pthread_cond_t  has_token; /* the rate limiter let go      */

    struct pq q[NUM_PRIO];
    int       total;
    int       cap;    /* hard bound across all three queues */
    int       closed;

    uint64_t dropped;
    uint64_t sent;

    /* weighted round robin */
    int credit[NUM_PRIO];
    int starved[NUM_PRIO];

    /* token bucket; rate == 0 means "no limit" */
    double tokens;
    int    rate;  /* frames per second */
    int    burst; /* bucket depth      */
};

int  queue_init(struct port_queue *q, int cap, int rate);
void queue_destroy(struct port_queue *q);

/* Always consumes the caller's reference.  0 = queued, -1 = dropped. */
int queue_push(struct port_queue *q, struct frame *f);

/* Blocks until a frame is available.  NULL means the queue was closed:
 * the sender thread should return. */
struct frame *queue_pop(struct port_queue *q);

void queue_close(struct port_queue *q);  /* wake everyone, stop accepting */
void queue_reopen(struct port_queue *q); /* reuse the slot for a new host */
int  queue_drain(struct port_queue *q);  /* unref whatever is left        */

void queue_refill(struct port_queue *q, double seconds);
void queue_set_rate(struct port_queue *q, int rate);

int      queue_depth(struct port_queue *q);
uint64_t queue_dropped(struct port_queue *q);

#endif /* QUEUE_H */