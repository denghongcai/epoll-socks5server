CC=gcc
DEBUGCFLAGS=-DDEBUG -pg -g -Wall
RELEASECFLAGS=-Wall -O2 -s -DNDEBUG

all: release

debug: main.c
	$(CC) $(DEBUGCFLAGS) main.c -o epoll-socks5server

release: main.c
	$(CC) $(RELEASECFLAGS) main.c -o epoll-socks5server

clean:
	rm -rf epoll-socks5server
