#define _POSIX_C_SOURCE 200809L

#include "shm_stats.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct shm_stats *shm_stats_create(int nports)
{
    struct shm_stats *s;
    pthread_mutexattr_t attr;
    int fd;

    fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0)
    {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof *s) != 0)
    {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED)
    {
        perror("mmap");
        return NULL;
    }

    memset(s, 0, sizeof *s);
    s->nports = nports;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    return s;
}

struct shm_stats *shm_stats_attach(void)
{
    struct shm_stats *s;
    int fd;

    fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd < 0)
    {
        return NULL;
    }
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return s == MAP_FAILED ? NULL : s;
}

void shm_stats_destroy(struct shm_stats *s)
{
    pthread_mutex_destroy(&s->lock);
    munmap(s, sizeof *s);
    shm_unlink(SHM_NAME);
}