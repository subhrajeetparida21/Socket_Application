/*
 * client.c — Lab Worker Client
 *
 * Compile:  gcc client.c -o client -lpthread
 * Run:      ./client
 *
 * - Asks for the server IP
 * - Connects to the server
 * - Waits for C source code from server
 * - Compiles and runs it, sends output back
 * - Stays connected, ready for the next job
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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

/* ── run binary payload and capture stdout+stderr ─────────────────────── */
static char *run_binary(const char *binary, uint32_t bin_len) {
    char bin_tpl[] = "/tmp/lab_exec_XXXXXX";
    char out_tpl[] = "/tmp/lab_out_XXXXXX.txt";
    int bin_fd = mkstemp(bin_tpl);
    if (bin_fd < 0) return strdup("client: failed to create temp binary file.\n");

    if (write_all(bin_fd, binary, bin_len) < 0) {
        close(bin_fd);
        unlink(bin_tpl);
        return strdup("client: failed to write binary payload.\n");
    }
    close(bin_fd);
    chmod(bin_tpl, 0700);

    int out_fd = mkstemps(out_tpl, 4);
    if (out_fd < 0) {
        unlink(bin_tpl);
        return strdup("client: failed to create output file.\n");
    }
    close(out_fd);

    pid_t pid = fork();
    if (pid == 0) {
        int ofd = open(out_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDOUT_FILENO); dup2(ofd, STDERR_FILENO); close(ofd); }
        execl(bin_tpl, bin_tpl, NULL);
        _exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    char *result = NULL;
    FILE *fp = fopen(out_tpl, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (sz == 0) {
            result = strdup("(program produced no output)\n");
        } else {
            result = (char *)malloc(sz + 1);
            size_t nr = fread(result, 1, sz, fp); (void)nr;
            result[sz] = '\0';
        }
        fclose(fp);
    } else {
        result = strdup("(could not read output file)\n");
    }

    unlink(bin_tpl);
    unlink(out_tpl);
    return result;
}

/* ── main ────────────────────────────────────────────────────────────── */
int main(void) {
    char server_ip[128];
    char port_str[16];
    int  port;

    printf("Enter server IP address: ");
    fflush(stdout);
    if (!fgets(server_ip, sizeof(server_ip), stdin)) return 1;
    server_ip[strcspn(server_ip, "\n")] = '\0';

    printf("Enter server port [9001]: ");
    fflush(stdout);
    if (!fgets(port_str, sizeof(port_str), stdin)) return 1;
    port_str[strcspn(port_str, "\n")] = '\0';
    port = (port_str[0] != '\0') ? atoi(port_str) : 9001;

    /* Connect */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct hostent *he = gethostbyname(server_ip);
    if (!he) { fprintf(stderr, "Cannot resolve %s\n", server_ip); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    memcpy(&addr.sin_addr.s_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        fprintf(stderr, "Could not connect to %s:%d\n", server_ip, port);
        return 1;
    }

    printf("[client] Connected to server %s:%d\n", server_ip, port);
    printf("[client] Waiting for jobs from server...\n");
    fflush(stdout);

    /* Job loop */
    while (1) {
        uint32_t src_len = 0;
        char *source = recv_str(fd, &src_len);
        if (!source) {
            printf("\n[client] Server disconnected. Exiting.\n");
            break;
        }

        printf("[client] Received job (%u bytes) — running binary...\n", src_len);
        fflush(stdout);

        char *output = run_binary(source, src_len);
        uint32_t out_len = (uint32_t)strlen(output);

        if (send_str(fd, output, out_len) < 0) {
            printf("[client] Failed to send output. Server may have disconnected.\n");
            free(source);
            free(output);
            break;
        }

        printf("[client] Output sent. Waiting for next job...\n");
        fflush(stdout);

        free(source);
        free(output);
    }

    close(fd);
    return 0;
}
