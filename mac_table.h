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

void mac_table_init(struct mac_table *t);
void mac_table_destroy(struct mac_table *t);
int  mac_table_lookup(struct mac_table *t, const uint8_t mac[6]);
void mac_table_insert(struct mac_table *t, const uint8_t mac[6], int port);

#endif