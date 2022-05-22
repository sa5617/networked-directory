# C HTTP Server

A concurrent HTTP/1.0 file server backed by a TCP message-database lookup server, built from scratch in C. I developed this out of a sequence of programming labs I completed for a course at Columbia University. In total, the labs covered linked lists, binary file I/O, TCP socket servers, and HTTP. This self-conducted project consolidates those pieces into a single cohesive multi-tier system and extends them with concurrent connection handling through a fork-per-connection model.

---

## Architecture

- **`http-server`** — accepts HTTP/1.0 GET requests, serves static files from a configurable web root, and proxies `/mdb-lookup` queries to the database server over a per-connection TCP socket.
- **`mdb-lookup-server`** — a TCP server that loads a binary record file into memory and answers substring search queries, one per line, returning a blank line as the end-of-results sentinel.
- **`mdb-add`** — a command-line tool for appending name/message records to the database file.

```
  Browser / curl
       |
       |  HTTP/1.0 GET
       v
  http-server  ──────────────────────  www/  (static files)
       |
       |  TCP (newline-delimited queries)
       v
  mdb-lookup-server
       |
       |  fopen()
       v
  database file  ◄──  mdb-add
```

The HTTP server opens a fresh TCP connection to `mdb-lookup-server` inside each child process, so there is no shared socket state across connections.

---

## Features

- HTTP/1.0 GET with correct status codes (200, 301, 400, 404, 500, 501)
- Static file serving with binary-safe `fread`/`send` transfer
- 301 redirect for directory URLs missing a trailing slash
- Path traversal protection (`/../` and trailing `/..` blocked)
- `/mdb-lookup` endpoint with an HTML form and alternating-row results table
- Fork-per-connection concurrency — each client runs in an isolated child process
- Zombie reaping via `SIGCHLD` handler with `waitpid(WNOHANG)` loop
- `SIGPIPE` ignored so writes to disconnected clients return an error instead of killing the process
- `SO_REUSEADDR` for immediate port reuse after restart

---

## Build

```sh
make all
```

Produces five binaries at the project root: `http-server`, `mdb-lookup-server`, `http-client`, `mdb-add`, `mdb-lookup`.

Requires GCC and POSIX sockets (Linux or macOS).

```sh
make clean   # remove binaries
```

---

## Usage

**1. Create and populate the database**

```sh
./mdb-add data/mydb      # prompts for a name and message, appends one record, can run again to add more records
```

**2. Start the database server in a separate terminal**

```sh
./mdb-lookup-server data/mydb 9999
```

Listens on port 9999. Reloads the database file on each incoming connection, so records added with `mdb-add` are visible to new connections without a restart.

**3. Start the HTTP server in a separate terminal**

```sh
./http-server 8080 www localhost 9999
```

Arguments: `<port> <web-root> <mdb-host> <mdb-port>`

Open `http://localhost:8080/` in a browser. Navigate to `/mdb-lookup` to search the database.

**Standalone database lookup (no HTTP)**

```sh
./mdb-lookup data/mydb   # interactive substring search, reads from stdin
```

**Download a file over HTTP**

```sh
./http-client localhost 8080 /index.html
```

Saves the response body as `index.html` in the current directory.

---

## Concurrency Model

The server forks a child process for every accepted connection. The child handles one HTTP request and exits; the parent closes its copy of the socket and loops back to `accept()`. Each child process has its own address space, so there is no shared state between connections and no locking needed. Each child also opens its own socket to `mdb-lookup-server`.

A `SIGCHLD` handler calls `waitpid(-1, NULL, WNOHANG)` in a loop so that multiple children finishing around the same time all get reaped. `sigaction` with `SA_RESTART` is used so a signal arriving during `accept()` does not cause it to return `EINTR`.

---

## Known Limitations

- **Percent-encoded traversal is not blocked.** The server rejects `/../` and trailing `/..` in the raw request URI, but a client sending `/%2e%2e/` or `/%2F..%2F` can bypass this check. A production server would decode the URI before validation.
- **No HTTP/1.1 keep-alive.** Every response closes the connection. Browsers that send `HTTP/1.1` requests are accepted, but the server responds as HTTP/1.0 and closes after each request.
- **No connection limit.** Each accepted connection forks a new process with no cap. A connection flood will exhaust the process table.
