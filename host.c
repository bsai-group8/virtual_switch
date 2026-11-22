#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "frame.h"

#define LINGER_MS 1000

static void print_frame(const struct frame *f)
{
    char s[MAC_STR_LEN], d[MAC_STR_LEN];

    mac_format(f->src, s);
    mac_format(f->dst, d);
    printf("recv src=%s dst=%s vlan=%u pri=%u \"%.*s\"\n", s, d, f->vlan, f->prio,
           (int)f->len, (const char *)f->payload);
    fflush(stdout);
}

static int send_line(int sock, const uint8_t *mac, int vlan, int prio, char *line)
{
    struct frame *f;
    char *text = strchr(line, ' ');
    size_t len;
    int rc;

    if (text == NULL)
    {
        fprintf(stderr, "usage: <dst_mac|bcast> <text>\n");
        return 0;
    }
    *text++ = '\0';
    len = strlen(text);
    if (len > MAX_PAYLOAD)
    {
        len = MAX_PAYLOAD;
    }
    f = frame_new((uint16_t)len);
    if (f == NULL)
    {
        return -1;
    }
    memcpy(f->src, mac, MAC_LEN);
    if (strcmp(line, "bcast") == 0)
    {
        memset(f->dst, 0xff, MAC_LEN);
    }
    else if (mac_parse(line, f->dst) != 0)
    {
        fprintf(stderr, "bad destination mac\n");
        frame_unref(f);
        return 0;
    }
    f->vlan = (uint16_t)vlan;
    f->prio = (uint8_t)prio;
    memcpy(f->payload, text, len);
    rc = frame_send(sock, f);
    frame_unref(f);
    return rc;
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    struct pollfd pf[2];
    struct frame *f;
    uint8_t mac[MAC_LEN];
    char line[MAX_PAYLOAD + 64];
    int sock, vlan = 1, prio = 1, one = 1;
    int stdin_open = 1, frozen = 0, n;

    if (argc < 4 || argc > 6 || mac_parse(argv[3], mac) != 0)
    {
        fprintf(stderr, "usage: %s <switch_ip> <tcp_port> <mac> [vlan [priority]]\n", argv[0]);
        return 1;
    }
    if (argc > 4)
    {
        vlan = atoi(argv[4]);
    }
    if (argc > 5)
    {
        prio = atoi(argv[5]);
    }
    signal(SIGPIPE, SIG_IGN);

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)atoi(argv[2]));
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1
        || connect(sock, (struct sockaddr *)&addr, sizeof addr) != 0)
    {
        perror("connect");
        return 1;
    }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setvbuf(stdin, NULL, _IONBF, 0);

    for (;;)
    {
        pf[0].fd = stdin_open ? 0 : -1;
        pf[0].events = POLLIN;
        pf[1].fd = frozen ? -1 : sock;
        pf[1].events = POLLIN;
        n = poll(pf, 2, stdin_open ? -1 : LINGER_MS);
        if (n == 0)
        {
            break;
        }
        if (n < 0)
        {
            perror("poll");
            break;
        }
        if (pf[1].revents)
        {
            if (frame_recv(sock, &f) != 1)
            {
                printf("disconnected\n");
                break;
            }
            print_frame(f);
            frame_unref(f);
        }
        if (pf[0].revents)
        {
            if (fgets(line, sizeof line, stdin) == NULL)
            {
                stdin_open = 0;
                continue;
            }
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, "freeze") == 0)
            {
                frozen = 1;
            }
            else if (line[0] != '\0' && send_line(sock, mac, vlan, prio, line) != 0)
            {
                printf("send failed\n");
                break;
            }
        }
    }
    close(sock);
    return 0;
}