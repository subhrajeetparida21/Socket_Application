CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread

COMMON  = common.c common.h

.PHONY: all clean

all: server node client

server: server.c $(COMMON)
	$(CC) $(CFLAGS) server.c common.c -o server $(LDFLAGS)

node: node.c $(COMMON)
	$(CC) $(CFLAGS) node.c common.c -o node $(LDFLAGS)

client: client.c $(COMMON)
	$(CC) $(CFLAGS) client.c common.c -o client

clean:
	rm -f server node client
