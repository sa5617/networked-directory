# Networked Directory
#
# Targets:
#   all     builds all five binaries; seeds both databases on first run
#   seed    wipes and reseeds both databases with default users and entries
#   clean   removes the binaries
#
# Binaries produced at the project root:
#   http-server        HTTP/1.0 server; serves static files, proxies /mdb-lookup
#                      and /api/ to their respective backends
#   mdb-lookup-server  TCP server that answers substring queries against data/mydb
#   http-client        minimal HTTP client for testing file downloads
#   mdb-add            command-line tool to append records to data/mydb
#   mdb-lookup         command-line substring search against data/mydb

CC     = gcc
CFLAGS = -Wall -Wextra -g -Isrc

all: http-server mdb-lookup-server http-client mdb-add mdb-lookup
	@if [ ! -f api/directory.db ] || [ ! -f data/mydb ]; then cd api && python3 seed.py; fi

# depends on mdb.c and mylist.c for the binary record format and linked list
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

seed:
	cd api && python3 seed.py

clean:
	rm -f http-server mdb-lookup-server http-client mdb-add mdb-lookup

.PHONY: all seed clean
