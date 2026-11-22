CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread \
          -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread
LDLIBS  = -lrt

BINS = switch swctl host

SWITCH_OBJS = switch.o frame.o mac_table.o queue.o logger.o shm_stats.o
SWCTL_OBJS  = swctl.o shm_stats.o
HOST_OBJS   = host.o frame.o

all: $(BINS)

switch: $(SWITCH_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

swctl: $(SWCTL_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

host: $(HOST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

frame.o:     frame.c frame.h
mac_table.o: mac_table.c mac_table.h
shm_stats.o: shm_stats.c shm_stats.h
switch.o:    switch.c mac_table.h shm_stats.h
swctl.o:     swctl.c shm_stats.h
host.o:      host.c frame.h

tsan: CFLAGS  += -fsanitize=thread
tsan: LDFLAGS += -fsanitize=thread
tsan: clean all

clean:
	rm -f $(BINS) *.o lab mac_dump.txt switch.log

.PHONY: all clean tsan