/*
 * node.c — Lab-machine worker (Machine B)
 *
 * Compile:  gcc node.c common.c -o node -lpthread
 * Run:      ./node          (uses DEFAULT_NODE_BASE_PORT = 9010)
 *           ./node 9011     (optional: pass port as argument)
 *
 * Responsibilities:
 *   1. Maintains a LOCAL SERVER that listens for connections from Machine A.
 *   2. Responds to load queries (MSG_QUERY_LOAD) with its active-job count.
 *   3. Receives a compiled binary (MSG_RUN_BINARY), executes it in a sandboxed
 *      temp directory, and returns stdout+stderr to Machine A.
 *
 * Each incoming connection is handled in its own thread so multiple jobs
 * can be accepted simultaneously (load is tracked atomically).
 */

#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── active-job counter (load tracking) ─────────────────────────────── */
static pthread_mutex_t g_load_mutex  = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_active_jobs = 0;

static void      inc_load(void) { pthread_mutex_lock(&g_load_mutex); g_active_jobs++;                        pthread_mutex_unlock(&g_load_mutex); }
static void      dec_load(void) { pthread_mutex_lock(&g_load_mutex); if (g_active_jobs) g_active_jobs--;    pthread_mutex_unlock(&g_load_mutex); }
static uint32_t  get_load(void) { uint32_t v; pthread_mutex_lock(&g_load_mutex); v = g_active_jobs; pthread_mutex_unlock(&g_load_mutex); return v; }

/* ── write_all (for writing binary data to a file descriptor) ────────── */
static int write_all(int fd, const void *buf, size_t len) {
    const char *p    = (const char *)buf;
    size_t      sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* ── read output file into a malloc'd string ─────────────────────────── */
static char *read_output_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        const char *msg = "(node: could not open output file)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(fp);
        const char *msg = "(program produced no output)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        const char *msg = "(node: malloc failed reading output)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    size_t nr = fread(buf, 1, (size_t)sz, fp); (void)nr;
    buf[sz] = '\0';
    fclose(fp);
    *out_len = (uint32_t)sz;
    return buf;
}

/*
 * run_binary()
 *
 * Receives the raw bytes of an already-compiled ELF binary,
 * writes them to a temp file, chmod 0700, forks, executes,
 * captures stdout+stderr, and returns the output string.
 *
 * The server compiles on Machine A; this node just RUNS the binary.
 */
static char *run_binary(const char *binary, uint32_t bin_len, uint32_t *out_len) {
    /* Temp file for the binary */
    char bin_tpl[] = "./lab_node_bin_XXXXXX";
    int  bin_fd    = mkstemp(bin_tpl);
    if (bin_fd < 0) {
        const char *msg = "(node: mkstemp failed for binary)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    if (write_all(bin_fd, binary, (size_t)bin_len) < 0) {
        close(bin_fd); unlink(bin_tpl);
        const char *msg = "(node: failed to write binary to temp file)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    close(bin_fd);
    chmod(bin_tpl, 0700);   /* make executable */

    /* Temp file to capture stdout + stderr */
    char out_tpl[] = "./lab_node_out_XXXXXX.txt";
    int  out_fd    = mkstemps(out_tpl, 4);
    if (out_fd < 0) {
        unlink(bin_tpl);
        const char *msg = "(node: mkstemps failed for output capture)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    close(out_fd);

    /* Fork and execute */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stdout & stderr into output file, then exec */
        int ofd = open(out_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) {
            dup2(ofd, STDOUT_FILENO);
            dup2(ofd, STDERR_FILENO);
            close(ofd);
        }
        execl(bin_tpl, bin_tpl, NULL);
        _exit(1);
    }

    if (pid < 0) {
        unlink(bin_tpl);
        unlink(out_tpl);
        const char *msg = "(node: fork() failed)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    char *result = read_output_file(out_tpl, out_len);

    unlink(bin_tpl);
    unlink(out_tpl);
    return result;
}

/* ── per-connection handler thread ──────────────────────────────────── */
static void *handle_connection(void *arg) {
    int      fd  = *(int *)arg;
    free(arg);

    uint32_t msg_type = 0;
    if (recv_u32(fd, &msg_type) < 0) {
        close(fd);
        return NULL;
    }

    /* ── MSG_QUERY_LOAD ── */
    if (msg_type == MSG_QUERY_LOAD) {
        uint32_t load = get_load();
        send_u32(fd, load);
        printf("[node] Load query answered: %u active job(s)\n", load);
        fflush(stdout);

    /* ── MSG_RUN_BINARY ── */
    } else if (msg_type == MSG_RUN_BINARY) {
        uint32_t  bin_len = 0;
        char     *binary  = recv_string(fd, &bin_len);
        if (!binary) {
            fprintf(stderr, "[node] Failed to receive binary payload.\n");
            close(fd);
            return NULL;
        }

        inc_load();
        printf("[node] Received binary (%u bytes). Running... (load now: %u)\n",
               bin_len, get_load());
        fflush(stdout);

        uint32_t out_len = 0;
        char    *output  = run_binary(binary, bin_len, &out_len);

        if (send_string(fd, output, out_len) < 0)
            fprintf(stderr, "[node] Failed to send output back to server.\n");

        dec_load();
        printf("[node] Job complete. Output sent (%u bytes). (load now: %u)\n",
               out_len, get_load());
        fflush(stdout);

        free(binary);
        free(output);

    } else {
        fprintf(stderr, "[node] Unknown message type: %u\n", msg_type);
    }

    close(fd);
    return NULL;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int port = DEFAULT_NODE_BASE_PORT;

    /* Allow port as command-line argument */
    if (argc >= 2) {
        port = atoi(argv[1]);
        if (port <= 0) port = DEFAULT_NODE_BASE_PORT;
    } else {
        /* Interactive prompt as fallback */
        char input[32];
        printf("Enter node listen port [%d]: ", DEFAULT_NODE_BASE_PORT);
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin)) {
            input[strcspn(input, "\n")] = '\0';
            if (input[0] != '\0') port = atoi(input);
        }
    }

    /* Create the local listener socket */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listener); return 1;
    }
    if (listen(listener, 64) < 0) {
        perror("listen"); close(listener); return 1;
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║         NODE WORKER — READY               ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    printf("[node] Listening on port %d\n", port);
    printf("[node] Waiting for jobs from server (Machine A)...\n\n");
    fflush(stdout);

    while (1) {
        int *cfd = (int *)malloc(sizeof(int));
        if (!cfd) continue;

        *cfd = accept(listener, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, cfd) != 0) {
            close(*cfd);
            free(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    close(listener);
    return 0;
}
