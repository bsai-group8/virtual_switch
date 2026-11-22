#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "frame.h"
#include "logger.h"
#include "mac_table.h"
#include "queue.h"
#include "shm_stats.h"

#define ACCEPT_WAIT 5
#define TICK_MS     100
#define SOCK_PATH   "/tmp/vswitch.sock"
#define DUMP_PATH   "mac_dump.txt"
#define LOG_PATH    "switch.log"

struct port
{
    int        idx;
    int        fd;
    int        in_use;
    int        admin_down;
    atomic_int vlan;
    atomic_int trunk;
    atomic_int mirror_to;
    pthread_t  tx;

    struct port_queue q;
};

static struct port       ports[MAX_PORTS];
static int               nports;
static pthread_mutex_t   ports_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t             free_ports;
static struct mac_table  table;
static struct shm_stats *stats;
static atomic_int        rate;
static atomic_int        port_macs;
static int               tcp_fd, un_fd;
static const char       *conf_path;
static sigset_t          sigs;

static pthread_mutex_t   stop_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    stop_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t    live_cond = PTHREAD_COND_INITIALIZER;
static atomic_int        stopping;
static int               live;

static void stat_inc(int port, int which)
{
    pthread_mutex_lock(&stats->lock);
    stats->port[port].count[which]++;
    pthread_mutex_unlock(&stats->lock);
}

static void publish(struct port *p)
{
    struct port_stats *s = &stats->port[p->idx];

    pthread_mutex_lock(&stats->lock);
    s->state = p->admin_down ? PSTATE_DOWN : (p->in_use ? PSTATE_UP : PSTATE_FREE);
    s->vlan = atomic_load(&p->vlan);
    s->trunk = atomic_load(&p->trunk);
    pthread_mutex_unlock(&stats->lock);
}

static struct port *port_of(int n)
{
    return (n >= 1 && n <= nports) ? &ports[n - 1] : NULL;
}

static int push_one(struct port *p, struct frame *f)
{
    int rc;

    frame_ref(f);
    rc = queue_push(&p->q, f);
    if (rc != 0)
    {
        frame_unref(f);
    }
    if (rc < 0)
    {
        stat_inc(p->idx, ST_DROP);
    }
    return rc;
}

static int send_to(struct port *p, struct frame *f)
{
    int rc = push_one(p, f);
    int m = atomic_load(&p->mirror_to);

    if (rc == 0 && m >= 0 && m != p->idx && push_one(&ports[m], f) == 0)
    {
        stat_inc(p->idx, ST_MIRR);
    }
    return rc;
}

static void *tx_main(void *arg)
{
    struct port *p = arg;
    struct frame *f;

    while ((f = queue_pop(&p->q, &rate)) != NULL)
    {
        if (frame_send(p->fd, f) == 0)
        {
            stat_inc(p->idx, ST_OUT);
        }
        else
        {
            shutdown(p->fd, SHUT_RDWR);
        }
        frame_unref(f);
    }
    return NULL;
}

static void port_shutdown(struct port *p)
{
    pthread_mutex_lock(&ports_lock);
    p->admin_down = 1;
    if (p->in_use)
    {
        shutdown(p->fd, SHUT_RDWR);
    }
    publish(p);
    pthread_mutex_unlock(&ports_lock);
}

static int forward(struct port *in, struct frame *f)
{
    int trunk = atomic_load(&in->trunk);
    int vlan = trunk ? f->vlan : atomic_load(&in->vlan);
    int limit = trunk ? 0 : atomic_load(&port_macs);
    int out, old, i, m;

    if (vlan < 1 || vlan > 4094)
    {
        stat_inc(in->idx, ST_DROP);
        return 0;
    }
    f->vlan = (uint16_t)vlan;

    if (!mac_is_broadcast(f->src))
    {
        switch (mac_table_learn(&table, (uint16_t)vlan, f->src, in->idx, limit, &old))
        {
        case MAC_MOVED:
            log_msg("MAC moved: port %d -> port %d", old + 1, in->idx + 1);
            break;
        case MAC_SECURITY:
            log_msg("port %d over its MAC limit, shutting it down", in->idx + 1);
            port_shutdown(in);
            return -1;
        default:
            break;
        }
    }

    out = mac_is_broadcast(f->dst) ? -1 : mac_table_lookup(&table, (uint16_t)vlan, f->dst);
    if (out >= 0)
    {
        if (out != in->idx)
        {
            send_to(&ports[out], f);
        }
    }
    else
    {
        for (i = 0; i < nports; i++)
        {
            if (i != in->idx && (atomic_load(&ports[i].trunk) || atomic_load(&ports[i].vlan) == vlan))
            {
                send_to(&ports[i], f);
            }
        }
    }

    m = atomic_load(&in->mirror_to);
    if (m >= 0 && m != in->idx && push_one(&ports[m], f) == 0)
    {
        stat_inc(in->idx, ST_MIRR);
    }
    return 0;
}

static void send_full(int fd)
{
    struct frame *f = frame_new(11);

    if (f != NULL)
    {
        memcpy(f->payload, "switch full", 11);
        frame_send(fd, f);
        frame_unref(f);
    }
}

static void live_dec(void)
{
    pthread_mutex_lock(&stop_lock);
    if (--live == 0)
    {
        pthread_cond_broadcast(&live_cond);
    }
    pthread_mutex_unlock(&stop_lock);
}

static void free_port(struct port *p)
{
    pthread_mutex_lock(&ports_lock);
    close(p->fd);
    p->in_use = 0;
    publish(p);
    pthread_mutex_unlock(&ports_lock);
    sem_post(&free_ports);
}

static struct port *claim(int fd)
{
    struct port *p = NULL;
    int i;

    pthread_mutex_lock(&ports_lock);
    for (i = 0; i < nports && !atomic_load(&stopping); i++)
    {
        if (!ports[i].in_use && !ports[i].admin_down)
        {
            p = &ports[i];
            p->in_use = 1;
            p->fd = fd;
            break;
        }
    }
    pthread_mutex_unlock(&ports_lock);
    if (p == NULL)
    {
        sem_post(&free_ports);
        send_full(fd);
        close(fd);
    }
    return p;
}

static struct port *wait_for_port(int fd)
{
    struct timespec ts;

    ts_in_ms(&ts, ACCEPT_WAIT * 1000);
    while (sem_timedwait(&free_ports, &ts) != 0)
    {
        if (errno != EINTR)
        {
            send_full(fd);
            close(fd);
            return NULL;
        }
    }
    return claim(fd);
}

static int start_port(struct port *p)
{
    queue_open(&p->q, atomic_load(&rate));
    if (pthread_create(&p->tx, NULL, tx_main, p) != 0)
    {
        queue_close(&p->q);
        free_port(p);
        return -1;
    }
    pthread_mutex_lock(&ports_lock);
    publish(p);
    pthread_mutex_unlock(&ports_lock);
    log_msg("port %d connected", p->idx + 1);
    return 0;
}

static void release(struct port *p)
{
    queue_close(&p->q);
    shutdown(p->fd, SHUT_RDWR);
    pthread_join(p->tx, NULL);
    queue_drain(&p->q);
    mac_table_flush_port(&table, p->idx);
    free_port(p);
    log_msg("port %d disconnected", p->idx + 1);
}

struct job
{
    int          fd;
    struct port *p;
};

static void *conn_main(void *arg)
{
    struct job *j = arg;
    struct port *p = j->p != NULL ? j->p : wait_for_port(j->fd);
    struct frame *f;
    int rc;

    free(j);
    if (p != NULL && start_port(p) == 0)
    {
        while (frame_recv(p->fd, &f) == 1)
        {
            stat_inc(p->idx, ST_IN);
            rc = forward(p, f);
            frame_unref(f);
            if (rc != 0)
            {
                break;
            }
        }
        release(p);
    }
    live_dec();
    return NULL;
}

static void *accept_main(void *arg)
{
    pthread_attr_t attr;
    pthread_t t;
    struct job *j;
    int fd, one = 1, sndbuf = 32768;

    (void)arg;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    for (;;)
    {
        fd = accept(tcp_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
            {
                continue;
            }
            break;
        }
        if (atomic_load(&stopping))
        {
            close(fd);
            break;
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
        j = malloc(sizeof *j);
        if (j == NULL)
        {
            close(fd);
            continue;
        }
        j->fd = fd;
        j->p = NULL;
        if (sem_trywait(&free_ports) == 0 && (j->p = claim(fd)) == NULL)
        {
            free(j);
            continue;
        }
        pthread_mutex_lock(&stop_lock);
        live++;
        pthread_mutex_unlock(&stop_lock);
        if (pthread_create(&t, &attr, conn_main, j) != 0)
        {
            live_dec();
            if (j->p != NULL)
            {
                free_port(j->p);
            }
            else
            {
                close(fd);
            }
            free(j);
        }
    }
    pthread_attr_destroy(&attr);
    return NULL;
}

static void print_mac(uint16_t vlan, const uint8_t mac[MAC_LEN], int port, long age, void *arg)
{
    char s[MAC_STR_LEN];

    mac_format(mac, s);
    fprintf(arg, "vlan %-4u %s  port %d  age %lds\n", vlan, s, port + 1, age);
}

static void show_ports(FILE *out)
{
    struct { int state, vlan, trunk, mirror; } snap[MAX_PORTS];
    int i;

    pthread_mutex_lock(&ports_lock);
    for (i = 0; i < nports; i++)
    {
        snap[i].state = ports[i].admin_down ? 2 : ports[i].in_use;
        snap[i].vlan = atomic_load(&ports[i].vlan);
        snap[i].trunk = atomic_load(&ports[i].trunk);
        snap[i].mirror = atomic_load(&ports[i].mirror_to);
    }
    pthread_mutex_unlock(&ports_lock);
    for (i = 0; i < nports; i++)
    {
        fprintf(out, "port %d: %-4s vlan %d%s", i + 1,
                snap[i].state == 2 ? "down" : (snap[i].state ? "up" : "free"),
                snap[i].vlan, snap[i].trunk ? " trunk" : "");
        if (snap[i].mirror >= 0)
        {
            fprintf(out, "  mirror -> port %d", snap[i].mirror + 1);
        }
        fprintf(out, "\n");
    }
}

static void port_changed(struct port *p)
{
    mac_table_flush_port(&table, p->idx);
    pthread_mutex_lock(&ports_lock);
    publish(p);
    pthread_mutex_unlock(&ports_lock);
}

static int exec_cmd(FILE *out, const char *line)
{
    struct port *p;
    char word[16];
    int a, b, i;

    if (strcmp(line, "show mac") == 0)
    {
        mac_table_foreach(&table, print_mac, out);
    }
    else if (strcmp(line, "show ports") == 0)
    {
        show_ports(out);
    }
    else if (strcmp(line, "clear mac") == 0)
    {
        fprintf(out, "cleared %d entries\n", mac_table_clear(&table));
    }
    else if (sscanf(line, "shutdown %d", &a) == 1 && (p = port_of(a)) != NULL)
    {
        port_shutdown(p);
        fprintf(out, "port %d shut down\n", a);
    }
    else if (sscanf(line, "up %d", &a) == 1 && (p = port_of(a)) != NULL)
    {
        pthread_mutex_lock(&ports_lock);
        p->admin_down = 0;
        publish(p);
        pthread_mutex_unlock(&ports_lock);
        fprintf(out, "port %d up\n", a);
    }
    else if (sscanf(line, "vlan %d %d", &a, &b) == 2 && (p = port_of(a)) != NULL
             && b >= 1 && b <= 4094)
    {
        atomic_store(&p->vlan, b);
        port_changed(p);
        fprintf(out, "port %d is in vlan %d\n", a, b);
    }
    else if (sscanf(line, "trunk %d", &a) == 1 && (p = port_of(a)) != NULL)
    {
        b = strstr(line, "off") == NULL;
        for (i = 0; i < nports && b; i++)
        {
            if (atomic_exchange(&ports[i].trunk, 0))
            {
                port_changed(&ports[i]);
            }
        }
        atomic_store(&p->trunk, b);
        port_changed(p);
        fprintf(out, "port %d trunk %s\n", a, b ? "on" : "off");
    }
    else if (sscanf(line, "mirror %d %15s", &a, word) == 2 && (p = port_of(a)) != NULL)
    {
        b = strcmp(word, "off") == 0 ? 0 : atoi(word);
        if (b != 0 && (port_of(b) == NULL || b == a))
        {
            fprintf(out, "error: bad sniffer port\n");
            return -1;
        }
        atomic_store(&p->mirror_to, b - 1);
        fprintf(out, "port %d mirrored to %d\n", a, b);
    }
    else if (sscanf(line, "rate %d", &a) == 1 && a >= 0)
    {
        atomic_store(&rate, a);
    }
    else if (sscanf(line, "age %d", &a) == 1 && a > 0)
    {
        atomic_store(&table.age_seconds, a);
    }
    else if (sscanf(line, "maxmacs %d", &a) == 1 && a > 0)
    {
        atomic_store(&table.max_entries, a);
    }
    else if (sscanf(line, "portmacs %d", &a) == 1 && a > 0)
    {
        atomic_store(&port_macs, a);
    }
    else
    {
        fprintf(out, "error: bad command\n");
        return -1;
    }
    return 0;
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
        fd = accept(un_fd, NULL, NULL);
        if (fd < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
            {
                continue;
            }
            break;
        }
        n = read(fd, cmd, sizeof cmd - 1);
        out = n > 0 ? fdopen(fd, "w") : NULL;
        if (out == NULL)
        {
            close(fd);
            continue;
        }
        cmd[n] = '\0';
        cmd[strcspn(cmd, "\r\n")] = '\0';
        exec_cmd(out, cmd);
        fclose(out);
    }
    return NULL;
}

static void reload_config(void)
{
    char line[128];
    FILE *f, *null;

    if (conf_path == NULL || (f = fopen(conf_path, "r")) == NULL)
    {
        return;
    }
    null = fopen("/dev/null", "w");
    while (null != NULL && fgets(line, sizeof line, f) != NULL)
    {
        line[strcspn(line, "#\r\n")] = '\0';
        if (line[0] != '\0' && exec_cmd(null, line) != 0)
        {
            log_msg("config: bad line '%.60s'", line);
        }
    }
    if (null != NULL)
    {
        fclose(null);
    }
    fclose(f);
    log_msg("config loaded");
}

static void dump_macs(void)
{
    FILE *f = fopen(DUMP_PATH, "w");

    if (f != NULL)
    {
        mac_table_foreach(&table, print_mac, f);
        fclose(f);
        log_msg("MAC table dumped to %s", DUMP_PATH);
    }
}

static void begin_shutdown(void)
{
    int i;

    pthread_mutex_lock(&stop_lock);
    atomic_store(&stopping, 1);
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_lock);
    shutdown(tcp_fd, SHUT_RDWR);
    shutdown(un_fd, SHUT_RDWR);
    pthread_mutex_lock(&ports_lock);
    for (i = 0; i < nports; i++)
    {
        if (ports[i].in_use)
        {
            shutdown(ports[i].fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&ports_lock);
}

static void *signal_main(void *arg)
{
    int sig;

    (void)arg;
    for (;;)
    {
        if (sigwait(&sigs, &sig) != 0)
        {
            continue;
        }
        if (sig == SIGHUP)
        {
            reload_config();
        }
        else if (sig == SIGUSR1)
        {
            dump_macs();
        }
        else
        {
            begin_shutdown();
            return NULL;
        }
    }
}

static void *timer_main(void *arg)
{
    struct timespec ts;
    int tick = 0, i, r;

    (void)arg;
    pthread_mutex_lock(&stop_lock);
    while (!atomic_load(&stopping))
    {
        ts_in_ms(&ts, TICK_MS);
        pthread_cond_timedwait(&stop_cond, &stop_lock, &ts);
        pthread_mutex_unlock(&stop_lock);

        r = atomic_load(&rate);
        for (i = 0; i < nports; i++)
        {
            queue_refill(&ports[i].q, r, TICK_MS);
        }
        if (++tick % (1000 / TICK_MS) == 0)
        {
            mac_table_age(&table);
        }
        pthread_mutex_lock(&stop_lock);
    }
    pthread_mutex_unlock(&stop_lock);
    return NULL;
}

static int setup_sockets(int tcp_port)
{
    struct sockaddr_in in;
    struct sockaddr_un un;
    int one = 1;

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    un_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (tcp_fd < 0 || un_fd < 0)
    {
        perror("socket");
        return -1;
    }
    setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&in, 0, sizeof in);
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_ANY);
    in.sin_port = htons((uint16_t)tcp_port);
    if (bind(tcp_fd, (struct sockaddr *)&in, sizeof in) != 0 || listen(tcp_fd, 128) != 0)
    {
        perror("tcp bind/listen");
        return -1;
    }
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", SOCK_PATH);
    unlink(SOCK_PATH);
    if (bind(un_fd, (struct sockaddr *)&un, sizeof un) != 0 || listen(un_fd, 8) != 0)
    {
        perror("unix bind/listen");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    pthread_t sig_th, acc_th, adm_th, timer_th;
    int tcp_port, i, n;

    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports> [config_file]\n", argv[0]);
        return 1;
    }
    tcp_port = atoi(argv[1]);
    nports = atoi(argv[2]);
    conf_path = argc == 4 ? argv[3] : NULL;
    if (tcp_port < 1 || tcp_port > 65535 || nports < 1 || nports > MAX_PORTS)
    {
        fprintf(stderr, "bad port or port count (1..%d)\n", MAX_PORTS);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGHUP);
    sigaddset(&sigs, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    if (mac_table_init(&table, 1024, 30) != 0 || sem_init(&free_ports, 0, (unsigned)nports) != 0
        || (stats = shm_stats_create(nports)) == NULL || log_start(LOG_PATH) != 0
        || setup_sockets(tcp_port) != 0)
    {
        fprintf(stderr, "startup failed\n");
        return 1;
    }
    atomic_store(&port_macs, 16);
    for (i = 0; i < nports; i++)
    {
        ports[i].idx = i;
        queue_init(&ports[i].q);
        atomic_store(&ports[i].vlan, 1);
        atomic_store(&ports[i].mirror_to, -1);
        publish(&ports[i]);
    }
    reload_config();

    if (pthread_create(&sig_th, NULL, signal_main, NULL) != 0
        || pthread_create(&adm_th, NULL, admin_main, NULL) != 0
        || pthread_create(&timer_th, NULL, timer_main, NULL) != 0
        || pthread_create(&acc_th, NULL, accept_main, NULL) != 0)
    {
        fprintf(stderr, "cannot start threads\n");
        return 1;
    }
    printf("switch running, pid %d, %d ports on tcp %d\n", (int)getpid(), nports, tcp_port);
    fflush(stdout);

    pthread_join(sig_th, NULL);
    pthread_join(acc_th, NULL);
    pthread_join(adm_th, NULL);
    pthread_join(timer_th, NULL);

    pthread_mutex_lock(&stop_lock);
    n = live;
    pthread_mutex_unlock(&stop_lock);
    for (i = 0; i < n; i++)
    {
        sem_post(&free_ports);
    }
    pthread_mutex_lock(&stop_lock);
    while (live > 0)
    {
        pthread_cond_wait(&live_cond, &stop_lock);
    }
    pthread_mutex_unlock(&stop_lock);

    log_msg("switch stopped");
    log_stop();
    for (i = 0; i < nports; i++)
    {
        queue_destroy(&ports[i].q);
    }
    close(tcp_fd);
    close(un_fd);
    unlink(SOCK_PATH);
    sem_destroy(&free_ports);
    mac_table_destroy(&table);
    shm_stats_destroy(stats);
    printf("clean shutdown\n");
    return 0;
}