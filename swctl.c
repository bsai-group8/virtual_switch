#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "shm_stats.h"

#define SOCK_PATH "/tmp/vswitch.sock"

static const char *state_name[] = {"free", "up", "down"};

static int stats_cmd(int watch)
{
    struct shm_stats *s = shm_stats_attach();
    struct shm_stats copy;
    int i;

    if (s == NULL)
    {
        fprintf(stderr, "swctl: no shared memory, is the switch running?\n");
        return 1;
    }
    do
    {
        pthread_mutex_lock(&s->lock);
        copy = *s;
        pthread_mutex_unlock(&s->lock);

        if (watch)
        {
            printf("\033[H\033[J");
        }
        printf("port  state  vlan  trunk        in       out   dropped  mirrored\n");
        for (i = 0; i < copy.nports; i++)
        {
            printf("%4d  %-5s  %4d  %5s  %8lu  %8lu  %8lu  %8lu\n", i + 1,
                   state_name[copy.port[i].state], copy.port[i].vlan,
                   copy.port[i].trunk ? "yes" : "no",
                   (unsigned long)copy.port[i].count[ST_IN],
                   (unsigned long)copy.port[i].count[ST_OUT],
                   (unsigned long)copy.port[i].count[ST_DROP],
                   (unsigned long)copy.port[i].count[ST_MIRR]);
        }
        fflush(stdout);
    } while (watch && sleep(1) == 0);
    shm_stats_detach(s);
    return 0;
}

static int send_cmd(int argc, char **argv)
{
    struct sockaddr_un addr;
    char cmd[128] = "", buf[512];
    ssize_t n;
    int fd, i;

    for (i = 1; i < argc; i++)
    {
        if (i > 1)
        {
            strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        }
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
    {
        perror("swctl: cannot reach the switch");
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

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <command> ...  (see the README)\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "stats") == 0)
    {
        return stats_cmd(argc > 2 && strcmp(argv[2], "--watch") == 0);
    }
    return send_cmd(argc, argv);
}