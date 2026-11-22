#ifndef SHM_STATS_H
#define SHM_STATS_H

#include <pthread.h>
#include <stdint.h>

#define SHM_NAME  "/vswitch_stats"
#define MAX_PORTS 64

enum { ST_IN, ST_OUT, ST_DROP, ST_MIRR, ST_COUNT };
enum { PSTATE_FREE, PSTATE_UP, PSTATE_DOWN };

struct port_stats
{
    uint64_t count[ST_COUNT];
    int32_t  state;
    int32_t  vlan;
    int32_t  trunk;
    int32_t  pad;
};

struct shm_stats
{
    pthread_mutex_t   lock;
    int32_t           nports;
    int32_t           pad;
    struct port_stats port[MAX_PORTS];
};

struct shm_stats *shm_stats_create(int nports);
struct shm_stats *shm_stats_attach(void);
void shm_stats_detach(struct shm_stats *s);
void shm_stats_destroy(struct shm_stats *s);

#endif