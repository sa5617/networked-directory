/*
 * mdb-lookup-server.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "mylist.h"
#include "mdb.h"

#define KeyMax 32

static void die(const char *s) { perror(s); exit(1); }

/*
 * Load all records from filename into list.
 */
static void loadDatabase(const char *filename, struct List *list)
{
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL)
        die(filename);

    if (loadmdb(fp, list) < 0)
        die("loadmdb failed");

    fclose(fp);
}

/*
 * Handle one client connection: reload the database, answer queries
 * until the client disconnects, then clean up.
 *
 * The database reloads fresh on each connection so clients always see
 * the latest records without requiring a server restart.
 */
static void handleClient(int clntsock, const char *filename)
{
    struct List list;
    initList(&list);
    loadDatabase(filename, &list);

    // wrap the socket with FILE* so we can use fgets() for line reads
    FILE *input = fdopen(clntsock, "r");
    if (input == NULL)
        die("fdopen failed");

    char line[1000];
    char key[KeyMax + 1];
    char buf[4096];

    while (fgets(line, sizeof(line), input) != NULL) {
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';

        size_t last = strlen(key);
        if (last > 0 && key[last - 1] == '\n')
            key[last - 1] = '\0';

        struct Node *node = list.head;
        int recNo = 1;
        while (node) {
            struct MdbRec *rec = (struct MdbRec *)node->data;
            if (strstr(rec->name, key) || strstr(rec->msg, key)) {
                int size = sprintf(buf, "%4d: {%s} said {%s}\n",
                        recNo, rec->name, rec->msg);
                if (send(clntsock, buf, size, 0) != size) {
                    perror("send failed");
                    goto done;
                }
            }
            node = node->next;
            recNo++;
        }

        // blank line signals end of results to the HTTP server
        int size = sprintf(buf, "\n");
        if (send(clntsock, buf, size, 0) != size)
            perror("send failed");
    }

    if (ferror(input))
        perror("fgets failed");

done:
    freemdb(&list);
    fclose(input); // also closes clntsock
}

int main(int argc, char **argv)
{
    // prevent SIGPIPE from terminating the process when writing to a disconnected socket
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    if (argc != 3) {
        fprintf(stderr, "usage: %s <db_file> <server-port>\n", argv[0]);
        exit(1);
    }

    const char *filename = argv[1];
    unsigned short port = atoi(argv[2]);

    int servsock;
    if ((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    int opt = 1;
    if (setsockopt(servsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        die("setsockopt failed");

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(port);

    if (bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("bind failed");

    if (listen(servsock, 5) < 0)
        die("listen failed");

    int clntsock;
    socklen_t clntlen;
    struct sockaddr_in clntaddr;

    while (1) {
        clntlen = sizeof(clntaddr);
        if ((clntsock = accept(servsock,
                        (struct sockaddr *) &clntaddr, &clntlen)) < 0)
            die("accept failed");

        fprintf(stderr, "\nconnection from: %s\n", inet_ntoa(clntaddr.sin_addr));
        handleClient(clntsock, filename);
        fprintf(stderr, "connection closed: %s\n", inet_ntoa(clntaddr.sin_addr));
    }
}
