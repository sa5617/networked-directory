/*
 * mdb-add.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mdb.h"

static void die(const char *message) {
    perror(message);
    exit(1);
}

/* records are stored as raw bytes; non-printable characters corrupt printf and HTML output */
static void sanitize(char *s) {
    while (*s) {
        if (!isprint(*s))
            *s = ' ';
        s++;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "%s\n", "usage: mdb-add <database_file>");
        exit(1);
    }

    char *filename = argv[1];
    // open for append & read, in binary mode
    FILE *fp = fopen(filename, "a+b");
    if (fp == NULL)
        die(filename);

    fseek(fp, 0, SEEK_END);
    int recNo = (int)(ftell(fp) / sizeof(struct MdbRec));
    recNo++;

    struct MdbRec r;
    char line[1000];

    printf("name please (will truncate to %d chars): ", (int)sizeof(r.name)-1);
    if (fgets(line, sizeof(line), stdin) == NULL) {
        fprintf(stderr, "%s\n", "could not read name");
        exit(1);
    }
    // must null-terminate the string manually after strncpy().
    strncpy(r.name, line, sizeof(r.name) - 1);
    r.name[sizeof(r.name) - 1] = '\0';
    size_t last = strlen(r.name);
    if (last > 0 && r.name[last - 1] == '\n')
        r.name[last - 1] = '\0';

    printf("msg please (will truncate to %d chars): ", (int)sizeof(r.msg)-1);
    if (fgets(line, sizeof(line), stdin) == NULL) {
        fprintf(stderr, "%s\n", "could not read msg");
        exit(1);
    }
    // must null-terminate the string manually after strncpy().
    strncpy(r.msg, line, sizeof(r.msg) - 1);
    r.msg[sizeof(r.msg) - 1] = '\0';
    last = strlen(r.msg);
    if (last > 0 && r.msg[last - 1] == '\n')
        r.msg[last - 1] = '\0';

    sanitize(r.name);
    sanitize(r.msg);

    if (fwrite(&r, sizeof(r), 1, fp) < 1) {
        perror("can't write record");
        exit(1);
    }
    if (fflush(fp) != 0) {
        perror("can't write to file");
        exit(1);
    }

    printf("%4d: {%s} said {%s}\n", recNo, r.name, r.msg);
    fflush(stdout);

    fclose(fp);
    return 0;
}
