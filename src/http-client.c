/*
 * http-client.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ctype.h>

#define BUF_SIZE 4096

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static void printUsage() {
    fprintf(stderr, "usage: http-client <host name> <port number> <file path>\n");
    fprintf(stderr, "   ex) http-client www.example.com 80 /index.html\n");
    exit(1);
}

int main(int argc, char **argv) {

    char *serverName;
    char *serverIP;
    char *serverPort;
    char *filePath;
    char *fname;

    int sock;
    struct sockaddr_in serverAddr;
    struct hostent *he;
    char buf[BUF_SIZE];

    if (argc != 4) {
	printUsage();
    }

    serverName = argv[1];
    serverPort = argv[2];
    filePath = argv[3];
    char *p = strrchr(filePath, '/');
    if (!p)
	printUsage();
    fname = p + 1;

    if ((he = gethostbyname(serverName)) == NULL) {
	die("gethostbyname failed");
    }
    serverIP = inet_ntoa(*(struct in_addr *)he->h_addr);

    if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
	die("socket failed");
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(serverIP);
    unsigned short port = atoi(serverPort);
    serverAddr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
	die("connect failed");
    }

    snprintf(buf, sizeof(buf),
	    "GET %s HTTP/1.0\r\n"
	    "Host: %s:%s\r\n"
	    "\r\n",
	    filePath, serverName, serverPort);
    if (send(sock, buf, strlen(buf), 0) != (ssize_t)strlen(buf)) {
	die("send failed");
    }

    // wrap the socket with FILE* so we can read the response line by line
    FILE *fd;
    if ((fd = fdopen(sock, "r")) == NULL) {
	die("fdopen failed");
    }

    if (fgets(buf, sizeof(buf), fd) == NULL) {
	if (ferror(fd))
	    die("IO error");
	else {
	    fprintf(stderr, "server terminated connection without response");
            fclose(fd);
	    exit(1);
	}
    }
    if (strncmp("HTTP/1.0 ", buf, 9) != 0 && strncmp("HTTP/1.1 ", buf, 9) != 0) {
	fprintf(stderr, "unknown protocol response: %s\n", buf);
        fclose(fd);
	exit(1);
    }
    if (strncmp("200", buf + 9, 3) != 0 || !isspace(buf[12])) {
	fprintf(stderr, "%s\n", buf);
        fclose(fd);
	exit(1);
    }

    for (;;) {
	if (fgets(buf, sizeof(buf), fd) == NULL) {
	    if (ferror(fd))
		die("IO error");
	    else {
		fprintf(stderr, "server terminated connection without sending file");
                fclose(fd);
		exit(1);
	    }
	}
	if (strcmp("\r\n", buf) == 0) {
	    break;
	}
    }

    // use fread()/fwrite() so we can download binary files as well as HTML
    FILE *outputFile = fopen(fname, "wb");
    if (outputFile == NULL) {
        fclose(fd);
	die("can't open output file");
    }

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fd)) > 0) {
	if (fwrite(buf, 1, n, outputFile) != n) {
            fclose(fd);
	    die("fwrite failed");
        }
    }
    // fread() returns 0 on both EOF and error
    if (ferror(fd))
	die("fread failed");

    fclose(outputFile);

    // closing fd also closes the underlying socket
    fclose(fd);

    return 0;
}
