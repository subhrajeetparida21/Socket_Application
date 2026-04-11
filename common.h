#ifndef COMMON_H
#define COMMON_H

/*
 * common.h — Shared definitions for the distributed execution system.
 *
 * Architecture:
 *   Machine A (server.c):
 *     1. Compiles the user's .c file locally with gcc → produces a binary.
 *     2. Queries each connected node for its current load (active jobs).
 *     3. Lets the user pick a node manually OR auto-selects the least-loaded one.
 *     4. Sends the compiled binary to the chosen node.
 *     5. Receives and prints the output.
 *
 *   Machine B (node.c):
 *     - Runs a persistent listener (its "local server").
 *     - Handles two message types:
 *         MSG_QUERY_LOAD → reply with current active-job count (uint32).
 *         MSG_RUN_BINARY → recv binary, write to /tmp, chmod+x, run, send back stdout+stderr.
 */

#include <stdint.h>

/* ── port / sizing constants ─────────────────────────────────────────── */
#define DEFAULT_SERVER_PORT    9001   /* port the server listens on for node registration */
#define DEFAULT_NODE_BASE_PORT 9010   /* nodes listen here (can be overridden at startup)  */
#define MAX_NODES              32

/* ── message type tags (server → node) ──────────────────────────────── */
#define MSG_QUERY_LOAD   1u   /* ask node: how many active jobs?         */
#define MSG_RUN_BINARY   2u   /* send binary; node runs it, replies output */

/* ── wire-protocol helpers ───────────────────────────────────────────── */
int    send_all   (int fd, const void *buf, int len);
int    recv_all   (int fd, void *buf,       int len);

int    send_u32   (int fd, uint32_t value);
int    recv_u32   (int fd, uint32_t *value);

/* send_string / recv_string: 4-byte length prefix (network order) + data */
int    send_string(int fd, const char *text, uint32_t len);
char  *recv_string(int fd, uint32_t *out_len);          /* caller must free() */

#endif /* COMMON_H */
