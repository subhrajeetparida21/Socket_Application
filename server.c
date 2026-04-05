#include "common.h"

#include <arpa/inet.h>
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

static void prompt_text(const char *label, char *buffer, size_t size, const char *default_value) {
    if (default_value && default_value[0] != '\0') {
        printf("%s [%s]: ", label, default_value);
    } else {
        printf("%s: ", label);
    }

    if (!fgets(buffer, (int)size, stdin)) {
        buffer[0] = '\0';
        return;
    }

    buffer[strcspn(buffer, "\n")] = '\0';
    if (buffer[0] == '\0' && default_value) {
        snprintf(buffer, size, "%s", default_value);
    }
}

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

static char *read_text_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    char *buffer = NULL;
    long size = 0;

    if (!fp) {
        buffer = strdup("Failed to open result file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        buffer = strdup("Failed to read result file size.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        buffer = strdup("Memory allocation failed.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    if (size > 0) {
        fread(buffer, 1, (size_t)size, fp);
    }
    buffer[size] = '\0';
    fclose(fp);

    if (size == 0) {
        free(buffer);
        buffer = strdup("Program finished with no output.\n");
        size = (long)strlen(buffer);
    }

    *out_len = (uint32_t)size;
    return buffer;
}

static char *run_code(const char *source_code, uint32_t *out_len) {
    char source_template[] = "/tmp/lab_code_XXXXXX.c";
    char binary_path[PATH_MAX];
    char output_template[] = "/tmp/lab_out_XXXXXX.txt";
    int source_fd = -1;
    int output_fd = -1;
    pid_t pid;
    int status;
    char *buffer = NULL;

    source_fd = mkstemps(source_template, 2);
    if (source_fd < 0) {
        buffer = strdup("Failed to create temp source file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }

    if (write(source_fd, source_code, strlen(source_code)) < 0) {
        close(source_fd);
        unlink(source_template);
        buffer = strdup("Failed to write source file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }
    close(source_fd);

    snprintf(binary_path, sizeof(binary_path), "%s.bin", source_template);

    output_fd = mkstemps(output_template, 4);
    if (output_fd < 0) {
        unlink(source_template);
        buffer = strdup("Failed to create temp output file.\n");
        *out_len = (uint32_t)strlen(buffer);
        return buffer;
    }
    close(output_fd);

    pid = fork();
    if (pid == 0) {
        freopen(output_template, "w", stdout);
        freopen(output_template, "a", stderr);
        execlp("gcc", "gcc", source_template, "-O2", "-o", binary_path, NULL);
        _exit(1);
    }
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        buffer = read_text_file(output_template, out_len);
        unlink(source_template);
        unlink(output_template);
        return buffer;
    }

    pid = fork();
    if (pid == 0) {
        freopen(output_template, "w", stdout);
        freopen(output_template, "a", stderr);
        execl(binary_path, binary_path, NULL);
        _exit(1);
    }
    waitpid(pid, &status, 0);

    buffer = read_text_file(output_template, out_len);

    unlink(source_template);
    unlink(binary_path);
    unlink(output_template);
    return buffer;
}

static void *client_worker(void *arg) {
    int client_fd = *(int *)arg;
    uint32_t code_len = 0;
    uint32_t output_len = 0;
    char *source = NULL;
    char *output = NULL;

    free(arg);

    source = recv_string(client_fd, &code_len);
    if (!source) {
        close(client_fd);
        return NULL;
    }

    output = run_code(source, &output_len);
    send_string(client_fd, output, output_len);

    free(source);
    free(output);
    close(client_fd);
    return NULL;
}

int main(void) {
    int port = DEFAULT_CLIENT_PORT;
    int listener;
    char input[32];

    prompt_text("Enter server port", input, sizeof(input), "9001");
    port = atoi(input);

    listener = create_listener(port);

    printf("Server ready on port %d\n", port);

    while (1) {
        int *client_fd = (int *)malloc(sizeof(int));
        pthread_t tid;

        if (!client_fd) {
            continue;
        }

        *client_fd = accept(listener, NULL, NULL);
        if (*client_fd < 0) {
            free(client_fd);
            continue;
        }

        pthread_create(&tid, NULL, client_worker, client_fd);
        pthread_detach(tid);
    }

    return 0;
}
