#include "common.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int fd, const void *buf, int len) {
    const char *ptr = (const char *)buf;
    int sent = 0;

    while (sent < len) {
        int n = send(fd, ptr + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += n;
    }

    return 0;
}

int recv_all(int fd, void *buf, int len) {
    char *ptr = (char *)buf;
    int received = 0;

    while (received < len) {
        int n = recv(fd, ptr + received, len - received, 0);
        if (n <= 0) {
            return -1;
        }
        received += n;
    }

    return 0;
}

int send_u32(int fd, uint32_t value) {
    uint32_t net = htonl(value);
    return send_all(fd, &net, sizeof(net));
}

int recv_u32(int fd, uint32_t *value) {
    uint32_t net = 0;
    if (recv_all(fd, &net, sizeof(net)) < 0) {
        return -1;
    }
    *value = ntohl(net);
    return 0;
}

int send_double(int fd, double value) {
    return send_all(fd, &value, sizeof(value));
}

int recv_double(int fd, double *value) {
    return recv_all(fd, value, sizeof(*value));
}

int send_string(int fd, const char *text, uint32_t len) {
    if (send_u32(fd, len) < 0) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    return send_all(fd, text, (int)len);
}

char *recv_string(int fd, uint32_t *out_len) {
    uint32_t len = 0;
    char *buffer = NULL;

    if (recv_u32(fd, &len) < 0) {
        return NULL;
    }

    buffer = (char *)malloc(len + 1);
    if (!buffer) {
        return NULL;
    }

    if (len > 0 && recv_all(fd, buffer, (int)len) < 0) {
        free(buffer);
        return NULL;
    }

    buffer[len] = '\0';
    if (out_len) {
        *out_len = len;
    }

    return buffer;
}
