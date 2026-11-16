#include "mac_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bucket_of(const uint8_t mac[6])
{
    unsigned h = 0;
    int i;

    for (i = 0; i < 6; i++)
    {
        h = h * 31 + mac[i];
    }
    return h % NBUCKETS;
}
static struct mac_entry *find(struct mac_bucket *b, const uint8_t mac[6])
{
    struct mac_entry *e;
    for (e = b->head; e != NULL; e = e->next)
    {
        if (memcmp(e->mac, mac, 6) == 0)
        {
            return e;
        }
    }
    return NULL;
}
void mac_table_init(struct mac_table *t)
{
    int i;

    for (i = 0; i < NBUCKETS; i++)
    {
        t->bucket[i].head = NULL;
        if (pthread_mutex_init(&t->bucket[i].lock, NULL) != 0)
        {
            perror("pthread_mutex_init");
            exit(1);
        }
    }
}
void mac_table_destroy(struct mac_table *t)
{
    struct mac_entry *e;
    struct mac_entry *next;
    int i;

    for (i = 0; i < NBUCKETS; i++)
    {
        for (e = t->bucket[i].head; e != NULL; e = next)
        {
            next = e->next;
            free(e);
        }
        pthread_mutex_destroy(&t->bucket[i].lock);
    }
}

int mac_table_lookup(struct mac_table *t, const uint8_t mac[6])
{
    struct mac_bucket *b = &t->bucket[bucket_of(mac)];
    struct mac_entry *e;
    int port = -1;
    pthread_mutex_lock(&b->lock);
    e = find(b, mac);
    if (e != NULL)
    {
        port = e->port;
    }
    pthread_mutex_unlock(&b->lock);
    return port;
}
void mac_table_insert(struct mac_table *t, const uint8_t mac[6], int port)
{
    struct mac_bucket *b = &t->bucket[bucket_of(mac)];
    struct mac_entry *e;
    int old_port = -1;

    pthread_mutex_lock(&b->lock);
    e = find(b, mac);
    if (e == NULL)
    {
        e = malloc(sizeof *e);
        if (e == NULL)
        {
            perror("malloc");
            exit(1);
        }
        memcpy(e->mac, mac, 6);
        e->port = port;
        e->next = b->head;
        b->head = e;
    }
    else if (e->port != port)
    {
        old_port = e->port;
        e->port = port;
    }
    pthread_mutex_unlock(&b->lock);
    if (old_port != -1)
    {
        printf("MAC moved: port %d -> port %d\n", old_port, port);
    }
}