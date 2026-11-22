#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "frame.h"
#include "shm_stats.h"

#define MAC_BUCKETS 64

enum { MAC_OK = 0, MAC_MOVED, MAC_SECURITY };

struct mac_entry
{
    uint8_t           mac[MAC_LEN];
    uint16_t          vlan;
    int               port;
    long              last_seen;
    struct mac_entry *next;
};

struct mac_bucket
{
    pthread_mutex_t   lock;
    struct mac_entry *head;
};

struct mac_table
{
    struct mac_bucket bucket[MAC_BUCKETS];
    atomic_int        total;
    atomic_int        max_entries;
    atomic_int        age_seconds;
    atomic_int        on_port[MAX_PORTS];
};

typedef void (*mac_visit_fn)(uint16_t vlan, const uint8_t mac[MAC_LEN],
                             int port, long age, void *arg);

int  mac_table_init(struct mac_table *t, int max_entries, int age_seconds);
void mac_table_destroy(struct mac_table *t);

int mac_table_lookup(struct mac_table *t, uint16_t vlan, const uint8_t mac[MAC_LEN]);

int mac_table_learn(struct mac_table *t, uint16_t vlan, const uint8_t mac[MAC_LEN],
                    int port, int limit, int *old);

int  mac_table_age(struct mac_table *t);
int  mac_table_clear(struct mac_table *t);
int  mac_table_flush_port(struct mac_table *t, int port);
void mac_table_foreach(struct mac_table *t, mac_visit_fn fn, void *arg);

#endif