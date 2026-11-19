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
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stop_cond = PTHREAD_COND_INITIALIZER;
static int             stopping;
 

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
            pthread_cond_broadcast(&stop_cond);
            pthread_mutex_unlock(&stop_lock);
            shutdown(listen_fd, SHUT_RDWR);       
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
