#include "common.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;

    while (sent < len) {
        int n = (int)send(fd, p + sent, (size_t)(len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

int recv_all(int fd, void *buf, int len) {
    char *p = (char *)buf;
    int got = 0;

    while (got < len) {
        int n = (int)recv(fd, p + got, (size_t)(len - got), 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

int send_u8(int fd, uint8_t v) {
    return send_all(fd, &v, 1);
}

int recv_u8(int fd, uint8_t *v) {
    return recv_all(fd, v, 1);
}

int send_u32(int fd, uint32_t v) {
    uint32_t net = htonl(v);
    return send_all(fd, &net, 4);
}

int recv_u32(int fd, uint32_t *v) {
    uint32_t net = 0;
    if (recv_all(fd, &net, 4) < 0) return -1;
    *v = ntohl(net);
    return 0;
}

int send_string(int fd, const char *s, uint32_t len) {
    if (send_u32(fd, len) < 0) return -1;
    if (len > 0 && send_all(fd, s, (int)len) < 0) return -1;
    return 0;
}

char *recv_string(int fd, uint32_t *out_len) {
    uint32_t len = 0;
    if (recv_u32(fd, &len) < 0) return NULL;

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) return NULL;

    if (len > 0 && recv_all(fd, buf, (int)len) < 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}
