/*
 * server.c — Binary Distribution Server (NO COMPILATION)
 *
 * Compile:  gcc server.c -o server -lpthread
 * Run:      ./server
 *
 * - Lists executable files (compiled binaries)
 * - Sends selected binary to all clients
 * - Receives and prints output
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── constants ───────────────────────── */
#define MAX_CLIENTS 32
#define START_PORT 9001
#define MAX_PORT 9100

/* ── network helpers ─────────────────── */
static int send_all(int fd, const void *buf, int len) {
    const char *p = buf;
    int sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, int len) {
    char *p = buf;
    int got = 0;
    while (got < len) {
        int n = recv(fd, p + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

static int send_str(int fd, const char *s, uint32_t len) {
    uint32_t net = htonl(len);
    if (send_all(fd, &net, 4) < 0) return -1;
    if (len > 0 && send_all(fd, s, len) < 0) return -1;
    return 0;
}

static char *recv_str(int fd, uint32_t *out_len) {
    uint32_t net = 0;
    if (recv_all(fd, &net, 4) < 0) return NULL;
    uint32_t len = ntohl(net);

    char *buf = malloc(len + 1);
    if (!buf) return NULL;

    if (len > 0 && recv_all(fd, buf, len) < 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ── client registry ─────────────────── */
typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    int active;
} Client;

static Client g_clients[MAX_CLIENTS];
static int g_client_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── detect disconnect ───────────────── */
static void *watch_client(void *arg) {
    Client *c = arg;
    char tmp[4];

    while (recv(c->fd, tmp, sizeof(tmp), MSG_PEEK) > 0)
        sleep(1);

    pthread_mutex_lock(&g_mutex);
    c->active = 0;
    close(c->fd);
    printf("\n[server] Client %s disconnected.\n> ", c->ip);
    fflush(stdout);
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

/* ── accept clients ─────────────────── */
static void *acceptor(void *arg) {
    int listener = *(int *)arg;

    while (1) {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);

        int cfd = accept(listener, (struct sockaddr *)&addr, &len);
        if (cfd < 0) continue;

        pthread_mutex_lock(&g_mutex);

        if (g_client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_mutex);
            close(cfd);
            continue;
        }

        Client *c = &g_clients[g_client_count++];
        c->fd = cfd;
        c->active = 1;

        inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof(c->ip));

        printf("\n[server] Client connected: %s (total: %d)\n> ",
               c->ip, g_client_count);

        fflush(stdout);
        pthread_mutex_unlock(&g_mutex);

        pthread_t tid;
        pthread_create(&tid, NULL, watch_client, c);
        pthread_detach(tid);
    }
}

/* ── get IP ─────────────────────────── */
static void get_local_ip(char *buf) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in remote = {0};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    connect(s, (struct sockaddr *)&remote, sizeof(remote));

    struct sockaddr_in local;
    socklen_t len = sizeof(local);

    getsockname(s, (struct sockaddr *)&local, &len);
    close(s);

    inet_ntop(AF_INET, &local.sin_addr, buf, INET_ADDRSTRLEN);
}

/* ── list executable files ──────────── */
static int list_exec_files(char files[][256], int max) {
    DIR *d = opendir(".");
    int count = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL && count < max) {
        if (e->d_type == DT_REG) {
            if (access(e->d_name, X_OK) == 0) {
                if (strcmp(e->d_name, "server") == 0) continue;

                strncpy(files[count], e->d_name, 255);
                files[count][255] = '\0';
                count++;
            }
        }
    }

    closedir(d);
    return count;
}

/* ── read file ─────────────────────── */
static char *read_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, fp);
    fclose(fp);

    buf[sz] = '\0';
    *out_len = sz;
    return buf;
}

/* ── job thread ───────────────────── */
typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    char *payload;
    uint32_t payload_len;
    char *output;
    uint32_t out_len;
    int ok;
} JobArg;

static void *send_job(void *arg) {
    JobArg *j = arg;

    if (send_str(j->fd, j->payload, j->payload_len) < 0) {
        j->ok = 0;
        return NULL;
    }

    j->output = recv_str(j->fd, &j->out_len);
    j->ok = (j->output != NULL);

    return NULL;
}

/* ── main ─────────────────────────── */
int main() {
    char ip[INET_ADDRSTRLEN];
    get_local_ip(ip);

    int listener = -1, port;

    for (port = START_PORT; port <= MAX_PORT; port++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            listen(fd, 16);
            listener = fd;
            break;
        }

        close(fd);
    }

    printf("[server] Listening on %s:%d\n", ip, port);

    pthread_t t;
    pthread_create(&t, NULL, acceptor, &listener);
    pthread_detach(t);

    char files[64][256];
    char input[64];

    while (1) {
        printf("\n> Press ENTER to send job\n");
        fgets(input, sizeof(input), stdin);

        int n = list_exec_files(files, 64);

        if (n == 0) {
            printf("No executable files found\n");
            continue;
        }

        printf("\nExecutable files:\n");
        for (int i = 0; i < n; i++)
            printf(" [%d] %s\n", i + 1, files[i]);

        printf("Pick file: ");
        fgets(input, sizeof(input), stdin);

        int choice = atoi(input);
        if (choice < 1 || choice > n) continue;

        uint32_t len;
        char *binary = read_file(files[choice - 1], &len);

        printf("[server] Sending %s (%u bytes)\n", files[choice - 1], len);

        pthread_mutex_lock(&g_mutex);

        pthread_t tids[MAX_CLIENTS];
        JobArg jobs[MAX_CLIENTS];
        int count = 0;

        for (int i = 0; i < g_client_count; i++) {
            if (!g_clients[i].active) continue;

            jobs[count].fd = g_clients[i].fd;
            strcpy(jobs[count].ip, g_clients[i].ip);
            jobs[count].payload = binary;
            jobs[count].payload_len = len;

            pthread_create(&tids[count], NULL, send_job, &jobs[count]);
            count++;
        }

        pthread_mutex_unlock(&g_mutex);

        for (int i = 0; i < count; i++)
            pthread_join(tids[i], NULL);

        for (int i = 0; i < count; i++) {
            printf("\n--- Output from %s ---\n", jobs[i].ip);
            if (jobs[i].ok) printf("%s\n", jobs[i].output);
            else printf("No response\n");

            free(jobs[i].output);
        }

        free(binary);
    }
}