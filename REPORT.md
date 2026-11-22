# Virtual Switch Report

## 1. Overview

The virtual switch accepts TCP connections from test hosts. Each accepted connection is assigned one switch port. The receiver thread reads complete frames from the connection, learns the source MAC address, selects the destination port or VLAN flood set, and places references to the frame into output queues. Each active port has its own sender thread, so a slow destination cannot block other destinations.

The frame header is 17 bytes: a network-order payload length, six-byte source MAC, six-byte destination MAC, network-order VLAN, and one-byte priority. `frame_recv` and `frame_send` use full-read and full-write loops, so TCP fragmentation and partial writes are handled correctly.

## 2. Thread diagram

```text
                         +------------------+
                         | signal thread    |
                         | sigwait          |
                         +--------+---------+
                                  |
        +-------------------------+--------------------------+
        |                         |                          |
+-------v-------+         +-------v-------+          +-------v-------+
| accept thread |         | admin thread  |          | timer thread  |
| TCP clients   |         | UNIX socket   |          | token refill  |
+-------+-------+         | swctl         |          | MAC aging     |
        |                 +---------------+          +---------------+
        |
        +--> one detached connection thread per host
                    |
                    +--> frame receive, learning, forwarding
                                   |
                    +--------------+--------------+
                    |                             |
             port output queue              MAC table
                    |
             one sender thread
             per active port

                         logger thread
                    asynchronous log file writer
```

The switch also creates a process-shared statistics object. `swctl stats` attaches to that shared memory and copies counters while holding its process-shared mutex.

## 3. Locks and protected data

| Lock | Protects |
|---|---|
| `ports_lock` | Port ownership, administrative state, file descriptors, and published port state. |
| `stop_lock` | Shutdown state coordination, live connection count, and shutdown condition variables. |
| Each output queue mutex | Queue contents, priority counts, scheduling credits, tokens, and closing state for one port. |
| Each MAC-table bucket mutex | The linked-list entries in that hash bucket. This is striped locking rather than one global table lock. |
| Logger mutex | The logger ring buffer and logger lifecycle state. |
| Shared-memory mutex | The statistics structure shared by `switch` and `swctl`. It is initialized with `PTHREAD_PROCESS_SHARED`. |

## 4. Lock order

The normal forwarding path takes one output queue lock at a time and does not hold `ports_lock` or a MAC bucket lock while doing socket I/O. MAC-table operations take only the relevant bucket lock. Port state operations use `ports_lock` and then publish through the shared-memory mutex. The code does not intentionally acquire a MAC bucket lock while holding a port or queue lock.

The practical order is:

```text
ports_lock -> shared-memory mutex
stop_lock  -> stop condition variables
queue lock -> queue condition variable
MAC bucket lock -> no other project lock
logger lock -> logger condition variable
```

Socket reads and writes happen after queue locks are released. This prevents a slow host from holding a queue mutex during network I/O.

## 5. Core behavior

A frame is allocated once and carries an atomic reference count. Flooding or mirroring increments the reference count for every queue insertion. A failed queue insertion immediately releases that reference. The final sender or drain operation frees the frame.

Each port has three bounded queues. `queue_push` never waits: it returns a full result and the switch increments the dropped counter. The sender selects high, normal, and low priority frames using weighted scheduling, with bounded aging promotions so low priority traffic is not starved. The token bucket is refilled by the timer thread and the sender waits on a timed condition variable when no token is available.

The MAC table uses 64 striped buckets. Entries age out, move when a source appears on another port, and are removed when the configured maximum is exceeded. Port MAC limits provide flooding protection. A counting semaphore limits active connections; an extra connection waits with `sem_timedwait` and receives a `switch full` response after the timeout.

## 6. Tier 2 features

Implemented Tier 2 items are:

- Item 6: priority queues, weighted scheduling, and aging protection.
- Item 7: per-port token-bucket rate limiting.
- Item 8: VLAN filtering and one configurable trunk port.
- Item 9: ingress and egress port mirroring.
- Item 10: live port shutdown/up, VLAN changes, and MAC clearing.
- Item 11: `swctl`, UNIX-domain administration, shared statistics, and watch mode.
- Item 12: dedicated `sigwait` handling for `SIGINT`, `SIGTERM`, `SIGHUP`, and `SIGUSR1`.
- Item 13: asynchronous logging through a bounded logger queue.

MAC aging is performed by the timer thread once per second rather than by a separate aging thread.

## 7. Tier 3 status

The Tier 3 bonus features are not implemented. There is no two-switch pipe trunk, supervisor restart process, or benchmark comparing locking strategies.

## 8. Bug found and fixed

The first clean build failed at link time because the `switch` target omitted `queue.o` and `logger.o`, even though `switch.c` called functions from both modules. The compiler produced object files successfully, but the linker reported undefined references such as `queue_push`, `queue_pop`, and `log_msg`. Adding both objects to `SWITCH_OBJS` fixed the fresh build.

A later functional test exposed two behavioral issues: queue drops were not observable quickly with a large TCP send buffer, and aging could override the initial priority schedule. The accepted socket send buffer was bounded, and aging promotions were limited so weighted scheduling remains dominant. The resulting functional suite passes all 36 checks.
