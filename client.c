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

/* ── compile and run source code, return stdout+stderr ───────────────── */
static char *run_code(const char *source) {
    char src_tpl[] = "/tmp/lab_src_XXXXXX.c";
    char out_tpl[] = "/tmp/lab_out_XXXXXX.txt";
    char bin_path[PATH_MAX];

    /* Write source to temp file */
    int src_fd = mkstemps(src_tpl, 2);    /* .c suffix → len 2 */
    if (src_fd < 0) return strdup("client: failed to create temp file.\n");
    size_t wn = write(src_fd, source, strlen(source)); (void)wn;
    close(src_fd);

    snprintf(bin_path, sizeof(bin_path), "%s.bin", src_tpl);

    /* Create output capture file */
    int out_fd = mkstemps(out_tpl, 4);    /* .txt suffix → len 4 */
    if (out_fd < 0) { unlink(src_tpl); return strdup("client: failed to create output file.\n"); }
    close(out_fd);

    /* ── Compile ── */
    pid_t pid = fork();
    if (pid == 0) {
        int ofd = open(out_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDOUT_FILENO); dup2(ofd, STDERR_FILENO); close(ofd); }
        execlp("gcc", "gcc", src_tpl, "-O2", "-o", bin_path, NULL);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);

    /* On compile error, return compiler output */
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FILE *fp = fopen(out_tpl, "r");
        char *err = NULL;
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
            err = (char *)malloc(sz + 64);
            snprintf(err, 64, "[compile error]\n");
            size_t nr = fread(err + strlen(err), 1, sz, fp); (void)nr;
            err[strlen(err)] = '\0';
            fclose(fp);
        } else {
            err = strdup("[compile error — could not read output]\n");
        }
        unlink(src_tpl); unlink(out_tpl);
        return err;
    }

    /* ── Run ── */
    pid = fork();
    if (pid == 0) {
        int ofd = open(out_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDOUT_FILENO); dup2(ofd, STDERR_FILENO); close(ofd); }
        execl(bin_path, bin_path, NULL);
        _exit(1);
    }
    waitpid(pid, &status, 0);

    /* Read output */
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

    unlink(src_tpl);
    unlink(bin_path);
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

        printf("[client] Received job (%u bytes) — compiling and running...\n", src_len);
        fflush(stdout);

        char *output = run_code(source);
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
