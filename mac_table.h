#ifndef MAC_TABLE_H
#define MAC_TABLE_H

#include <pthread.h>
#include <stdint.h>
#define NBUCKETS 16

struct mac_entry
{
    uint8_t mac[6];
    int port;
    struct mac_entry *next;
};
struct mac_bucket
{
    pthread_mutex_t lock;
    struct mac_entry *head;
};
struct mac_table
{
    struct mac_bucket bucket[NBUCKETS];
};
typedef void (*mac_visit_fn)(const uint8_t mac[6], int port, void *arg);

void mac_table_init(struct mac_table *t);
void mac_table_destroy(struct mac_table *t);
int  mac_table_lookup(struct mac_table *t, const uint8_t mac[6]);
void mac_table_insert(struct mac_table *t, const uint8_t mac[6], int port);

/*
 * Walk every entry.  One bucket is locked at a time, so a walk never
 * blocks the whole table and never nests two bucket locks.
 */
void mac_table_foreach(struct mac_table *t, mac_visit_fn fn, void *arg);

/* Drop every entry, return how many were removed. */
int mac_table_clear(struct mac_table *t);

#endif