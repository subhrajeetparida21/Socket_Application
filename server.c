#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int active;
    int busy;
    int socket_fd;
    char name[64];
    pthread_mutex_t lock;
} LabNode;

static LabNode nodes[MAX_NODES];
static pthread_mutex_t nodes_mutex = PTHREAD_MUTEX_INITIALIZER;

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    struct sockaddr_in addr;

    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        exit(1);
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        exit(1);
    }

    return fd;
}

static void remove_node(int index) {
    if (nodes[index].active) {
        close(nodes[index].socket_fd);
        nodes[index].active = 0;
        nodes[index].busy = 0;
        nodes[index].name[0] = '\0';
    }
}

static int reserve_best_node(double *best_load) {
    int chosen = -1;
    double chosen_load = 0.0;

    pthread_mutex_lock(&nodes_mutex);
    for (int i = 0; i < MAX_NODES; i++) {
        double current_load = 0.0;

        if (!nodes[i].active || nodes[i].busy) {
            continue;
        }

        pthread_mutex_lock(&nodes[i].lock);
        if (send_string(nodes[i].socket_fd, "LOAD", 4) < 0 ||
            recv_double(nodes[i].socket_fd, &current_load) < 0) {
            pthread_mutex_unlock(&nodes[i].lock);
            remove_node(i);
            continue;
        }
        pthread_mutex_unlock(&nodes[i].lock);

        if (chosen == -1 || current_load < chosen_load) {
            chosen = i;
            chosen_load = current_load;
        }
    }

    if (chosen >= 0) {
        nodes[chosen].busy = 1;
    }
    pthread_mutex_unlock(&nodes_mutex);

    if (best_load && chosen >= 0) {
        *best_load = chosen_load;
    }

    return chosen;
}

static void release_node(int index) {
    pthread_mutex_lock(&nodes_mutex);
    if (index >= 0 && index < MAX_NODES && nodes[index].active) {
        nodes[index].busy = 0;
    }
    pthread_mutex_unlock(&nodes_mutex);
}

static void *client_worker(void *arg) {
    int client_fd = *(int *)arg;
    uint32_t code_len = 0;
    char *source = NULL;
    int node_index = -1;
    double load = 0.0;
    char *output = NULL;
    uint32_t output_len = 0;

    free(arg);

    source = recv_string(client_fd, &code_len);
    if (!source) {
        close(client_fd);
        return NULL;
    }

    node_index = reserve_best_node(&load);
    if (node_index < 0) {
        const char *message = "No lab node is currently available.\n";
        send_string(client_fd, message, (uint32_t)strlen(message));
        free(source);
        close(client_fd);
        return NULL;
    }

    pthread_mutex_lock(&nodes[node_index].lock);
    if (send_string(nodes[node_index].socket_fd, "RUN", 3) < 0 ||
        send_string(nodes[node_index].socket_fd, source, code_len) < 0) {
        pthread_mutex_unlock(&nodes[node_index].lock);
        pthread_mutex_lock(&nodes_mutex);
        remove_node(node_index);
        pthread_mutex_unlock(&nodes_mutex);
        release_node(node_index);
        free(source);
        close(client_fd);
        return NULL;
    }

    output = recv_string(nodes[node_index].socket_fd, &output_len);
    pthread_mutex_unlock(&nodes[node_index].lock);

    if (!output) {
        pthread_mutex_lock(&nodes_mutex);
        remove_node(node_index);
        pthread_mutex_unlock(&nodes_mutex);
        release_node(node_index);
        free(source);
        close(client_fd);
        return NULL;
    }

    send_string(client_fd, output, output_len);

    printf("Client job sent to node %s (load %.2f)\n", nodes[node_index].name, load);

    free(output);
    free(source);
    release_node(node_index);
    close(client_fd);
    return NULL;
}

static void *accept_clients(void *arg) {
    int listen_fd = *(int *)arg;

    while (1) {
        int *client_fd = (int *)malloc(sizeof(int));
        pthread_t tid;

        if (!client_fd) {
            continue;
        }

        *client_fd = accept(listen_fd, NULL, NULL);
        if (*client_fd < 0) {
            free(client_fd);
            continue;
        }

        pthread_create(&tid, NULL, client_worker, client_fd);
        pthread_detach(tid);
    }

    return NULL;
}

static void *accept_nodes(void *arg) {
    int listen_fd = *(int *)arg;

    while (1) {
        int node_fd = accept(listen_fd, NULL, NULL);
        uint32_t name_len = 0;
        char *name = NULL;
        int slot = -1;

        if (node_fd < 0) {
            continue;
        }

        name = recv_string(node_fd, &name_len);
        if (!name) {
            close(node_fd);
            continue;
        }

        pthread_mutex_lock(&nodes_mutex);
        for (int i = 0; i < MAX_NODES; i++) {
            if (!nodes[i].active) {
                slot = i;
                nodes[i].active = 1;
                nodes[i].busy = 0;
                nodes[i].socket_fd = node_fd;
                snprintf(nodes[i].name, sizeof(nodes[i].name), "%s", name);
                break;
            }
        }
        pthread_mutex_unlock(&nodes_mutex);

        if (slot < 0) {
            const char *full = "Server already has 10 nodes.\n";
            send_string(node_fd, full, (uint32_t)strlen(full));
            close(node_fd);
        } else {
            printf("Node connected: %s\n", name);
        }

        free(name);
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    int node_port = DEFAULT_NODE_PORT;
    int client_port = DEFAULT_CLIENT_PORT;
    int node_listener;
    int client_listener;
    pthread_t node_thread;
    pthread_t client_thread;

    if (argc >= 2) {
        node_port = atoi(argv[1]);
    }
    if (argc >= 3) {
        client_port = atoi(argv[2]);
    }

    for (int i = 0; i < MAX_NODES; i++) {
        pthread_mutex_init(&nodes[i].lock, NULL);
    }

    node_listener = create_listener(node_port);
    client_listener = create_listener(client_port);

    printf("Server ready\n");
    printf("Node port   : %d\n", node_port);
    printf("Client port : %d\n", client_port);

    pthread_create(&node_thread, NULL, accept_nodes, &node_listener);
    pthread_create(&client_thread, NULL, accept_clients, &client_listener);

    pthread_join(node_thread, NULL);
    pthread_join(client_thread, NULL);
    return 0;
}
