#include "queue.h"

#include <stdlib.h>
#include <string.h>

static const int wrr_weight[NUM_PRIO] = {WRR_HIGH, WRR_NORMAL, WRR_LOW};

/* ---------------------------------------------------------------- */
/* the three rings (q->lock held by every caller)                   */
/* ---------------------------------------------------------------- */

static int pq_push(struct pq *p, struct frame *f)
{
    if (p->count == p->cap)
    {
        return -1;
    }
    p->slot[(p->head + p->count) % p->cap] = f;
    p->count++;
    return 0;
}

static struct frame *pq_pop(struct pq *p)
{
    struct frame *f;

    if (p->count == 0)
    {
        return NULL;
    }
    f       = p->slot[p->head];
    p->head = (p->head + 1) % p->cap;
    p->count--;
    return f;
}

/* ---------------------------------------------------------------- */
/* life cycle                                                       */
/* ---------------------------------------------------------------- */

int queue_init(struct port_queue *q, int cap, int rate)
{
    int i;
    int j;

    if (cap < 1)
    {
        cap = 1;
    }
    memset(q, 0, sizeof *q);

    if (pthread_mutex_init(&q->lock, NULL) != 0)
    {
        return -1;
    }
    if (pthread_cond_init(&q->not_empty, NULL) != 0)
    {
        pthread_mutex_destroy(&q->lock);
        return -1;
    }
    if (pthread_cond_init(&q->has_token, NULL) != 0)
    {
        pthread_cond_destroy(&q->not_empty);
        pthread_mutex_destroy(&q->lock);
        return -1;
    }

    for (i = 0; i < NUM_PRIO; i++)
    {
        /* each ring can hold the whole budget; q->total is the real bound */
        q->q[i].slot = calloc((size_t)cap, sizeof *q->q[i].slot);
        if (q->q[i].slot == NULL)
        {
            for (j = 0; j < i; j++)
            {
                free(q->q[j].slot);
            }
            pthread_cond_destroy(&q->has_token);
            pthread_cond_destroy(&q->not_empty);
            pthread_mutex_destroy(&q->lock);
            return -1;
        }
        q->q[i].cap    = cap;
        q->q[i].head   = 0;
        q->q[i].count  = 0;
        q->credit[i]   = wrr_weight[i];
        q->starved[i]  = 0;
    }
    q->cap    = cap;
    q->total  = 0;
    q->closed = 0;
    q->rate   = rate;
    q->burst  = rate > 0 ? rate : 0;
    q->tokens = rate > 0 ? (double)rate : 0.0;
    return 0;
}

void queue_destroy(struct port_queue *q)
{
    int i;

    queue_drain(q);
    for (i = 0; i < NUM_PRIO; i++)
    {
        free(q->q[i].slot);
        q->q[i].slot = NULL;
    }
    pthread_cond_destroy(&q->has_token);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

/* ---------------------------------------------------------------- */
/* producer side: never blocks                                      */
/* ---------------------------------------------------------------- */

int queue_push(struct port_queue *q, struct frame *f)
{
    int prio = f->hdr.priority;
    int rc;

    if (prio < 0 || prio >= NUM_PRIO)
    {
        prio = PRIO_NORMAL;
    }

    pthread_mutex_lock(&q->lock);
    if (q->closed || q->total >= q->cap)
    {
        /* full or shut: drop it, count it, and get out of the way.  The
         * receiving thread must never wait on a full queue. */
        q->dropped++;
        pthread_mutex_unlock(&q->lock);
        frame_unref(f);
        return -1;
    }
    rc = pq_push(&q->q[prio], f);
    if (rc != 0)
    {
        q->dropped++;
        pthread_mutex_unlock(&q->lock);
        frame_unref(f);
        return -1;
    }
    q->total++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ---------------------------------------------------------------- */
/* scheduling: weighted round robin with ageing                     */
/* ---------------------------------------------------------------- */

static int pick_prio(struct port_queue *q)
{
    int p;

    /* ageing wins first: anything left waiting too long goes next, so a
     * flood of high priority frames cannot starve the low queue */
    for (p = NUM_PRIO - 1; p >= 1; p--)
    {
        if (q->q[p].count > 0 && q->starved[p] >= STARVE_LIMIT)
        {
            return p;
        }
    }
    /* then plain weighted round robin, best priority with credit left */
    for (p = 0; p < NUM_PRIO; p++)
    {
        if (q->q[p].count > 0 && q->credit[p] > 0)
        {
            return p;
        }
    }
    /* everybody spent their credit: start a new round */
    for (p = 0; p < NUM_PRIO; p++)
    {
        q->credit[p] = wrr_weight[p];
    }
    for (p = 0; p < NUM_PRIO; p++)
    {
        if (q->q[p].count > 0)
        {
            return p;
        }
    }
    return -1;
}

static void account(struct port_queue *q, int served)
{
    int p;

    if (q->credit[served] > 0)
    {
        q->credit[served]--;
    }
    q->starved[served] = 0;
    for (p = 0; p < NUM_PRIO; p++)
    {
        if (p != served && q->q[p].count > 0)
        {
            q->starved[p]++;
        }
    }
}

struct frame *queue_pop(struct port_queue *q)
{
    struct timespec ts;
    struct frame   *f = NULL;
    int             p;

    pthread_mutex_lock(&q->lock);
    for (;;)
    {
        if (q->closed)
        {
            /* the port is going away; whatever is queued gets freed by
             * queue_drain(), we just stop */
            break;
        }
        if (q->total == 0)
        {
            pthread_cond_wait(&q->not_empty, &q->lock);
            continue;
        }
        if (q->rate > 0 && q->tokens < 1.0)
        {
            /* rate limited.  Wait for the refill timer, do not spin and
             * do not sleep(): a timed condition wait is the one tool
             * that is both correct and cheap here. */
            ts_in_ms(&ts, TOKEN_WAIT_MS);
            pthread_cond_timedwait(&q->has_token, &q->lock, &ts);
            continue;
        }
        p = pick_prio(q);
        if (p < 0)
        {
            continue; /* cannot happen while total > 0, but be safe */
        }
        f = pq_pop(&q->q[p]);
        q->total--;
        account(q, p);
        if (q->rate > 0)
        {
            q->tokens -= 1.0;
        }
        q->sent++;
        break;
    }
    pthread_mutex_unlock(&q->lock);
    return f;
}

/* ---------------------------------------------------------------- */
/* control                                                          */
/* ---------------------------------------------------------------- */

void queue_close(struct port_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closed = 1;
    /* wake every thread that is parked on either condition variable */
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->has_token);
    pthread_mutex_unlock(&q->lock);
}

void queue_reopen(struct port_queue *q)
{
    int i;

    pthread_mutex_lock(&q->lock);
    q->closed  = 0;
    q->dropped = 0;
    q->sent    = 0;
    for (i = 0; i < NUM_PRIO; i++)
    {
        q->credit[i]  = wrr_weight[i];
        q->starved[i] = 0;
    }
    q->tokens = q->rate > 0 ? (double)q->rate : 0.0;
    pthread_mutex_unlock(&q->lock);
}

int queue_drain(struct port_queue *q)
{
    struct frame *f;
    int           freed = 0;
    int           i;

    pthread_mutex_lock(&q->lock);
    for (i = 0; i < NUM_PRIO; i++)
    {
        while ((f = pq_pop(&q->q[i])) != NULL)
        {
            frame_unref(f); /* the last holder frees it */
            freed++;
        }
    }
    q->total = 0;
    pthread_mutex_unlock(&q->lock);
    return freed;
}

void queue_refill(struct port_queue *q, double seconds)
{
    pthread_mutex_lock(&q->lock);
    if (q->rate > 0)
    {
        q->tokens += (double)q->rate * seconds;
        if (q->tokens > (double)q->burst)
        {
            q->tokens = (double)q->burst;
        }
        if (q->tokens >= 1.0)
        {
            pthread_cond_broadcast(&q->has_token);
        }
    }
    pthread_mutex_unlock(&q->lock);
}

void queue_set_rate(struct port_queue *q, int rate)
{
    pthread_mutex_lock(&q->lock);
    q->rate   = rate > 0 ? rate : 0;
    q->burst  = q->rate;
    q->tokens = q->rate > 0 ? (double)q->rate : 0.0;
    pthread_cond_broadcast(&q->has_token); /* an unlimited port must not stay parked */
    pthread_mutex_unlock(&q->lock);
}

int queue_depth(struct port_queue *q)
{
    int n;

    pthread_mutex_lock(&q->lock);
    n = q->total;
    pthread_mutex_unlock(&q->lock);
    return n;
}

uint64_t queue_dropped(struct port_queue *q)
{
    uint64_t n;

    pthread_mutex_lock(&q->lock);
    n = q->dropped;
    pthread_mutex_unlock(&q->lock);
    return n;
}