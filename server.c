/*
 * server.c  –  Load-Balancer Server
 *
 * Accepts C source code from clients, queries all registered lab nodes for
 * their current load, forwards the job to the **least-loaded** node, and
 * relays the output back to the client.
 *
 * Protocol (client ↔ load-balancer):
 *   Client → LB : send_string(source_code, len)
 *   LB → Client : send_string(output, len)
 *
 * Protocol (load-balancer ↔ node):
 *   LB → Node : send_u32(MSG_QUERY_LOAD)   → Node replies send_u32(active_jobs)
 *   LB → Node : send_u32(MSG_RUN_CODE)
 *               send_string(source, len)    → Node replies send_string(output, len)
 */

#include "common.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── node registry ──────────────────────────────────────────────────── */
typedef struct {
    char ip[128];
    int  port;
} NodeInfo;

static NodeInfo g_nodes[MAX_NODES];
static int      g_node_count = 0;

/* ── connect to a node ──────────────────────────────────────────────── */
static int connect_to_node(const NodeInfo *n) {
    int fd;
    struct sockaddr_in addr;
    struct hostent    *he;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    /* resolve hostname or dotted-decimal IP */
    he = gethostbyname(n->ip);
    if (!he) {
        fprintf(stderr, "[lb] Cannot resolve node host %s\n", n->ip);
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(n->port);
    memcpy(&addr.sin_addr.s_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[lb] connect to node");
        close(fd);
        return -1;
    }
    return fd;
}

/* ── query a single node's load ─────────────────────────────────────── */
static int query_node_load(const NodeInfo *n) {
    int      fd = connect_to_node(n);
    uint32_t load = UINT32_MAX;     /* treat unreachable node as infinitely busy */

    if (fd < 0) return (int)UINT32_MAX;

    if (send_u32(fd, MSG_QUERY_LOAD) < 0 || recv_u32(fd, &load) < 0) {
        fprintf(stderr, "[lb] Failed to query load from %s:%d\n", n->ip, n->port);
        load = UINT32_MAX;
    }
    close(fd);
    return (int)load;
}

/* ── pick the least-loaded node index ──────────────────────────────── */
static int pick_node(void) {
    int best_idx  = -1;
    int best_load = INT_MAX;

    for (int i = 0; i < g_node_count; i++) {
        int load = query_node_load(&g_nodes[i]);
        printf("[lb] Node %s:%d  load=%d\n", g_nodes[i].ip, g_nodes[i].port, load);
        if (load < best_load) {
            best_load = load;
            best_idx  = i;
        }
    }
    return best_idx;
}

/* ── forward job to chosen node, get output ─────────────────────────── */
static char *run_on_node(const NodeInfo *n, const char *source,
                         uint32_t src_len, uint32_t *out_len) {
    int   fd;
    char *output = NULL;

    fd = connect_to_node(n);
    if (fd < 0) {
        const char *err = "Load balancer: could not connect to lab node.\n";
        output   = strdup(err);
        *out_len = (uint32_t)strlen(err);
        return output;
    }

    if (send_u32(fd, MSG_RUN_CODE) < 0 ||
        send_string(fd, source, src_len) < 0) {
        close(fd);
        const char *err = "Load balancer: failed to send code to node.\n";
        output   = strdup(err);
        *out_len = (uint32_t)strlen(err);
        return output;
    }

    output = recv_string(fd, out_len);
    close(fd);

    if (!output) {
        output   = strdup("Load balancer: failed to receive output from node.\n");
        *out_len = (uint32_t)strlen(output);
    }
    return output;
}

/* ── per-client thread ─────────────────────────────────────────────── */
typedef struct { int fd; } ClientArg;

static void *handle_client(void *arg) {
    int      client_fd = ((ClientArg *)arg)->fd;
    uint32_t code_len  = 0;
    uint32_t out_len   = 0;
    char    *source    = NULL;
    char    *output    = NULL;
    int      node_idx;
    free(arg);

    source = recv_string(client_fd, &code_len);
    if (!source) {
        fprintf(stderr, "[lb] Failed to receive source from client\n");
        close(client_fd);
        return NULL;
    }

    node_idx = pick_node();
    if (node_idx < 0) {
        const char *err = "No lab nodes available.\n";
        send_string(client_fd, err, (uint32_t)strlen(err));
        free(source);
        close(client_fd);
        return NULL;
    }

    printf("[lb] Routing job to node %s:%d\n",
           g_nodes[node_idx].ip, g_nodes[node_idx].port);
    fflush(stdout);

    output = run_on_node(&g_nodes[node_idx], source, code_len, &out_len);
    send_string(client_fd, output, out_len);

    free(source);
    free(output);
    close(client_fd);
    return NULL;
}

/* ── listener ───────────────────────────────────────────────────────── */
static int create_listener(int port) {
    int fd  = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    struct sockaddr_in addr;

    if (fd < 0) { perror("socket"); exit(1); }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); exit(1);
    }
    if (listen(fd, 16) < 0) {
        perror("listen"); close(fd); exit(1);
    }
    return fd;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
    char input[128];
    int  lb_port;
    int  listener;

    /* ── 1. Configure load-balancer port ── */
    printf("Enter load-balancer port [%d]: ", DEFAULT_LB_PORT);
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin)) {
        input[strcspn(input, "\n")] = '\0';
        lb_port = (input[0] != '\0') ? atoi(input) : DEFAULT_LB_PORT;
    } else {
        lb_port = DEFAULT_LB_PORT;
    }

    /* ── 2. Register lab nodes ── */
    printf("How many lab nodes (1–%d)? ", MAX_NODES);
    fflush(stdout);
    if (!fgets(input, sizeof(input), stdin)) { g_node_count = 0; }
    else {
        input[strcspn(input, "\n")] = '\0';
        g_node_count = atoi(input);
        if (g_node_count < 1)        g_node_count = 1;
        if (g_node_count > MAX_NODES) g_node_count = MAX_NODES;
    }

    for (int i = 0; i < g_node_count; i++) {
        char def_port[16];
        snprintf(def_port, sizeof(def_port), "%d", DEFAULT_NODE_BASE_PORT + i);

        printf("Node %d IP [127.0.0.1]: ", i + 1);
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin)) {
            input[strcspn(input, "\n")] = '\0';
            snprintf(g_nodes[i].ip, sizeof(g_nodes[i].ip),
                     "%s", (input[0] != '\0') ? input : "127.0.0.1");
        } else {
            snprintf(g_nodes[i].ip, sizeof(g_nodes[i].ip), "127.0.0.1");
        }

        printf("Node %d port [%s]: ", i + 1, def_port);
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin)) {
            input[strcspn(input, "\n")] = '\0';
            g_nodes[i].port = (input[0] != '\0') ? atoi(input) : atoi(def_port);
        } else {
            g_nodes[i].port = atoi(def_port);
        }

        printf("[lb] Registered node %d → %s:%d\n",
               i + 1, g_nodes[i].ip, g_nodes[i].port);
    }

    /* ── 3. Start accepting clients ── */
    listener = create_listener(lb_port);
    printf("[lb] Load-balancer ready on port %d  (%d node(s) registered)\n",
           lb_port, g_node_count);
    fflush(stdout);

    while (1) {
        ClientArg *ca  = (ClientArg *)malloc(sizeof(ClientArg));
        pthread_t  tid;
        if (!ca) continue;

        ca->fd = accept(listener, NULL, NULL);
        if (ca->fd < 0) { free(ca); continue; }

        pthread_create(&tid, NULL, handle_client, ca);
        pthread_detach(tid);
    }

    return 0;
}
