#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "mac_table.h"
#include "shm_stats.h"

#define SOCK_PATH "/tmp/vswitch.sock"
#define DUMP_PATH "mac_dump.txt"
#define NPORTS    4

static struct mac_table  table;
static struct shm_stats *stats;
static int               listen_fd;
static sigset_t          sigs;

/* shutdown flag, protected by stop_lock */
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stop_cond = PTHREAD_COND_INITIALIZER;
static int             stopping;

static void print_mac(const uint8_t mac[6], int port, void *arg)
{
    fprintf(arg, "%02x:%02x:%02x:%02x:%02x:%02x  port %d\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], port);
}


static void *signal_main(void *arg)
{
    int sig;
    FILE *f;

    (void)arg;
    for (;;)
    {
        if (sigwait(&sigs, &sig) != 0)
        {
            continue;
        }
        if (sig == SIGUSR1)
        {
            f = fopen(DUMP_PATH, "w");
            if (f != NULL)
            {
                mac_table_foreach(&table, print_mac, f);
                fclose(f);
            }
        }
        else if (sig == SIGINT)
        {
            pthread_mutex_lock(&stop_lock);
            stopping = 1;
            pthread_cond_broadcast(&stop_cond);   /* wake waiters */
            pthread_mutex_unlock(&stop_lock);
            shutdown(listen_fd, SHUT_RDWR);       /* wake accept() */
            return NULL;
        }
    }
}


static void handle(FILE *out, const char *cmd)
{
    int i;

    if (strcmp(cmd, "show mac") == 0)
    {
        mac_table_foreach(&table, print_mac, out);
    }
    else if (strcmp(cmd, "show ports") == 0)
    {
        for (i = 0; i < stats->nports; i++)
        {
            fprintf(out, "port %d: up\n", i);
        }
    }
    else if (strcmp(cmd, "clear mac") == 0)
    {
        fprintf(out, "cleared %d entries\n", mac_table_clear(&table));
    }
    else
    {
        fprintf(out, "unknown command\n");
    }
}

static void *admin_main(void *arg)
{
    char cmd[64];
    ssize_t n;
    FILE *out;
    int fd;

    (void)arg;
    for (;;)
    {
        fd = accept(listen_fd, NULL, NULL);
        if (fd < 0)
        {
            break;                   /* socket was shut down */
        }
        n = read(fd, cmd, sizeof cmd - 1);
        if (n <= 0)
        {
            close(fd);
            continue;
        }
        cmd[n] = '\0';
        cmd[strcspn(cmd, "\r\n")] = '\0';
        out = fdopen(fd, "w");
        if (out == NULL)
        {
            close(fd);
            continue;
        }
        handle(out, cmd);
        fclose(out);
    }
    return NULL;
}


static void *traffic_main(void *arg)
{
    struct timespec ts;
    int i;

    (void)arg;
    pthread_mutex_lock(&stop_lock);
    while (!stopping)
    {
        pthread_mutex_unlock(&stop_lock);

        pthread_mutex_lock(&stats->lock);
        for (i = 0; i < stats->nports; i++)
        {
            stats->port[i].frames_in += 10 + i;
            stats->port[i].frames_out += 8 + i;
            stats->port[i].frames_dropped += i;
            stats->port[i].frames_mirrored += 1;
        }
        pthread_mutex_unlock(&stats->lock);

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_mutex_lock(&stop_lock);
        pthread_cond_timedwait(&stop_cond, &stop_lock, &ts);
    }
    pthread_mutex_unlock(&stop_lock);
    return NULL;
}

int main(void)
{
    struct sockaddr_un addr;
    pthread_t sig_th;
    pthread_t admin_th;
    pthread_t traffic_th;
    uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
    int i;

    /* block signals BEFORE creating threads so all of them inherit it */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    mac_table_init(&table);
    for (i = 0; i < 6; i++)
    {
        mac[5] = (uint8_t)i;
        mac_table_insert(&table, mac, i % NPORTS);
    }

    stats = shm_stats_create(NPORTS);
    if (stats == NULL)
    {
        return 1;
    }

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    unlink(SOCK_PATH);
    if (listen_fd < 0
        || bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0
        || listen(listen_fd, 5) != 0)
    {
        perror("socket setup");
        shm_stats_destroy(stats);
        return 1;
    }

    pthread_create(&sig_th, NULL, signal_main, NULL);
    pthread_create(&admin_th, NULL, admin_main, NULL);
    pthread_create(&traffic_th, NULL, traffic_main, NULL);
    printf("switch running, pid %d\n", (int)getpid());
    fflush(stdout);

    pthread_join(sig_th, NULL);      /* returns after SIGINT */
    pthread_join(admin_th, NULL);
    pthread_join(traffic_th, NULL);

    close(listen_fd);
    unlink(SOCK_PATH);
    shm_stats_destroy(stats);
    mac_table_destroy(&table);
    printf("clean shutdown\n");
    return 0;
}