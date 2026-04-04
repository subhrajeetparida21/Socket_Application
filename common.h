#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define MAX_NODES 10
#define DEFAULT_NODE_PORT 9000
#define DEFAULT_CLIENT_PORT 9001

int send_all(int fd, const void *buf, int len);
int recv_all(int fd, void *buf, int len);
int send_u32(int fd, uint32_t value);
int recv_u32(int fd, uint32_t *value);
int send_double(int fd, double value);
int recv_double(int fd, double *value);
int send_string(int fd, const char *text, uint32_t len);
char *recv_string(int fd, uint32_t *out_len);

#endif
