#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#define PORT 8080

int main() {
    int sock = 0;
    struct sockaddr_in serv_addr;

    system("gcc program.c -o program");

    sock = socket(AF_INET, SOCK_STREAM, 0);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    inet_pton(AF_INET, "192.168.1.2", &serv_addr.sin_addr);

    connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    FILE *fp = fopen("program", "rb");

    fseek(fp, 0, SEEK_END);
    int file_size = ftell(fp);
    rewind(fp);

    write(sock, &file_size, sizeof(file_size));

    char buffer[1024];

    while (!feof(fp)) {
        int bytes = fread(buffer, 1, sizeof(buffer), fp);
        write(sock, buffer, bytes);
    }

    fclose(fp);

    FILE *out = fopen("output.txt", "wb");

    int out_size;
    read(sock, &out_size, sizeof(out_size));

    int received = 0;

    while (received < out_size) {
        int bytes = read(sock, buffer, sizeof(buffer));
        fwrite(buffer, 1, bytes, out);
        received += bytes;
    }

    fclose(out);

    system("cat output.txt");

    close(sock);

    return 0;
}