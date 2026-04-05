#include "common.h"

#include <arpa/inet.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
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

static int connect_to_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;
    struct hostent *server;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    server = gethostbyname(host);
    if (!server) {
        fprintf(stderr, "Cannot resolve host %s\n", host);
        exit(1);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        exit(1);
    }

    return fd;
}

static double current_load(void) {
    double loads[3];

    if (getloadavg(loads, 3) == -1) {
        return 9999.0;
    }

    return loads[0];
}

static char *read_text_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    char *buffer = NULL;
    long size = 0;

    if (!fp) {
        return strdup("Failed to open result file.\n");
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        return strdup("Failed to read result file size.\n");
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return strdup("Memory allocation failed.\n");
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
        return strdup("Failed to create temp source file.\n");
    }

    if (write(source_fd, source_code, strlen(source_code)) < 0) {
        close(source_fd);
        unlink(source_template);
        return strdup("Failed to write source file.\n");
    }
    close(source_fd);

    snprintf(binary_path, sizeof(binary_path), "%s.bin", source_template);

    output_fd = mkstemps(output_template, 4);
    if (output_fd < 0) {
        unlink(source_template);
        return strdup("Failed to create temp output file.\n");
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
        uint32_t compile_len = 0;
        buffer = read_text_file(output_template, &compile_len);
        if (compile_len == 0) {
            free(buffer);
            buffer = strdup("Compilation failed on selected lab node.\n");
            compile_len = (uint32_t)strlen(buffer);
        }
        unlink(source_template);
        unlink(output_template);
        *out_len = compile_len;
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

int main(void) {
    char server_host[128] = {0};
    int port = DEFAULT_NODE_PORT;
    int fd;
    char host_name[64] = {0};
    char input[32];

    prompt_text("Enter server IP or hostname", server_host, sizeof(server_host), "127.0.0.1");
    prompt_text("Enter node port", input, sizeof(input), "9000");
    port = atoi(input);

    gethostname(host_name, sizeof(host_name) - 1);
    fd = connect_to_server(server_host, port);

    if (send_string(fd, host_name, (uint32_t)strlen(host_name)) < 0) {
        close(fd);
        return 1;
    }

    printf("Node connected as %s\n", host_name);

    while (1) {
        uint32_t cmd_len = 0;
        char *command = recv_string(fd, &cmd_len);

        if (!command) {
            break;
        }

        if (strcmp(command, "LOAD") == 0) {
            double load = current_load();
            if (send_double(fd, load) < 0) {
                free(command);
                break;
            }
        } else if (strcmp(command, "RUN") == 0) {
            uint32_t code_len = 0;
            uint32_t out_len = 0;
            char *code = recv_string(fd, &code_len);
            char *output;

            if (!code) {
                free(command);
                break;
            }

            output = run_code(code, &out_len);
            if (send_string(fd, output, out_len) < 0) {
                free(output);
                free(code);
                free(command);
                break;
            }

            free(output);
            free(code);
        } else {
            const char *msg = "Unknown command received by node.\n";
            send_string(fd, msg, (uint32_t)strlen(msg));
        }

        free(command);
    }

    close(fd);
    return 0;
}
