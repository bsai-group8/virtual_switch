/*
 * host - a test host for the virtual switch.
 *
 *   ./host <switch_ip> <tcp_port> <mac>
 *
 * Two threads:
 *   main      reads commands from stdin and writes frames to the socket
 *   receiver  reads frames from the socket and prints them
 *
 * stdin commands:
 *   send <dst_mac> <text>      unicast
 *   bcast <text>               broadcast (ff:ff:ff:ff:ff:ff)
 *   prio <0|1|2>               priority of later frames (0 high .. 2 low)
 *   vlan <1..4094>             vlan tag of later frames
 *   flood <count> <dst_mac>    blast <count> frames, for load tests
 *   sleep <ms>                 go deaf for a while (tests head of line
 *                              blocking: the other hosts must keep running)
 *   quit
 */

#include "frame.h"
#include "mac_table.h"
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int     sock_fd = -1;
static uint8_t my_mac[MAC_LEN];

/* Guards writes to the socket: main and the sleep command both touch it. */
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t bcast_mac[MAC_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static void *receiver_main(void *arg)
{
    struct frame *f;
    char          src[MAC_STR_LEN];
    char          dst[MAC_STR_LEN];
    int           rc;

    (void)arg;
    for (;;)
    {
        rc = frame_recv(sock_fd, &f);
        if (rc <= 0)
        {
            if (rc == 0)
            {
                fprintf(stderr, "host: switch closed the connection\n");
            }
            else
            {
                fprintf(stderr, "host: receive error: %s\n", strerror(errno));
            }
            break;
        }
        mac_format(f->hdr.src, src);
        mac_format(f->hdr.dst, dst);
        printf("RX  src=%s dst=%s vlan=%u prio=%u len=%u  \"%.*s\"\n", src, dst,
               (unsigned)f->hdr.vlan, (unsigned)f->hdr.priority,
               (unsigned)f->hdr.length, (int)f->hdr.length,
               (const char *)f->payload);
        fflush(stdout);
        frame_unref(f);
    }
    return NULL;
}

static int send_one(const uint8_t dst[MAC_LEN], uint16_t vlan, uint8_t prio,
                    const char *text)
{
    struct mera_frame_header h;
    struct frame            *f;
    size_t                   len = strlen(text);
    int                      rc;

    if (len > FRAME_MAX_PAYLOAD)
    {
        len = FRAME_MAX_PAYLOAD;
    }
    memset(&h, 0, sizeof h);
    h.length = (uint16_t)len;
    memcpy(h.src, my_mac, MAC_LEN);
    memcpy(h.dst, dst, MAC_LEN);
    h.vlan     = vlan;
    h.priority = prio;

    f = frame_new(&h, (const uint8_t *)text);
    if (f == NULL)
    {
        fprintf(stderr, "host: out of memory\n");
        return -1;
    }
    pthread_mutex_lock(&send_lock);
    rc = frame_send(sock_fd, f);
    pthread_mutex_unlock(&send_lock);
    frame_unref(f);

    if (rc != 0)
    {
        fprintf(stderr, "host: send failed: %s\n", strerror(errno));
    }
    return rc;
}

static int connect_to_switch(const char *ip, const char *port)
{
    struct addrinfo  hints;
    struct addrinfo *res;
    struct addrinfo *ai;
    int              fd = -1;
    int              rc;

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    rc = getaddrinfo(ip, port, &hints, &res);
    if (rc != 0)
    {
        fprintf(stderr, "host: getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
    {
        fprintf(stderr, "host: cannot connect to %s:%s\n", ip, port);
    }
    return fd;
}

int main(int argc, char **argv)
{
    pthread_t rx_th;
    char      line[2048];
    char      arg[256];
    uint8_t   dst[MAC_LEN];
    uint16_t  vlan = 1;
    uint8_t   prio = PRIO_NORMAL;
    int       n;
    int       i;

    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <switch_ip> <tcp_port> <mac>\n", argv[0]);
        return 2;
    }
    if (mac_parse(argv[3], my_mac) != 0)
    {
        fprintf(stderr, "host: bad mac '%s', expected aa:bb:cc:dd:ee:ff\n",
                argv[3]);
        return 2;
    }

    /* A switch that goes away must give us EPIPE, not kill us. */
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        perror("signal");
        return 1;
    }

    sock_fd = connect_to_switch(argv[1], argv[2]);
    if (sock_fd < 0)
    {
        return 1;
    }
    if (pthread_create(&rx_th, NULL, receiver_main, NULL) != 0)
    {
        perror("pthread_create");
        close(sock_fd);
        return 1;
    }
    printf("host %s connected to %s:%s\n", argv[3], argv[1], argv[2]);
    fflush(stdout);

    while (fgets(line, sizeof line, stdin) != NULL)
    {
        line[strcspn(line, "\r\n")] = '\0';

        if (strcmp(line, "quit") == 0)
        {
            break;
        }
        if (sscanf(line, "prio %d", &n) == 1 && n >= 0 && n < NUM_PRIO)
        {
            prio = (uint8_t)n;
            continue;
        }
        if (sscanf(line, "vlan %d", &n) == 1 && n >= VLAN_MIN && n <= VLAN_MAX)
        {
            vlan = (uint16_t)n;
            continue;
        }
        if (sscanf(line, "sleep %d", &n) == 1 && n > 0)
        {
            struct timespec ts;
            ts.tv_sec  = n / 1000;
            ts.tv_nsec = (long)(n % 1000) * 1000000L;
            nanosleep(&ts, NULL);
            continue;
        }
        if (sscanf(line, "flood %d %31s", &n, arg) == 2)
        {
            if (mac_parse(arg, dst) != 0)
            {
                fprintf(stderr, "host: bad mac '%s'\n", arg);
                continue;
            }
            for (i = 0; i < n; i++)
            {
                char body[64];
                snprintf(body, sizeof body, "flood %d", i);
                if (send_one(dst, vlan, prio, body) != 0)
                {
                    break;
                }
            }
            continue;
        }
        if (strncmp(line, "bcast ", 6) == 0)
        {
            send_one(bcast_mac, vlan, prio, line + 6);
            continue;
        }
        if (strncmp(line, "send ", 5) == 0)
        {
            char *sp;
            if (sscanf(line + 5, "%31s", arg) != 1
                || mac_parse(arg, dst) != 0)
            {
                fprintf(stderr, "host: usage: send <dst_mac> <text>\n");
                continue;
            }
            sp = strchr(line + 5, ' ');
            send_one(dst, vlan, prio, sp != NULL ? sp + 1 : "");
            continue;
        }
        fprintf(stderr, "host: unknown command '%s'\n", line);
    }

    shutdown(sock_fd, SHUT_RDWR); /* unblocks the receiver thread */
    pthread_join(rx_th, NULL);
    close(sock_fd);
    pthread_mutex_destroy(&send_lock);
    return 0;
}