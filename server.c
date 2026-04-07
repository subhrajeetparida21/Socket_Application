#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int fd;
    char ip[INET_ADDRSTRLEN];
    int active;
    uint32_t load;
} Client;

typedef struct {
    int idx;
    int fd;
    char ip[INET_ADDRSTRLEN];
} ActiveClient;

static Client g_clients[MAX_NODES];
static int g_client_count = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void get_local_ip(char *buf, size_t size) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        strncpy(buf, "127.0.0.1", size);
        if (size > 0) buf[size - 1] = '\0';
        return;
    }

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);

    if (connect(s, (struct sockaddr *)&remote, sizeof(remote)) < 0) {
        close(s);
        strncpy(buf, "127.0.0.1", size);
        if (size > 0) buf[size - 1] = '\0';
        return;
    }

    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    getsockname(s, (struct sockaddr *)&local, &len);
    close(s);
    inet_ntop(AF_INET, &local.sin_addr, buf, size);
}

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

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

static char *read_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz < 0) {
        fclose(fp);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    *out_len = (uint32_t)sz;
    return buf;
}

static void *acceptor(void *arg) {
    int listener = *(int *)arg;

    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int cfd = accept(listener, (struct sockaddr *)&addr, &addrlen);
        if (cfd < 0) continue;

        pthread_mutex_lock(&g_mutex);
        if (g_client_count >= MAX_NODES) {
            pthread_mutex_unlock(&g_mutex);
            close(cfd);
            continue;
        }

        Client *c = &g_clients[g_client_count++];
        c->fd = cfd;
        c->active = 1;
        c->load = 0;
        inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof(c->ip));

        printf("\n[server] Node connected: %s  (total: %d)\n> ",
               c->ip, g_client_count);
        fflush(stdout);

        pthread_mutex_unlock(&g_mutex);
    }

    return NULL;
}

static int snapshot_active_clients(ActiveClient *out, int max) {
    int n = 0;

    pthread_mutex_lock(&g_mutex);
    for (int i = 0; i < g_client_count && n < max; i++) {
        if (!g_clients[i].active) continue;
        out[n].idx = i;
        out[n].fd = g_clients[i].fd;
        strncpy(out[n].ip, g_clients[i].ip, INET_ADDRSTRLEN - 1);
        out[n].ip[INET_ADDRSTRLEN - 1] = '\0';
        n++;
    }
    pthread_mutex_unlock(&g_mutex);

    return n;
}

static void mark_client_dead(int idx) {
    pthread_mutex_lock(&g_mutex);
    if (idx >= 0 && idx < g_client_count && g_clients[idx].active) {
        g_clients[idx].active = 0;
        close(g_clients[idx].fd);
    }
    pthread_mutex_unlock(&g_mutex);
}

static int query_client_load(int fd, uint32_t *load) {
    if (send_u8(fd, MSG_QUERY_LOAD) < 0) return -1;
    if (recv_u32(fd, load) < 0) return -1;
    return 0;
}

static int choose_least_loaded_client(uint32_t *load_out) {
    ActiveClient active[MAX_NODES];
    int n = snapshot_active_clients(active, MAX_NODES);

    int best_idx = -1;
    uint32_t best_load = UINT32_MAX;

    for (int i = 0; i < n; i++) {
        uint32_t load = 0;
        if (query_client_load(active[i].fd, &load) < 0) {
            mark_client_dead(active[i].idx);
            continue;
        }

        pthread_mutex_lock(&g_mutex);
        if (active[i].idx >= 0 && active[i].idx < g_client_count) {
            g_clients[active[i].idx].load = load;
        }
        pthread_mutex_unlock(&g_mutex);

        if (load < best_load) {
            best_load = load;
            best_idx = active[i].idx;
        }
    }

    if (load_out) *load_out = best_load;
    return best_idx;
}

static void increment_load(int idx) {
    pthread_mutex_lock(&g_mutex);
    if (idx >= 0 && idx < g_client_count && g_clients[idx].active) {
        g_clients[idx].load++;
    }
    pthread_mutex_unlock(&g_mutex);
}

static void decrement_load(int idx) {
    pthread_mutex_lock(&g_mutex);
    if (idx >= 0 && idx < g_client_count && g_clients[idx].active && g_clients[idx].load > 0) {
        g_clients[idx].load--;
    }
    pthread_mutex_unlock(&g_mutex);
}

static int send_job_to_client(int fd, const char *source, uint32_t src_len,
                              char **out_text, uint32_t *out_len) {
    if (send_u8(fd, MSG_RUN_CODE) < 0) return -1;
    if (send_string(fd, source, src_len) < 0) return -1;
    *out_text = recv_string(fd, out_len);
    return (*out_text != NULL) ? 0 : -1;
}

int main(void) {
    char myip[INET_ADDRSTRLEN];
    get_local_ip(myip, sizeof(myip));

    int listener = create_listener(DEFAULT_LB_PORT);
    if (listener < 0) {
        perror("[server] listener");
        return 1;
    }

    printf("[server] Listening on %s:%d\n", myip, DEFAULT_LB_PORT);
    printf("[server] Waiting for lab systems to connect...\n\n");

    pthread_t atid;
    pthread_create(&atid, NULL, acceptor, &listener);
    pthread_detach(atid);

    char files[64][256];
    char input[64];

    while (1) {
        printf("> Press ENTER to assign a job, or type 'list' to see nodes\n");
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';

        if (strcmp(input, "list") == 0) {
            pthread_mutex_lock(&g_mutex);
            printf("[server] Connected nodes:\n");
            for (int i = 0; i < g_client_count; i++) {
                if (g_clients[i].active) {
                    printf("  [%d] %s  (load: %u)\n", i + 1, g_clients[i].ip, g_clients[i].load);
                }
            }
            pthread_mutex_unlock(&g_mutex);
            continue;
        }

        int nfiles = list_c_files(files, 64);
        if (nfiles == 0) {
            printf("[server] No .c files found in the current directory.\n");
            continue;
        }

        printf("\nC files in current directory:\n");
        for (int i = 0; i < nfiles; i++) {
            printf("  [%d] %s\n", i + 1, files[i]);
        }

        printf("Pick a file (1-%d): ", nfiles);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        int choice = atoi(input);
        if (choice < 1 || choice > nfiles) {
            printf("[server] Invalid choice.\n");
            continue;
        }

        uint32_t src_len = 0;
        char *source = read_file(files[choice - 1], &src_len);
        if (!source) {
            printf("[server] Could not read %s\n", files[choice - 1]);
            continue;
        }

        uint32_t selected_load = 0;
        int best = choose_least_loaded_client(&selected_load);
        if (best < 0) {
            printf("[server] No active nodes available.\n");
            free(source);
            continue;
        }

        pthread_mutex_lock(&g_mutex);
        int fd = g_clients[best].fd;
        char ip[INET_ADDRSTRLEN];
        strncpy(ip, g_clients[best].ip, sizeof(ip) - 1);
        ip[sizeof(ip) - 1] = '\0';
        pthread_mutex_unlock(&g_mutex);

        printf("[server] Sending '%s' to least loaded node %s (load=%u)\n",
               files[choice - 1], ip, selected_load);

        increment_load(best);

        char *output = NULL;
        uint32_t out_len = 0;

        if (send_job_to_client(fd, source, src_len, &output, &out_len) < 0) {
            printf("[server] Failed to get response from %s\n", ip);
            decrement_load(best);
            mark_client_dead(best);
            free(source);
            continue;
        }

        decrement_load(best);

        printf("──── Output from %s ────\n", ip);
        printf("%s", output);
        if (out_len == 0 || output[out_len - 1] != '\n') printf("\n");
        printf("\n");

        free(output);
        free(source);
    }

    close(listener);
    return 0;
}
