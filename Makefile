CC     = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
OBJS   = main.c frame.c mac_table.c

all: lab
lab: $(OBJS) frame.h mac_table.h
	$(CC) $(CFLAGS) $(OBJS) -o lab

clean:
	rm -f lab

.PHONY: all clean