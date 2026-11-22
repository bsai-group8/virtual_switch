#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shm_stats.h"

struct shm_stats *shm_stats_create(int nports)
{
    pthread_mutexattr_t attr;
    struct shm_stats *s;
    int fd;

    shm_unlink(SHM_NAME);
    fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
    {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(fd, sizeof *s) != 0)
    {
        perror("ftruncate");
        close(fd);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED)
    {
        perror("mmap");
        shm_unlink(SHM_NAME);
        return NULL;
    }
    if (pthread_mutexattr_init(&attr) != 0
        || pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0
        || pthread_mutex_init(&s->lock, &attr) != 0)
    {
        fprintf(stderr, "cannot create the shared mutex\n");
        munmap(s, sizeof *s);
        shm_unlink(SHM_NAME);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    s->nports = nports;
    return s;
}

struct shm_stats *shm_stats_attach(void)
{
    struct shm_stats *s;
    int fd = shm_open(SHM_NAME, O_RDWR, 0);

    if (fd < 0)
    {
        return NULL;
    }
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return s == MAP_FAILED ? NULL : s;
}

void shm_stats_detach(struct shm_stats *s)
{
    munmap(s, sizeof *s);
}

void shm_stats_destroy(struct shm_stats *s)
{
    pthread_mutex_destroy(&s->lock);
    munmap(s, sizeof *s);
    shm_unlink(SHM_NAME);
}