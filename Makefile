CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
LDFLAGS ?= -pthread

map-node: main.o config.o node.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

main.o: main.c map.h
config.o: config.c map.h
node.o: node.c map.h

clean:
	rm -f map-node *.o