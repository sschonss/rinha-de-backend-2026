// Bare HTTP/1.1 server on UDS, single-thread epoll, multi-worker via fork.
// Replaces PHP/Swoole entirely; calls ivf_score_json directly.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include "vector_search.h"

#define MAX_CONNS    4096
#define BUF_SIZE     4096

static const char *RESPONSES[6] = {
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 35\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}",
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}",
};
static int  RLEN[6];

static const char *NOT_FOUND = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char *READY_OK  = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
static int NF_LEN, RDY_LEN;

typedef struct {
    int  fd;
    int  used;
    int  buf_len;
    char buf[BUF_SIZE];
} conn_t;

static conn_t conns[MAX_CONNS];

static int next_slot = 0;
static int find_conn_slot(void) {
    for (int t = 0; t < MAX_CONNS; t++) {
        int i = (next_slot + t) % MAX_CONNS;
        if (!conns[i].used) { next_slot = (i + 1) % MAX_CONNS; return i; }
    }
    return -1;
}

static void close_conn(int epfd, int idx) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, conns[idx].fd, NULL);
    close(conns[idx].fd);
    conns[idx].used = 0;
    conns[idx].buf_len = 0;
}

static int safe_write(int fd, const char *p, int n) {
    int off = 0;
    while (off < n) {
        int w = (int)write(fd, p + off, (size_t)(n - off));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += w;
    }
    return 0;
}

// returns bytes consumed, 0 if incomplete, -1 if error/close
static int try_handle(conn_t *c) {
    char *hdr_end = memmem(c->buf, c->buf_len, "\r\n\r\n", 4);
    if (!hdr_end) return 0;
    int hdr_len = (int)(hdr_end - c->buf) + 4;

    char *sp = memchr(c->buf, ' ', hdr_len);
    if (!sp) return -1;
    char *uri = sp + 1;
    char *sp2 = memchr(uri, ' ', hdr_len - (uri - c->buf));
    if (!sp2) return -1;
    int uri_len = (int)(sp2 - uri);

    int is_post = (c->buf_len >= 4 && memcmp(c->buf, "POST", 4) == 0);
    int is_fraud = (uri_len == 12 && memcmp(uri, "/fraud-score", 12) == 0);
    int is_ready = (uri_len == 6  && memcmp(uri, "/ready", 6)  == 0);

    int cl = 0;
    if (is_post) {
        char *p = c->buf;
        char *end = c->buf + hdr_len;
        // case-insensitive scan for "content-length:"
        while (p < end - 16) {
            if ((p[0]|0x20)=='c' && (p[1]|0x20)=='o' && (p[2]|0x20)=='n' &&
                (p[3]|0x20)=='t' && (p[4]|0x20)=='e' && (p[5]|0x20)=='n' &&
                (p[6]|0x20)=='t' && p[7]=='-' &&
                (p[8]|0x20)=='l' && (p[9]|0x20)=='e' && (p[10]|0x20)=='n' &&
                (p[11]|0x20)=='g' && (p[12]|0x20)=='t' && (p[13]|0x20)=='h' &&
                p[14]==':') {
                p += 15;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                cl = (int)strtol(p, NULL, 10);
                break;
            }
            p++;
        }
    }

    int total = hdr_len + cl;
    if (c->buf_len < total) return 0;

    if (is_ready) {
        if (safe_write(c->fd, READY_OK, RDY_LEN) < 0) return -1;
    } else if (is_post && is_fraud) {
        int count = ivf_score_json(c->buf + hdr_len, cl);
        if (count < 0 || count > 5) count = 0;
        if (safe_write(c->fd, RESPONSES[count], RLEN[count]) < 0) return -1;
    } else {
        if (safe_write(c->fd, NOT_FOUND, NF_LEN) < 0) return -1;
    }
    return total;
}

static void run_worker(int listen_fd) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLEXCLUSIVE, .data.u32 = 0xFFFFFFFFu };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        struct epoll_event ev2 = { .events = EPOLLIN, .data.u32 = 0xFFFFFFFFu };
        epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev2);
    }

    struct epoll_event events[256];
    for (;;) {
        int n = epoll_wait(epfd, events, 256, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        for (int i = 0; i < n; i++) {
            unsigned int tag = events[i].data.u32;
            if (tag == 0xFFFFFFFFu) {
                for (;;) {
                    int cfd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        break;
                    }
                    int idx = find_conn_slot();
                    if (idx < 0) { close(cfd); continue; }
                    conns[idx].fd = cfd;
                    conns[idx].used = 1;
                    conns[idx].buf_len = 0;
                    struct epoll_event cev = { .events = EPOLLIN, .data.u32 = (unsigned)idx };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
            } else {
                int idx = (int)tag;
                conn_t *c = &conns[idx];
                for (;;) {
                    ssize_t r = read(c->fd, c->buf + c->buf_len, (size_t)(BUF_SIZE - c->buf_len));
                    if (r > 0) {
                        c->buf_len += (int)r;
                        for (;;) {
                            int consumed = try_handle(c);
                            if (consumed < 0) { close_conn(epfd, idx); goto next_event; }
                            if (consumed == 0) break;
                            if (consumed < c->buf_len) {
                                memmove(c->buf, c->buf + consumed, (size_t)(c->buf_len - consumed));
                                c->buf_len -= consumed;
                            } else {
                                c->buf_len = 0;
                                break;
                            }
                        }
                        if (c->buf_len >= BUF_SIZE) { close_conn(epfd, idx); break; }
                        continue;
                    } else if (r == 0) {
                        close_conn(epfd, idx); break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        close_conn(epfd, idx); break;
                    }
                }
            }
            next_event: ;
        }
    }
}

int main(void) {
    const char *sock_path  = getenv("SOCK_PATH");  if (!sock_path)  sock_path  = "/sockets/api.sock";
    const char *index_path = getenv("INDEX_PATH"); if (!index_path) index_path = "/data/index.bin";
    const char *e_fast = getenv("FAST_NPROBE");
    const char *e_mid  = getenv("MID_NPROBE");
    const char *e_full = getenv("FULL_NPROBE");
    const char *e_work = getenv("WORKERS");
    const char *e_warm = getenv("WARMUP");
    int fast    = e_fast ? atoi(e_fast) : 8;
    int mid     = e_mid  ? atoi(e_mid)  : 0;
    int full    = e_full ? atoi(e_full) : 24;
    int workers = e_work ? atoi(e_work) : 4;
    int warmup  = e_warm ? atoi(e_warm) : 500;
    if (workers < 1) workers = 1;
    if (workers > 32) workers = 32;

    for (int i = 0; i < 6; i++) RLEN[i] = (int)strlen(RESPONSES[i]);
    NF_LEN  = (int)strlen(NOT_FOUND);
    RDY_LEN = (int)strlen(READY_OK);

    if (ivf_init(index_path, fast, mid, full) != 0) {
        fprintf(stderr, "ivf_init failed for %s\n", index_path);
        return 1;
    }
    if (warmup > 0) ivf_warmup(warmup);
    fprintf(stderr, "[master] index loaded fast=%d mid=%d full=%d warmup=%d workers=%d\n",
            fast, mid, full, warmup, workers);

    unlink(sock_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    chmod(sock_path, 0666);
    if (listen(lfd, 4096) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[master] listening on %s\n", sock_path);

    // Signal a "ready" file so any healthcheck can poll it.
    int rfd = open("/tmp/ready", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rfd >= 0) { (void)!write(rfd, "1", 1); close(rfd); }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < workers; i++) {
        pid_t p = fork();
        if (p == 0) { run_worker(lfd); _exit(0); }
        else if (p < 0) { perror("fork"); }
    }
    run_worker(lfd);
    return 0;
}
