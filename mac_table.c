#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "mac_table.h"

struct mac_row
{
    uint16_t vlan;
    uint8_t  mac[MAC_LEN];
    int      port;
    long     age;
};

static unsigned hash(uint16_t vlan, const uint8_t mac[MAC_LEN])
{
    unsigned h = vlan;
    int i;

    for (i = 0; i < MAC_LEN; i++)
    {
        h = h * 31 + mac[i];
    }
    return h % MAC_BUCKETS;
}

static int same(const struct mac_entry *e, uint16_t vlan, const uint8_t mac[MAC_LEN])
{
    return e->vlan == vlan && memcmp(e->mac, mac, MAC_LEN) == 0;
}

static void unlink_entry(struct mac_table *t, struct mac_entry **pp)
{
    struct mac_entry *e = *pp;

    *pp = e->next;
    atomic_fetch_sub(&t->on_port[e->port], 1);
    atomic_fetch_sub(&t->total, 1);
    free(e);
}

int mac_table_init(struct mac_table *t, int max_entries, int age_seconds)
{
    int i;

    for (i = 0; i < MAC_BUCKETS; i++)
    {
        t->bucket[i].head = NULL;
        if (pthread_mutex_init(&t->bucket[i].lock, NULL) != 0)
        {
            return -1;
        }
    }
    atomic_init(&t->total, 0);
    atomic_init(&t->max_entries, max_entries);
    atomic_init(&t->age_seconds, age_seconds);
    for (i = 0; i < MAX_PORTS; i++)
    {
        atomic_init(&t->on_port[i], 0);
    }
    return 0;
}

void mac_table_destroy(struct mac_table *t)
{
    struct mac_entry *e;
    int i;

    for (i = 0; i < MAC_BUCKETS; i++)
    {
        while ((e = t->bucket[i].head) != NULL)
        {
            t->bucket[i].head = e->next;
            free(e);
        }
        pthread_mutex_destroy(&t->bucket[i].lock);
    }
}

int mac_table_lookup(struct mac_table *t, uint16_t vlan, const uint8_t mac[MAC_LEN])
{
    struct mac_bucket *b = &t->bucket[hash(vlan, mac)];
    struct mac_entry *e;
    int port = -1;

    pthread_mutex_lock(&b->lock);
    for (e = b->head; e != NULL; e = e->next)
    {
        if (same(e, vlan, mac))
        {
            port = e->port;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return port;
}

static int evict_oldest(struct mac_table *t)
{
    struct mac_entry *e, **pp;
    uint8_t mac[MAC_LEN] = {0};
    uint16_t vlan = 0;
    long best = 0;
    int i, bi = -1;

    for (i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&t->bucket[i].lock);
        for (e = t->bucket[i].head; e != NULL; e = e->next)
        {
            if (bi < 0 || e->last_seen < best)
            {
                best = e->last_seen;
                bi = i;
                vlan = e->vlan;
                memcpy(mac, e->mac, MAC_LEN);
            }
        }
        pthread_mutex_unlock(&t->bucket[i].lock);
    }
    if (bi < 0)
    {
        return -1;
    }
    pthread_mutex_lock(&t->bucket[bi].lock);
    for (pp = &t->bucket[bi].head; *pp != NULL; pp = &(*pp)->next)
    {
        if (same(*pp, vlan, mac))
        {
            unlink_entry(t, pp);
            break;
        }
    }
    pthread_mutex_unlock(&t->bucket[bi].lock);
    return 0;
}

int mac_table_learn(struct mac_table *t, uint16_t vlan, const uint8_t mac[MAC_LEN],
                    int port, int limit, int *old)
{
    struct mac_bucket *b = &t->bucket[hash(vlan, mac)];
    struct mac_entry *e;
    int res = MAC_OK;
    int over = limit > 0 && atomic_load(&t->on_port[port]) >= limit;

    pthread_mutex_lock(&b->lock);
    for (e = b->head; e != NULL; e = e->next)
    {
        if (same(e, vlan, mac))
        {
            break;
        }
    }
    if (e != NULL)
    {
        e->last_seen = mono_sec();
        if (e->port != port)
        {
            if (over)
            {
                pthread_mutex_unlock(&b->lock);
                return MAC_SECURITY;
            }
            *old = e->port;
            atomic_fetch_sub(&t->on_port[e->port], 1);
            atomic_fetch_add(&t->on_port[port], 1);
            e->port = port;
            res = MAC_MOVED;
        }
        pthread_mutex_unlock(&b->lock);
        return res;
    }
    if (over)
    {
        pthread_mutex_unlock(&b->lock);
        return MAC_SECURITY;
    }
    e = malloc(sizeof *e);
    if (e == NULL)
    {
        pthread_mutex_unlock(&b->lock);
        return MAC_OK;
    }
    memcpy(e->mac, mac, MAC_LEN);
    e->vlan = vlan;
    e->port = port;
    e->last_seen = mono_sec();
    e->next = b->head;
    b->head = e;
    atomic_fetch_add(&t->on_port[port], 1);
    atomic_fetch_add(&t->total, 1);
    pthread_mutex_unlock(&b->lock);

    while (atomic_load(&t->total) > atomic_load(&t->max_entries) && evict_oldest(t) == 0)
    {
    }
    return MAC_OK;
}

static int sweep(struct mac_table *t, int port, long older_than)
{
    struct mac_entry **pp;
    long now = mono_sec();
    int i, n = 0;

    for (i = 0; i < MAC_BUCKETS; i++)
    {
        pthread_mutex_lock(&t->bucket[i].lock);
        pp = &t->bucket[i].head;
        while (*pp != NULL)
        {
            if ((port < 0 || (*pp)->port == port) && now - (*pp)->last_seen > older_than)
            {
                unlink_entry(t, pp);
                n++;
            }
            else
            {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&t->bucket[i].lock);
    }
    return n;
}

int mac_table_age(struct mac_table *t)
{
    return sweep(t, -1, atomic_load(&t->age_seconds));
}

int mac_table_clear(struct mac_table *t)
{
    return sweep(t, -1, -1);
}

int mac_table_flush_port(struct mac_table *t, int port)
{
    return sweep(t, port, -1);
}

void mac_table_foreach(struct mac_table *t, mac_visit_fn fn, void *arg)
{
    struct mac_row *rows = NULL, *grown;
    struct mac_entry *e;
    size_t n = 0, cap = 0, i;
    long now = mono_sec();
    int b;

    for (b = 0; b < MAC_BUCKETS; b++)
    {
        pthread_mutex_lock(&t->bucket[b].lock);
        for (e = t->bucket[b].head; e != NULL; e = e->next)
        {
            if (n == cap)
            {
                cap = cap ? cap * 2 : 64;
                grown = realloc(rows, cap * sizeof *rows);
                if (grown == NULL)
                {
                    pthread_mutex_unlock(&t->bucket[b].lock);
                    goto out;
                }
                rows = grown;
            }
            rows[n].vlan = e->vlan;
            memcpy(rows[n].mac, e->mac, MAC_LEN);
            rows[n].port = e->port;
            rows[n].age = now - e->last_seen;
            n++;
        }
        pthread_mutex_unlock(&t->bucket[b].lock);
    }
out:
    for (i = 0; i < n; i++)
    {
        fn(rows[i].vlan, rows[i].mac, rows[i].port, rows[i].age, arg);
    }
    free(rows);
}