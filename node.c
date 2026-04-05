/*
 * node.c  –  Lab-machine worker
 *
 * Listens for connections from the load-balancer.
 * Each connection carries a 1-byte message type:
 *   MSG_QUERY_LOAD  → reply with uint32 active-job count
 *   MSG_RUN_CODE    → recv source string, compile & run, reply output string
 */

#include "common.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── load tracking ─────────────────────────────────────────────────── */
static pthread_mutex_t g_load_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        g_active_jobs = 0;

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

/* ── helpers ────────────────────────────────────────────────────────── */
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
    if (listen(fd, 64) < 0) {
        perror("listen"); close(fd); exit(1);
    }
    return fd;
}

static char *read_text_file(const char *path, uint32_t *out_len) {
    FILE *fp     = fopen(path, "rb");
    char *buffer = NULL;
    long  size   = 0;

    if (!fp) {
        buffer   = strdup("Failed to open result file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        buffer   = strdup("Failed to read result file size.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        buffer   = strdup("Memory allocation failed.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    if (size > 0) { size_t nr = fread(buffer, 1, (size_t)size, fp); (void)nr; }
    buffer[size] = '\0';
    fclose(fp);

    if (size == 0) {
        free(buffer);
        buffer = strdup("Program finished with no output.\n");
        size   = (long)strlen(buffer);
    }

    *out_len = (uint32_t)size;
    return buffer;
}

/* Compile source_code in a temp file, run it, return stdout+stderr */
static char *run_code(const char *source_code, uint32_t *out_len) {
    char  source_tpl[] = "/tmp/lab_node_XXXXXX.c";
    char  output_tpl[] = "/tmp/lab_out_XXXXXX.txt";
    char  binary_path[PATH_MAX];
    int   source_fd, output_fd;
    pid_t pid;
    int   status;
    char *buffer;

    source_fd = mkstemps(source_tpl, 2);   /* suffix ".c" → len 2 */
    if (source_fd < 0) {
        buffer   = strdup("Failed to create temp source file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }
    if (write(source_fd, source_code, strlen(source_code)) < 0) {
        close(source_fd); unlink(source_tpl);
        buffer   = strdup("Failed to write source file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }
    close(source_fd);

    snprintf(binary_path, sizeof(binary_path), "%s.bin", source_tpl);

    output_fd = mkstemps(output_tpl, 4);   /* suffix ".txt" → len 4 */
    if (output_fd < 0) {
        unlink(source_tpl);
        buffer   = strdup("Failed to create temp output file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }
    close(output_fd);

    /* ── compile ── */
    pid = fork();
    if (pid == 0) {
        int ofd = open(output_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDOUT_FILENO); dup2(ofd, STDERR_FILENO); close(ofd); }
        execlp("gcc", "gcc", source_tpl, "-O2", "-o", binary_path, NULL);
        _exit(1);
    }
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* Compilation error – return compiler output */
        buffer = read_text_file(output_tpl, out_len);
        unlink(source_tpl);
        unlink(output_tpl);
        return buffer;
    }

    /* ── run ── */
    pid = fork();
    if (pid == 0) {
        int ofd = open(output_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDOUT_FILENO); dup2(ofd, STDERR_FILENO); close(ofd); }
        execl(binary_path, binary_path, NULL);
        _exit(1);
    }
    waitpid(pid, &status, 0);

    buffer = read_text_file(output_tpl, out_len);

    unlink(source_tpl);
    unlink(binary_path);
    unlink(output_tpl);
    return buffer;
}

/* ── per-connection thread ─────────────────────────────────────────── */
static void *handle_connection(void *arg) {
    int      fd  = *(int *)arg;
    uint32_t msg = 0;
    free(arg);

    if (recv_u32(fd, &msg) < 0) { close(fd); return NULL; }

    if (msg == MSG_QUERY_LOAD) {
        /* Reply with current active-job count */
        send_u32(fd, get_jobs());

    } else if (msg == MSG_RUN_CODE) {
        uint32_t code_len  = 0;
        uint32_t out_len   = 0;
        char    *source    = NULL;
        char    *output    = NULL;

        source = recv_string(fd, &code_len);
        if (!source) { close(fd); return NULL; }

        inc_jobs();
        printf("[node] Running job (%u active)\n", get_jobs());
        fflush(stdout);

        output = run_code(source, &out_len);
        send_string(fd, output, out_len);

        dec_jobs();
        printf("[node] Job done  (%u active)\n", get_jobs());
        fflush(stdout);

        free(source);
        free(output);
    } else {
        fprintf(stderr, "[node] Unknown message type %u\n", msg);
    }

    close(fd);
    return NULL;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
    int  port;
    char input[32];
    int  listener;

    printf("Enter node port [%d]: ", DEFAULT_NODE_BASE_PORT);
    fflush(stdout);
    if (!fgets(input, sizeof(input), stdin)) { port = DEFAULT_NODE_BASE_PORT; }
    else {
        input[strcspn(input, "\n")] = '\0';
        port = (input[0] != '\0') ? atoi(input) : DEFAULT_NODE_BASE_PORT;
    }

    listener = create_listener(port);
    printf("[node] Listening on port %d\n", port);
    fflush(stdout);

    while (1) {
        int       *cfd = (int *)malloc(sizeof(int));
        pthread_t  tid;
        if (!cfd) continue;

        *cfd = accept(listener, NULL, NULL);
        if (*cfd < 0) { free(cfd); continue; }

        pthread_create(&tid, NULL, handle_connection, cfd);
        pthread_detach(tid);
    }

    return 0;
}
