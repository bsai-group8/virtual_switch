# Virtual Switch

Operating Systems, Fall 2026. This project implements a multithreaded TCP software switch in C.

## Build

Requirements: Linux, GCC, POSIX threads, and POSIX shared memory.

```sh
make
make clean
```

The required warning flags are enabled by the Makefile:
`-std=c11 -Wall -Wextra -Werror -g -O0 -pthread`.

The programs are:

- `switch`: the virtual switch server
- `host`: a simple test host
- `swctl`: the administrative client

## Run

Start the switch with a TCP port and number of switch ports:

```sh
./switch 9200 4
```

A host uses a 1-based connection slot assigned by the switch. Start a test host with:

```sh
./host 127.0.0.1 9200 02:00:00:00:00:01
```

Optional host arguments set the VLAN and priority:

```sh
./host 127.0.0.1 9200 02:00:00:00:00:02 1 0
```

Host input is one command per line:

```text
<destination-mac|bcast> <text>
freeze
```

`freeze` stops the host from reading, which is useful for testing bounded output queues.

## Administration

`swctl` connects to the switch through `/tmp/vswitch.sock`. Port numbers are 1-based in every command.

```sh
./swctl show mac
./swctl show ports
./swctl shutdown 2
./swctl up 2
./swctl vlan 2 10
./swctl trunk 3
./swctl trunk 3 off
./swctl mirror 1 4
./swctl mirror 1 off
./swctl clear mac
./swctl rate 20
./swctl age 30
./swctl maxmacs 1024
./swctl portmacs 16
./swctl stats
./swctl stats --watch
```

Counters are stored in `/vswitch_stats` shared memory and protected by a process-shared mutex.

## Configuration

An optional configuration file may be supplied as the third switch argument:

```sh
./switch 9200 4 switch.conf
```

The file is a list of the same administrative commands accepted by `swctl`, one per line. Blank lines and text after `#` are ignored. Example:

```text
rate 20
age 30
portmacs 16
vlan 1 10
vlan 2 10
trunk 3
mirror 1 4
```

The file is loaded at startup and reloaded after `SIGHUP`:

```sh
kill -HUP <switch-pid>
```

## Signals and logging

- `SIGINT` or `SIGTERM`: clean shutdown
- `SIGHUP`: reload the configuration file
- `SIGUSR1`: write the MAC table to `mac_dump.txt`

Log messages are written asynchronously to `switch.log`.

## Testing

Run the functional tests after building:

```sh
python3 test.py
```

The current public test suite checks forwarding, partial frames, disconnects, queue drops, port limits, MAC security, rate limiting, priority scheduling, VLANs, trunks, mirroring, admin commands, aging, signals, and shutdown.

## Completion

Tier 1 items 1 through 5 are implemented. Tier 2 items 6 through 13 are implemented, with MAC aging performed by the timer thread. Tier 3 bonus work is not implemented.
