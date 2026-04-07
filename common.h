#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

#define MAX_NODES              10
#define DEFAULT_NODE_BASE_PORT 9010
#define DEFAULT_LB_PORT        9001

/* Message type tags sent from load-balancer → node */
#define MSG_QUERY_LOAD  1u
#define MSG_RUN_CODE    2u

int    send_all(int fd, const void *buf, int len);
int    recv_all(int fd, void *buf, int len);

int    send_u8(int fd, uint8_t value);
int    recv_u8(int fd, uint8_t *value);

int    send_u32(int fd, uint32_t value);
int    recv_u32(int fd, uint32_t *value);

int    send_double(int fd, double value);
int    recv_double(int fd, double *value);

int    send_string(int fd, const char *text, uint32_t len);
char  *recv_string(int fd, uint32_t *out_len);

#endif /* COMMON_H */
