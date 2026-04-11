/*
 * common.c — Wire-protocol helper implementations.
 */

#include "common.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

/* ── low-level send / recv loops ─────────────────────────────────────── */

int send_all(int fd, const void *buf, int len) {
    const char *p   = (const char *)buf;
    int         sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int recv_all(int fd, void *buf, int len) {
    char *p   = (char *)buf;
    int   got = 0;
    while (got < len) {
        int n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ── uint32 helpers ──────────────────────────────────────────────────── */

int send_u32(int fd, uint32_t value) {
    uint32_t net = htonl(value);
    return send_all(fd, &net, (int)sizeof(net));
}

int recv_u32(int fd, uint32_t *value) {
    uint32_t net = 0;
    if (recv_all(fd, &net, (int)sizeof(net)) < 0) return -1;
    *value = ntohl(net);
    return 0;
}

/* ── length-prefixed string helpers ─────────────────────────────────── */

int send_string(int fd, const char *text, uint32_t len) {
    if (send_u32(fd, len) < 0) return -1;
    if (len == 0)              return 0;
    return send_all(fd, text, (int)len);
}

char *recv_string(int fd, uint32_t *out_len) {
    uint32_t len = 0;
    if (recv_u32(fd, &len) < 0) return NULL;

    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;

    if (len > 0 && recv_all(fd, buf, (int)len) < 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}
