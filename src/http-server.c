/*
 * http-server.c
 */

#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAXPENDING 5
#define DISK_IO_BUF_SIZE 4096
#define MAX_HOSTNAME_LEN 256

/* stores hostname and port for building 301 Location headers */
static struct {
    char hostName[MAX_HOSTNAME_LEN];
    unsigned short port;
} localServerInfo;

static void die(const char *message)
{
    perror(message);
    exit(1);
}

/*
 * Reaps all terminated child processes.
 */
static void sigchld_handler(int sig)
{
    (void)sig; /* suppress unused parameter warning */
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");

    // allow immediate reuse of the port after server restarts
    int opt = 1;
    if (setsockopt(servSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
        die("setsockopt() failed");

    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family      = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port        = htons(port);

    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

static int createMdbLookupConnection(const char *mdbHost, unsigned short mdbPort)
{
    struct hostent *he;
    if ((he = gethostbyname(mdbHost)) == NULL)
        die("gethostbyname failed");
    char *serverIP = inet_ntoa(*(struct in_addr *)he->h_addr);

    int sock;
    if ((sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket failed");

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr(serverIP);
    serverAddr.sin_port        = htons(mdbPort);

    if (connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0)
        die("connect failed");

    return sock;
}

/*
 * send() wrapper for null-terminated strings.
 * Returns -1 on failure.
 */
static ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != (ssize_t)len) {
        perror("send() failed");
        return -1;
    }
    return res;
}

/*
 * HTTP/1.0 status codes and reason phrases.
 */
static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 301, "Moved Permanently" },
    { 400, "Bad Request" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 0, NULL }
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}

static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n", statusCode, getReasonPhrase(statusCode));
    Send(clntSock, buf);
}

static void sendErrorStatus(int clntSock, int statusCode)
{
    sendStatusLine(clntSock, statusCode);
    Send(clntSock, "\r\n");

    char buf[1000];
    snprintf(buf, sizeof(buf),
            "<html><body>\n"
            "<h1>%d %s</h1>\n"
            "</body></html>\n",
            statusCode, getReasonPhrase(statusCode));
    Send(clntSock, buf);
}

/*
 * Writes an HTML-escaped version of `in` into `out`.
 * Escapes &, <, >, and " so user-controlled strings are safe in HTML context.
 */
static void htmlEscape(char *out, size_t outSize, const char *in)
{
    if (outSize == 0)
        return;
    size_t i = 0;
    while (*in) {
        const char *ent;
        size_t elen;
        switch (*in) {
            case '&': ent = "&amp;";  elen = 5; break;
            case '<': ent = "&lt;";   elen = 4; break;
            case '>': ent = "&gt;";   elen = 4; break;
            case '"': ent = "&quot;"; elen = 6; break;
            default:  ent = NULL;     elen = 1; break;
        }
        if (i + elen >= outSize)
            break;
        if (ent)
            memcpy(out + i, ent, elen);
        else
            out[i] = *in;
        i += elen;
        in++;
    }
    out[i] = '\0';
}

/*
 * Redirect the browser to requestURI with a trailing slash appended.
 */
static void send301Status(int clntSock, const char *requestURI)
{
    sendStatusLine(clntSock, 301);

    char *buf = malloc(
        2 * (strlen(localServerInfo.hostName) + strlen(requestURI)) + 1000);
    if (buf == NULL)
        die("malloc failed");

    // send Location header and an HTML fallback in case the browser
    // doesn't follow the redirect automatically
    sprintf(buf,
            "Location: http://%s:%d%s/\r\n"
            "\r\n"
            "<html><body>\n"
            "<h1>301 Moved Permanently</h1>\n"
            "<p>The document has moved "
            "<a href=\"http://%s:%d%s/\">here</a>.</p>\n"
            "</body></html>\n",
            localServerInfo.hostName, localServerInfo.port, requestURI,
            localServerInfo.hostName, localServerInfo.port, requestURI);

    Send(clntSock, buf);
    free(buf);
}

#define MAX_RESULTS 512

/*
 * Handle /mdb-lookup or /mdb-lookup?key=... requests.
 * Returns the HTTP status code sent to the browser.
 */
static int handleMdbRequest(
        const char *requestURI, FILE *mdbFp, int mdbSock, int clntSock)
{
    int statusCode = 200;
    const char *keyURI = "/mdb-lookup?key=";
    const char *key = NULL;

    if (strncmp(requestURI, keyURI, strlen(keyURI)) == 0)
        key = requestURI + strlen(keyURI);

    sendStatusLine(clntSock, statusCode);
    Send(clntSock, "\r\n");

#define S(x) if (Send(clntSock, (x)) < 0) return statusCode

    S("<!doctype html>\n<html lang=\"en\">\n<head>\n"
      "<meta charset=\"utf-8\">\n"
      "<title>The Directory \xe2\x80\x94 Network Directory</title>\n"
      "<style>\n"
      "@import url('https://fonts.googleapis.com/css2?family=Special+Elite&display=swap');\n"
      "html,body{margin:0;padding:0;min-height:100%;background:#d8d3c4;}\n"
      "body{font-family:'Special Elite','Courier New',Courier,monospace;color:#1f1b14;"
      "font-size:16px;line-height:1.7;padding:56px 24px 96px;"
      "box-sizing:border-box;min-height:100vh;}\n");

    S(".page{width:100%;max-width:820px;margin:0 auto;background:#f5efdd;"
      "background-image:radial-gradient(rgba(0,0,0,.035) 1px,transparent 1px),"
      "radial-gradient(rgba(0,0,0,.02) 1px,transparent 1px),"
      "linear-gradient(180deg,#f7f1df 0%,#ede4ca 100%);"
      "background-size:3px 3px,7px 7px,100% 100%;"
      "background-position:0 0,1px 2px,0 0;"
      "padding:72px 84px 88px;"
      "box-shadow:0 1px 0 rgba(0,0,0,.06),0 14px 30px -10px rgba(0,0,0,.35),"
      "0 30px 60px -20px rgba(0,0,0,.28);position:relative;box-sizing:border-box;}\n");

    S(".slug{display:flex;justify-content:space-between;align-items:baseline;"
      "font-size:13px;letter-spacing:1px;color:#46402f;"
      "border-bottom:1px dashed #8a7e5c;padding-bottom:8px;margin-bottom:28px;}\n"
      ".slug .left{text-transform:uppercase;}\n"
      ".slug a{color:#46402f;text-decoration:none;border-bottom:1px solid #8a7e5c;"
      "padding-bottom:1px;}\n"
      ".slug a:hover{color:#1f1b14;border-color:#1f1b14;}\n"
      ".title{text-align:center;font-size:26px;letter-spacing:3px;"
      "text-transform:uppercase;margin:12px 0 6px;}\n"
      ".sub{text-align:center;font-size:13px;letter-spacing:4px;"
      "text-transform:uppercase;color:#5a4f36;margin-bottom:36px;}\n");

    S("form{margin:0;}\n"
      ".search{display:flex;align-items:baseline;gap:14px;"
      "border-bottom:1.5px solid #1f1b14;padding:4px 2px 10px;margin-bottom:6px;}\n"
      ".search .prompt{font-size:13px;letter-spacing:4px;text-transform:uppercase;"
      "color:#5a4f36;flex:0 0 auto;}\n"
      ".search input{flex:1;background:transparent;border:0;outline:0;font:inherit;"
      "color:#1f1b14;font-size:18px;letter-spacing:1px;padding:0;"
      "text-shadow:.3px .2px 0 rgba(0,0,0,.22);}\n"
      ".search input::placeholder{color:#8a7e5c;}\n"
      ".search .caret{display:inline-block;width:.55ch;height:1em;background:#1f1b14;"
      "vertical-align:-2px;animation:blink 1.05s steps(2,start) infinite;}\n"
      "@keyframes blink{to{opacity:0;}}\n");

    S(".stat{margin:22px 0 10px;font-size:12px;letter-spacing:3px;"
      "text-transform:uppercase;color:#5a4f36;"
      "border-bottom:1px dashed #8a7e5c;padding-bottom:8px;"
      "display:flex;justify-content:space-between;align-items:baseline;}\n"
      ".entries{list-style:none;padding:0;margin:0;}\n"
      ".entries li{padding:14px 0;border-bottom:1px dotted #8a7e5c;"
      "text-shadow:.3px .2px 0 rgba(0,0,0,.22);"
      "display:grid;grid-template-columns:180px 1fr;gap:0 22px;align-items:baseline;}\n"
      ".entries li:last-child{border-bottom:0;}\n"
      ".entries .ename{font-size:16px;letter-spacing:1px;color:#5a4f36;}\n"
      ".entries .emsg{font-size:16px;}\n"
      ".entries li.empty{display:block;padding:36px 0;text-align:center;"
      "color:#5a4f36;letter-spacing:2px;text-transform:uppercase;font-size:13px;}\n");

    S(".foot{margin-top:44px;padding-top:18px;border-top:1px dashed #8a7e5c;"
      "display:flex;justify-content:space-between;align-items:baseline;"
      "font-size:13px;color:#4a4128;letter-spacing:1px;}\n"
      ".foot a{color:#1f1b14;text-decoration:none;border-bottom:1.5px solid #1f1b14;"
      "padding-bottom:1px;}\n"
      ".foot a::before{content:\"\\2196 \";}\n"
      "</style>\n</head>\n<body>\n");

    S("<div class=\"page\">\n"
      "  <div class=\"slug\">\n"
      "    <span class=\"left\">net.dir / lookup</span>\n"
      "    <span class=\"right\"><a href=\"/\">return to index</a>"
      " &nbsp;&middot;&nbsp; page&nbsp;&mdash;&nbsp;2&nbsp;&mdash;</span>\n"
      "  </div>\n"
      "  <div class=\"title\">The Directory</div>\n"
      "  <div class=\"sub\">search by name or message</div>\n");

    S("  <form method=\"GET\" action=\"/mdb-lookup\">\n"
      "  <div class=\"search\">\n"
      "    <span class=\"prompt\">find&nbsp;:</span>\n");

    if (key) {
        /* key comes directly from the URL — escape it before inserting into an attribute */
        char keyEsc[6000 + 1];
        htmlEscape(keyEsc, sizeof(keyEsc), key);
        char inputBuf[sizeof(keyEsc) + 150];
        snprintf(inputBuf, sizeof(inputBuf),
            "    <input type=\"text\" name=\"key\" value=\"%s\""
            " autocomplete=\"off\" autofocus />\n", keyEsc);
        if (Send(clntSock, inputBuf) < 0) return statusCode;
    } else {
        S("    <input type=\"text\" name=\"key\""
          " placeholder=\"a name or a note\xe2\x80\xa6\""
          " autocomplete=\"off\" autofocus />\n");
    }

    S("    <span class=\"caret\"></span>\n"
      "  </div>\n"
      "  </form>\n");

    if (key && *key != '\0') {
        if (Send(mdbSock, key) < 0 || Send(mdbSock, "\n") < 0) {
            perror("mdb-lookup-server send failed");
            return statusCode;
        }

        /* collect all rows before sending so we can show a result count */
        char rnames[MAX_RESULTS][20];
        char rmsgs[MAX_RESULTS][30];
        int count = 0;

        char line[1000];
        for (;;) {
            if (fgets(line, sizeof(line), mdbFp) == NULL) {
                if (ferror(mdbFp))
                    perror("mdb-lookup-server read failed");
                else
                    fprintf(stderr, "mdb-lookup-server: connection closed\n");
                break;
            }
            if (strcmp("\n", line) == 0)
                break;
            if (count < MAX_RESULTS) {
                int recNo;
                if (sscanf(line, "%d: {%19[^}]} said {%29[^}]}",
                           &recNo, rnames[count], rmsgs[count]) == 3)
                    count++;
            }
        }

        char statBuf[200];
        snprintf(statBuf, sizeof(statBuf),
            "  <div class=\"stat\"><span>%d %s</span></div>\n",
            count, count == 1 ? "result" : "results");
        if (Send(clntSock, statBuf) < 0) return statusCode;

        S("  <ul class=\"entries\">\n");

        if (count == 0) {
            S("    <li class=\"empty\">nothing found.</li>\n");
        } else {
            int i;
            char rowBuf[400];
            char enameEsc[20 * 6 + 1];
            char emsgEsc[30 * 6 + 1];
            for (i = 0; i < count; i++) {
                htmlEscape(enameEsc, sizeof(enameEsc), rnames[i]);
                htmlEscape(emsgEsc, sizeof(emsgEsc), rmsgs[i]);
                snprintf(rowBuf, sizeof(rowBuf),
                    "    <li><span class=\"ename\">%s</span>"
                    "<span class=\"emsg\">%s</span></li>\n",
                    enameEsc, emsgEsc);
                if (Send(clntSock, rowBuf) < 0) return statusCode;
            }
        }

        S("  </ul>\n");
    }

    S("  <div class=\"foot\">\n"
      "    <span><a href=\"/\">back to the index</a></span>\n"
      "  </div>\n"
      "</div>\n"
      "</body>\n</html>\n");

#undef S

    return statusCode;
}

/*
 * Handle static file requests.
 * Returns the HTTP status code sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    char *file = malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file) - 1] == '/')
        strcat(file, "index.html");

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 301;
        send301Status(clntSock, requestURI);
        goto func_end;
    }

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404;
        sendErrorStatus(clntSock, statusCode);
        goto func_end;
    }

    statusCode = 200;
    sendStatusLine(clntSock, statusCode);
    Send(clntSock, "\r\n");

    char buf[DISK_IO_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != (ssize_t)n) {
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 on both EOF and error
    if (ferror(fp))
        perror("fread failed");

func_end:
    free(file);
    if (fp)
        fclose(fp);
    return statusCode;
}

/*
 * Handle one HTTP connection: open a fresh mdb connection, parse the request,
 * dispatch to the appropriate handler, log, and clean up.
 * Called in the child process after fork().
 */
static void handleClient(int clntSock, struct sockaddr_in clntAddr,
                          const char *webRoot,
                          const char *mdbHost, unsigned short mdbPort)
{
    int mdbSock = createMdbLookupConnection(mdbHost, mdbPort);
    FILE *mdbFp = fdopen(mdbSock, "r");
    if (mdbFp == NULL) {
        close(mdbSock);
        die("fdopen failed");
    }

    FILE *clntFp = fdopen(clntSock, "r");
    if (clntFp == NULL) {
        fclose(mdbFp);
        die("fdopen failed");
    }

    char requestLine[1000];
    char line[1000];
    int statusCode    = 0;
    char *method      = "";
    char *requestURI  = "";
    char *httpVersion = "";

    if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
        statusCode = 400;
        goto done;
    }

    {
        char *sep = "\t \r\n";
        method      = strtok(requestLine, sep);
        requestURI  = strtok(NULL, sep);
        httpVersion = strtok(NULL, sep);
        char *extra = strtok(NULL, sep);

        if (!method || !requestURI || !httpVersion || extra) {
            statusCode = 400;
            sendErrorStatus(clntSock, statusCode);
            goto done;
        }
    }

    if (strcmp(method, "GET") != 0) {
        statusCode = 501;
        sendErrorStatus(clntSock, statusCode);
        goto done;
    }

    if (strcmp(httpVersion, "HTTP/1.0") != 0 &&
            strcmp(httpVersion, "HTTP/1.1") != 0) {
        statusCode = 501;
        sendErrorStatus(clntSock, statusCode);
        goto done;
    }

    if (*requestURI != '/') {
        statusCode = 400;
        sendErrorStatus(clntSock, statusCode);
        goto done;
    }

    // block path traversal — "/../" or trailing "/.." would escape webRoot
    {
        int ulen = strlen(requestURI);
        if (ulen >= 3) {
            char *tail = requestURI + (ulen - 3);
            if (strcmp(tail, "/..") == 0 || strstr(requestURI, "/../") != NULL) {
                statusCode = 400;
                sendErrorStatus(clntSock, statusCode);
                goto done;
            }
        }
    }

    while (1) {
        if (fgets(line, sizeof(line), clntFp) == NULL) {
            statusCode = 400;
            goto done;
        }
        if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0)
            break;
    }

    if (strcmp(requestURI, "/mdb-lookup") == 0 ||
            strncmp(requestURI, "/mdb-lookup?", 12) == 0) {
        statusCode = handleMdbRequest(requestURI, mdbFp, mdbSock, clntSock);
    } else {
        statusCode = handleFileRequest(webRoot, requestURI, clntSock);
    }

done:
    fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
            inet_ntoa(clntAddr.sin_addr),
            method, requestURI, httpVersion,
            statusCode, getReasonPhrase(statusCode));
    fclose(mdbFp);  // also closes mdbSock
    fclose(clntFp); // also closes clntSock
}

int main(int argc, char *argv[])
{
    // prevent SIGPIPE from killing the process when writing to a disconnected socket
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");

    // SA_RESTART causes accept() to restart after SIGCHLD instead of returning EINTR
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        die("sigaction() failed");

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <server_port> <web_root> <mdb-lookup-host> <mdb-lookup-port>\n",
                argv[0]);
        exit(1);
    }

    unsigned short servPort = atoi(argv[1]);
    const char *webRoot     = argv[2];
    const char *mdbHost     = argv[3];
    unsigned short mdbPort  = atoi(argv[4]);

    int servSock = createServerSocket(servPort);

    // resolve our own hostname for use in 301 Location headers
    char hostname[MAX_HOSTNAME_LEN];
    hostname[MAX_HOSTNAME_LEN - 1] = '\0';
    gethostname(hostname, MAX_HOSTNAME_LEN - 1);
    struct hostent *he = gethostbyname(hostname);
    if (he == NULL)
        die("gethostbyname failed");
    strncpy(localServerInfo.hostName, he->h_name, MAX_HOSTNAME_LEN - 1);
    localServerInfo.hostName[MAX_HOSTNAME_LEN - 1] = '\0';
    localServerInfo.port = servPort;

    struct sockaddr_in clntAddr;

    for (;;) {
        socklen_t clntLen = sizeof(clntAddr);
        int clntSock = accept(servSock, (struct sockaddr *)&clntAddr, &clntLen);
        if (clntSock < 0) {
            perror("accept() failed");
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork() failed");
            close(clntSock);
            continue;
        }
        if (pid == 0) {
            close(servSock);
            handleClient(clntSock, clntAddr, webRoot, mdbHost, mdbPort);
            exit(0);
        }
        // parent closes its copy so the fd is fully released when the child closes theirs
        close(clntSock);
    }
}
