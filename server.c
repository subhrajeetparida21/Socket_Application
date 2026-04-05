/*
 * server.c — Code Distribution Server
 *
 * Compile:  gcc server.c -o server -lpthread
 * Run:      ./server
 *
 * - Auto-detects your IP
 * - Finds an available port starting from 9001
 * - Accepts multiple client (lab machine) connections
 * - Prompts you to pick a .c file from the current folder
 * - Sends it to ALL connected clients at once, collects and prints their output
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* ── constants ──────────────────────────────────────────────────────── */
#define MAX_CLIENTS   32
#define START_PORT    9001
#define MAX_PORT      9100

/* ── network helpers ─────────────────────────────────────────────────── */
static int send_all(int fd, const void *buf, int len) {
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, int len) {
    char *p = (char *)buf;
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
    if (len > 0 && send_all(fd, s, (int)len) < 0) return -1;
    return 0;
}

static char *recv_str(int fd, uint32_t *out_len) {
    uint32_t net = 0;
    if (recv_all(fd, &net, 4) < 0) return NULL;
    uint32_t len = ntohl(net);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    if (len > 0 && recv_all(fd, buf, (int)len) < 0) { free(buf); return NULL; }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* ── client registry ─────────────────────────────────────────────────── */
typedef struct {
    int  fd;
    char ip[INET_ADDRSTRLEN];
    int  active;           /* 1 = connected */
} Client;

static Client       g_clients[MAX_CLIENTS];
static int          g_client_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── per-client accept thread: just detects disconnection ─────────────── */
static void *watch_client(void *arg) {
    Client *c = (Client *)arg;
    /* Block waiting — if client disconnects recv returns 0 */
    char tmp[4];
    while (recv(c->fd, tmp, sizeof(tmp), MSG_PEEK) > 0) {
        sleep(1);
    }
    pthread_mutex_lock(&g_mutex);
    c->active = 0;
    close(c->fd);
    printf("\n[server] Client %s disconnected.\n> ", c->ip);
    fflush(stdout);
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

/* ── acceptor thread ─────────────────────────────────────────────────── */
static void *acceptor(void *arg) {
    int listener = *(int *)arg;
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int cfd = accept(listener, (struct sockaddr *)&addr, &addrlen);
        if (cfd < 0) continue;

        pthread_mutex_lock(&g_mutex);
        if (g_client_count >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_mutex);
            close(cfd);
            continue;
        }
        Client *c  = &g_clients[g_client_count++];
        c->fd      = cfd;
        c->active  = 1;
        inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof(c->ip));
        printf("\n[server] Client connected: %s  (total: %d)\n> ",
               c->ip, g_client_count);
        fflush(stdout);
        pthread_mutex_unlock(&g_mutex);

        pthread_t tid;
        pthread_create(&tid, NULL, watch_client, c);
        pthread_detach(tid);
    }
    return NULL;
}

/* ── detect WSL ─────────────────────────────────────────────────────── */
static int is_wsl(void) {
    FILE *fp = fopen("/proc/version", "r");
    if (!fp) return 0;
    char line[256] = {0};
    size_t nr = fread(line, 1, sizeof(line) - 1, fp); (void)nr;
    fclose(fp);
    /* /proc/version contains "microsoft" or "Microsoft" on WSL */
    return (strstr(line, "icrosoft") != NULL);
}

/* Get the Windows host IP from WSL's /etc/resolv.conf (nameserver line) */
static int get_windows_host_ip(char *buf, size_t size) {
    FILE *fp = fopen("/etc/resolv.conf", "r");
    if (!fp) return 0;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "nameserver", 10) == 0) {
            char *p = line + 10;
            while (*p == ' ' || *p == '\t') p++;
            p[strcspn(p, " \t\n\r")] = '\0';
            strncpy(buf, p, size - 1); buf[size - 1] = '\0';
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    return 0;
}

/* ── get outbound IP (WSL virtual IP) ────────────────────────────────── */
static void get_local_ip(char *buf, size_t size) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    connect(s, (struct sockaddr *)&remote, sizeof(remote));
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s, (struct sockaddr *)&local, &len);
    close(s);
    inet_ntop(AF_INET, &local.sin_addr, buf, size);
}

/* ── list .c files in current directory ──────────────────────────────── */
static int list_c_files(char files[][256], int max) {
    DIR *d = opendir(".");
    int count = 0;
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max) {
        size_t n = strlen(e->d_name);
        if (n > 2 && strcmp(e->d_name + n - 2, ".c") == 0) {
            strncpy(files[count], e->d_name, 255);
            files[count][255] = '\0';
            count++;
        }
    }
    closedir(d);
    return count;
}

/* ── read a file into a malloc'd buffer ──────────────────────────────── */
static char *read_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp); (void)nr;
    buf[sz] = '\0';
    fclose(fp);
    *out_len = (uint32_t)sz;
    return buf;
}

/* ── per-client job thread: send code, collect output ────────────────── */
typedef struct {
    int      fd;
    char     ip[INET_ADDRSTRLEN];
    char    *source;
    uint32_t src_len;
    char    *output;       /* filled in by thread */
    uint32_t out_len;
    int      ok;
} JobArg;

static void *send_job(void *arg) {
    JobArg *j = (JobArg *)arg;
    if (send_str(j->fd, j->source, j->src_len) < 0) { j->ok = 0; return NULL; }
    j->output = recv_str(j->fd, &j->out_len);
    j->ok     = (j->output != NULL);
    return NULL;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
    char myip[INET_ADDRSTRLEN];
    get_local_ip(myip, sizeof(myip));

    /* Find an available port */
    int listener = -1, port = START_PORT;
    for (; port <= MAX_PORT; port++) {
        int fd  = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            listen(fd, 16);
            listener = fd;
            break;
        }
        printf("[server] Port %d is in use, trying %d...\n", port, port + 1);
        close(fd);
    }

    if (listener < 0) {
        fprintf(stderr, "[server] No available port in range %d-%d.\n",
                START_PORT, MAX_PORT);
        return 1;
    }

    if (is_wsl()) {
        char winip[INET_ADDRSTRLEN] = "(not found)";
        get_windows_host_ip(winip, sizeof(winip));
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════╗\n");
        printf("║              RUNNING INSIDE WSL — READ THIS             ║\n");
        printf("╠══════════════════════════════════════════════════════════╣\n");
        printf("║  WSL IP (not reachable from other machines): %-14s║\n", myip);
        printf("║  Windows host IP (use this for clients):     %-14s║\n", winip);
        printf("║                                                          ║\n");
        printf("║  Run this in Windows CMD/PowerShell (as Admin):          ║\n");
        printf("║  netsh interface portproxy add v4tov4 ^                  ║\n");
        printf("║    listenport=%d listenaddress=0.0.0.0 ^               ║\n", port);
        printf("║    connectport=%d connectaddress=%s       ║\n", port, myip);
        printf("╚══════════════════════════════════════════════════════════╝\n");
        printf("\n[server] Clients should connect to: %s : %d\n", winip, port);
    } else {
        printf("[server] Listening on %s : %d\n", myip, port);
        printf("[server] Clients connect to this IP and port.\n");
    }
    printf("[server] Waiting for clients to join...\n\n");

    /* Start acceptor thread */
    pthread_t atid;
    pthread_create(&atid, NULL, acceptor, &listener);
    pthread_detach(atid);

    /* Main loop: pick a file, distribute to all clients */
    char files[64][256];
    char input[64];

    while (1) {
        printf("> Press ENTER to distribute a job (or type 'list' to see clients)\n");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "list") == 0) {
            pthread_mutex_lock(&g_mutex);
            printf("  Connected clients: %d\n", g_client_count);
            for (int i = 0; i < g_client_count; i++) {
                if (g_clients[i].active)
                    printf("  [%d] %s\n", i + 1, g_clients[i].ip);
            }
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        /* Count active clients */
        pthread_mutex_lock(&g_mutex);
        int active = 0;
        for (int i = 0; i < g_client_count; i++)
            if (g_clients[i].active) active++;
        pthread_mutex_unlock(&g_mutex);

        if (active == 0) {
            printf("[server] No clients connected yet. Still waiting...\n");
            continue;
        }

        /* List .c files */
        int nfiles = list_c_files(files, 64);
        if (nfiles == 0) {
            printf("[server] No .c files found in the current directory.\n");
            continue;
        }

        printf("\nC files in current directory:\n");
        for (int i = 0; i < nfiles; i++)
            printf("  [%d] %s\n", i + 1, files[i]);
        printf("Pick a file (1-%d): ", nfiles);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        int choice = atoi(input);
        if (choice < 1 || choice > nfiles) {
            printf("[server] Invalid choice.\n");
            continue;
        }

        char *chosen = files[choice - 1];
        uint32_t src_len = 0;
        char *source = read_file(chosen, &src_len);
        if (!source) {
            printf("[server] Could not read %s\n", chosen);
            continue;
        }

        printf("[server] Sending '%s' to %d client(s)...\n\n", chosen, active);

        /* Spawn a thread per active client */
        pthread_mutex_lock(&g_mutex);
        JobArg *jobs     = (JobArg *)calloc(g_client_count, sizeof(JobArg));
        pthread_t *tids  = (pthread_t *)calloc(g_client_count, sizeof(pthread_t));
        int dispatched   = 0;

        for (int i = 0; i < g_client_count; i++) {
            if (!g_clients[i].active) continue;
            jobs[dispatched].fd      = g_clients[i].fd;
            strncpy(jobs[dispatched].ip, g_clients[i].ip, INET_ADDRSTRLEN - 1);
            jobs[dispatched].source  = source;
            jobs[dispatched].src_len = src_len;
            jobs[dispatched].output  = NULL;
            jobs[dispatched].ok      = 0;
            pthread_create(&tids[dispatched], NULL, send_job, &jobs[dispatched]);
            dispatched++;
        }
        pthread_mutex_unlock(&g_mutex);

        /* Wait for all threads */
        for (int i = 0; i < dispatched; i++)
            pthread_join(tids[i], NULL);

        /* Print results */
        for (int i = 0; i < dispatched; i++) {
            printf("──── Output from %s ────\n", jobs[i].ip);
            if (jobs[i].ok && jobs[i].output)
                printf("%s", jobs[i].output);
            else
                printf("(no response / disconnected)\n");
            printf("\n");
            free(jobs[i].output);
        }

        free(jobs);
        free(tids);
        free(source);
    }

    return 0;
}
