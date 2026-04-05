#include "common.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static char *read_file(const char *path, uint32_t *out_len) {
    FILE *fp = fopen(path, "rb");
    char *buffer;
    long size;

    if (!fp) {
        perror("fopen");
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        return NULL;
    }

    buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    if (size > 0) {
        fread(buffer, 1, (size_t)size, fp);
    }
    buffer[size] = '\0';
    fclose(fp);

    *out_len = (uint32_t)size;
    return buffer;
}

int main(void) {
    char code_path[256] = {0};
    char server_host[128] = {0};
    int port = DEFAULT_CLIENT_PORT;
    uint32_t code_len = 0;
    uint32_t response_len = 0;
    char *code = NULL;
    char *response = NULL;
    int fd;
    char input[32];

    prompt_text("Enter C source file path", code_path, sizeof(code_path), NULL);
    if (code_path[0] == '\0') {
        fprintf(stderr, "Source file path is required.\n");
        return 1;
    }

    prompt_text("Enter server IP or hostname", server_host, sizeof(server_host), "127.0.0.1");
    prompt_text("Enter server port", input, sizeof(input), "9001");
    port = atoi(input);

    code = read_file(code_path, &code_len);
    if (!code) {
        fprintf(stderr, "Failed to read code file.\n");
        return 1;
    }

    fd = connect_to_server(server_host, port);

    if (send_string(fd, code, code_len) < 0) {
        fprintf(stderr, "Failed to send code.\n");
        free(code);
        close(fd);
        return 1;
    }

    response = recv_string(fd, &response_len);
    if (!response) {
        fprintf(stderr, "Failed to receive output.\n");
        free(code);
        close(fd);
        return 1;
    }

    printf("Output from lab system:\n%s", response);

    free(response);
    free(code);
    close(fd);
    return 0;
}
