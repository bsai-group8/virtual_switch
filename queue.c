#define _POSIX_C_SOURCE 200809L

#include <errno.h>

#include "queue.h"

#define AGING_MS 200
#define AGE_GAP 5

static const int weight[NUM_PRIO] = {5, 3, 1};

static int bucket_cap(int rate)
{
    return rate * 100 > 1000 ? rate * 100 : 1000;
}

void queue_init(struct port_queue *q)
{
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->closing = 1;
    q->scheduling_started = 0;
    q->initial_wait_done = 0;
    q->since_aged = 0;
}

void queue_destroy(struct port_queue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

void queue_open(struct port_queue *q, int rate)
{
    pthread_mutex_lock(&q->lock);
    q->closing = 0;
    q->tokens = bucket_cap(rate);
    q->scheduling_started = 0;
    q->initial_wait_done = 0;
    q->since_aged = 0;
    pthread_mutex_unlock(&q->lock);
}

void queue_close(struct port_queue *q)
{
    pthread_mutex_lock(&q->lock);
    q->closing = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

void queue_drain(struct port_queue *q)
{
    int i;

    pthread_mutex_lock(&q->lock);
    for (i = 0; i < NUM_PRIO; i++)
    {
        while (q->cnt[i] > 0)
        {
            frame_unref(q->q[i][q->head[i]].f);
            q->head[i] = (q->head[i] + 1) % QCAP;
            q->cnt[i]--;
        }
    }
    pthread_mutex_unlock(&q->lock);
}

void queue_refill(struct port_queue *q, int rate, int tick_ms)
{
    pthread_mutex_lock(&q->lock);
    if (rate > 0)
    {
        q->tokens += rate * tick_ms;
        if (q->tokens > bucket_cap(rate))
        {
            q->tokens = bucket_cap(rate);
        }
    }
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

int queue_push(struct port_queue *q, struct frame *f)
{
    struct qent *e;
    int rc = 0;

    pthread_mutex_lock(&q->lock);
    if (q->closing)
    {
        rc = 1;
    }
    else if (q->cnt[f->prio] == QCAP)
    {
        rc = -1;
    }
    else
    {
        e = &q->q[f->prio][(q->head[f->prio] + q->cnt[f->prio]) % QCAP];
        e->f = f;
        e->t = now_ms();
        q->cnt[f->prio]++;
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
    return rc;
}

static int pick(struct port_queue *q)
{
    long long now = now_ms();
    int i, round;

    q->since_aged++;
    if (q->scheduling_started)
    {
        for (i = NUM_PRIO - 1; i > 0 && q->since_aged > AGE_GAP; i--)
        {
            if (q->cnt[i] > 0 && now - q->q[i][q->head[i]].t > AGING_MS)
            {
                q->since_aged = 0;
                return i;
            }
        }
    }
    for (round = 0; round < 2; round++)
    {
        for (i = 0; i < NUM_PRIO; i++)
        {
            if (q->cnt[i] > 0 && q->credit[i] > 0)
            {
                return i;
            }
        }
        for (i = 0; i < NUM_PRIO; i++)
        {
            q->credit[i] = weight[i];
        }
        q->scheduling_started = 1;
    }
    return 0;
}

struct frame *queue_pop(struct port_queue *q, atomic_int *rate)
{
    struct timespec ts;
    struct frame *f;
    int pr, r;

    pthread_mutex_lock(&q->lock);
    for (;;)
    {
        r = atomic_load(rate);
        if (q->closing)
        {
            pthread_mutex_unlock(&q->lock);
            return NULL;
        }
        if (r > 0 && !q->initial_wait_done
            && q->cnt[0] + q->cnt[1] + q->cnt[2] > 0)
        {
            ts_in_ms(&ts, 500);
            do
            {
                if (q->closing)
                {
                    pthread_mutex_unlock(&q->lock);
                    return NULL;
                }
                r = pthread_cond_timedwait(&q->cond, &q->lock, &ts);
            } while (r != ETIMEDOUT);
            q->initial_wait_done = 1;
        }
        if (q->cnt[0] + q->cnt[1] + q->cnt[2] == 0)
        {
            pthread_cond_wait(&q->cond, &q->lock);
        }
        else if (r > 0 && q->tokens < 1000)
        {
            ts_in_ms(&ts, 100);
            pthread_cond_timedwait(&q->cond, &q->lock, &ts);
        }
        else
        {
            break;
        }
    }
    pr = pick(q);
    f = q->q[pr][q->head[pr]].f;
    q->head[pr] = (q->head[pr] + 1) % QCAP;
    q->cnt[pr]--;
    if (q->credit[pr] > 0)
    {
        q->credit[pr]--;
    }
    if (r > 0)
    {
        q->tokens -= 1000;
    }
    pthread_mutex_unlock(&q->lock);
    return f;
}