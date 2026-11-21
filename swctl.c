#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "shm_stats.h"

#define SOCK_PATH "/tmp/vswitch.sock"

static volatile sig_atomic_t quit;

static void on_sigint(int sig)
{
    (void)sig;
    quit = 1;
}

/* send one command over the UNIX socket and print the reply */
static int send_command(const char *cmd)
{
    struct sockaddr_un addr;
    char buf[512];
    ssize_t n;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
    {
        fprintf(stderr, "swctl: cannot connect (is ./switch running?)\n");
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0)
    {
        perror("write");
        close(fd);
        return 1;
    }
    while ((n = read(fd, buf, sizeof buf)) > 0)
    {
        fwrite(buf, 1, (size_t)n, stdout);
    }
    close(fd);
    return 0;
}

/* read the shared memory directly, no socket involved */
static int stats_watch(void)
{
    struct shm_stats *s = shm_stats_attach();
    struct port_stats p[MAX_PORTS];
    struct sigaction sa;
    struct timespec one_sec = {1, 0};
    int n;
    int i;

    if (s == NULL)
    {
        fprintf(stderr, "swctl: cannot attach (is ./switch running?)\n");
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);    /* Ctrl-C ends the loop */

    while (!quit)
    {
        pthread_mutex_lock(&s->lock);
        n = s->nports;
        memcpy(p, s->port, sizeof p);
        pthread_mutex_unlock(&s->lock);

        printf("PORT  IN  OUT  DROPPED  MIRRORED\n");
        for (i = 0; i < n; i++)
        {
            printf("%-4d %4llu %4llu %7llu %9llu\n", i,
                   (unsigned long long)p[i].frames_in,
                   (unsigned long long)p[i].frames_out,
                   (unsigned long long)p[i].frames_dropped,
                   (unsigned long long)p[i].frames_mirrored);
        }
        printf("\n");
        fflush(stdout);
        nanosleep(&one_sec, NULL);
    }
    munmap(s, sizeof *s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "show") == 0
        && strcmp(argv[2], "mac") == 0)
    {
        return send_command("show mac");
    }
    if (argc == 3 && strcmp(argv[1], "show") == 0
        && strcmp(argv[2], "ports") == 0)
    {
        return send_command("show ports");
    }
    if (argc == 3 && strcmp(argv[1], "clear") == 0
        && strcmp(argv[2], "mac") == 0)
    {
        return send_command("clear mac");
    }
    if (argc == 3 && strcmp(argv[1], "stats") == 0
        && strcmp(argv[2], "--watch") == 0)
    {
        return stats_watch();
    }
    fprintf(stderr, "usage: swctl show mac | show ports | clear mac | "
            "stats --watch\n");
    return 2;
}