CC     = gcc
CFLAGS = -Wall -Wextra -g -Isrc
DB     = data/mydb

all: http-server mdb-lookup-server http-client mdb-add mdb-lookup

http-server: src/http-server.c
	$(CC) $(CFLAGS) -o $@ $^

mdb-lookup-server: src/mdb-lookup-server.c src/mdb.c src/mylist.c
	$(CC) $(CFLAGS) -o $@ $^

http-client: src/http-client.c
	$(CC) $(CFLAGS) -o $@ $^

mdb-add: src/mdb-add.c src/mdb.c src/mylist.c
	$(CC) $(CFLAGS) -o $@ $^

mdb-lookup: src/mdb-lookup.c src/mdb.c src/mylist.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f http-server mdb-lookup-server http-client mdb-add mdb-lookup

.PHONY: all clean
