#ifndef SHM_STATS_H
#define SHM_STATS_H
#include <pthread.h>
#include <stdint.h>
#define SHM_NAME  "/vswitch_stats"
#define MAX_PORTS 8

struct port_stats
{
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t frames_dropped;
    uint64_t frames_mirrored;
};
struct shm_stats
{
    pthread_mutex_t   lock;     
    int               nports;
    struct port_stats port[MAX_PORTS];
};
struct shm_stats *shm_stats_create(int nports);
struct shm_stats *shm_stats_attach(void);
void shm_stats_destroy(struct shm_stats *s);

#endif