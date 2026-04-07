#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t g_load_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_active_jobs = 0;

static void inc_jobs(void) {
    pthread_mutex_lock(&g_load_mutex);
    g_active_jobs++;
    pthread_mutex_unlock(&g_load_mutex);
}

static void dec_jobs(void) {
    pthread_mutex_lock(&g_load_mutex);
    if (g_active_jobs > 0) g_active_jobs--;
    pthread_mutex_unlock(&g_load_mutex);
}

static uint32_t get_jobs(void) {
    uint32_t v;
    pthread_mutex_lock(&g_load_mutex);
    v = g_active_jobs;
    pthread_mutex_unlock(&g_load_mutex);
    return v;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static char *read_text_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char *msg = strdup("Failed to open output file.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        char *msg = strdup("Failed to read output file.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        char *msg = strdup("Memory allocation failed.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }

    if (size > 0) fread(buf, 1, (size_t)size, fp);
    buf[size] = '\0';
    fclose(fp);

    if (size == 0) {
        free(buf);
        char *msg = strdup("(program produced no output)\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }

    *out_len = (uint32_t)size;
    return buf;
}

static char *run_code(const char *source_code, uint32_t *out_len) {
    char source_tpl[] = "/tmp/lab_src_XXXXXX.c";
    char output_tpl[] = "/tmp/lab_out_XXXXXX.txt";
    char binary_path[PATH_MAX];

    int source_fd = mkstemps(source_tpl, 2);
    if (source_fd < 0) {
        char *msg = strdup("Failed to create temp source file.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }

    if (write_all(source_fd, source_code, strlen(source_code)) < 0) {
        close(source_fd);
        unlink(source_tpl);
        char *msg = strdup("Failed to write temp source file.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }
    close(source_fd);

    snprintf(binary_path, sizeof(binary_path), "%s.bin", source_tpl);

    int output_fd = mkstemps(output_tpl, 4);
    if (output_fd < 0) {
        unlink(source_tpl);
        char *msg = strdup("Failed to create temp output file.\n");
        *out_len = (uint32_t)strlen(msg);
        return msg;
    }
    close(output_fd);

    pid_t pid = fork();
    int status;

    if (pid == 0) {
        int ofd = open(output_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) {
            dup2(ofd, STDOUT_FILENO);
            dup2(ofd, STDERR_FILENO);
            close(ofd);
        }
        execlp("gcc", "gcc", source_tpl, "-O2", "-o", binary_path, NULL);
        _exit(1);
    }

    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        char *err = read_text_file(output_tpl, out_len);
        unlink(source_tpl);
        unlink(output_tpl);
        return err;
    }

    pid = fork();
    if (pid == 0) {
        int ofd = open(output_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) {
            dup2(ofd, STDOUT_FILENO);
            dup2(ofd, STDERR_FILENO);
            close(ofd);
        }
        execl(binary_path, binary_path, NULL);
        _exit(1);
    }

    waitpid(pid, &status, 0);

    char *result = read_text_file(output_tpl, out_len);

    unlink(source_tpl);
    unlink(binary_path);
    unlink(output_tpl);

    return result;
}

int main(void) {
    char server_ip[128];
    char port_str[16];
    int port;

    printf("Enter server IP address: ");
    fflush(stdout);
    if (!fgets(server_ip, sizeof(server_ip), stdin)) return 1;
    server_ip[strcspn(server_ip, "\n")] = '\0';

    printf("Enter server port [9001]: ");
    fflush(stdout);
    if (!fgets(port_str, sizeof(port_str), stdin)) return 1;
    port_str[strcspn(port_str, "\n")] = '\0';
    port = (port_str[0] != '\0') ? atoi(port_str) : 9001;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct hostent *he = gethostbyname(server_ip);
    if (!he) {
        fprintf(stderr, "Cannot resolve %s\n", server_ip);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    printf("[worker] Connected to server %s:%d\n", server_ip, port);
    fflush(stdout);

    while (1) {
        uint8_t msg;
        if (recv_u8(fd, &msg) < 0) break;

        if (msg == MSG_RUN_CODE) {
            uint32_t src_len = 0;
            char *source = recv_string(fd, &src_len);
            if (!source) break;

            inc_jobs();
            printf("[worker] Running job. Active jobs = %u\n", get_jobs());
            fflush(stdout);

            uint32_t out_len = 0;
            char *output = run_code(source, &out_len);

            dec_jobs();

            if (send_string(fd, output, out_len) < 0) {
                free(source);
                free(output);
                break;
            }

            free(source);
            free(output);
        } else if (msg == MSG_QUERY_LOAD) {
            send_u32(fd, get_jobs());
        } else {
            break;
        }
    }

    close(fd);
    return 0;
}
