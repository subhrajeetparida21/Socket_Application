/*
 * server.c — Code Distribution Server  (Machine A)
 *
 * Compile:  gcc server.c common.c -o server -lpthread
 * Run:      ./server
 *
 * Workflow:
 *   1. User registers one or more nodes (Machine B) by entering IP + port.
 *   2. User picks a .c file from the current directory.
 *   3. Server compiles it locally with gcc into a temp binary.
 *   4. Server queries each registered node for its active-job load.
 *   5. Server shows a menu:
 *        [auto]   — send to least-loaded node automatically
 *        [1..N]   — send to a specific node chosen by user
 *   6. Server sends the binary to the chosen node (MSG_RUN_BINARY).
 *   7. Server receives the output and prints it.
 *
 * The node (node.c) handles MSG_QUERY_LOAD and MSG_RUN_BINARY.
 */

#define _GNU_SOURCE
#include "common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── node registry ───────────────────────────────────────────────────── */
typedef struct {
    char ip[INET_ADDRSTRLEN];
    int  port;
    int  alive;            /* 1 = reachable, 0 = last ping failed */
    uint32_t load;         /* last known active-job count          */
} NodeInfo;

static NodeInfo g_nodes[MAX_NODES];
static int      g_node_count = 0;

/* ── ANSI colour helpers ─────────────────────────────────────────────── */
#define CLR_RESET  "\033[0m"
#define CLR_BOLD   "\033[1m"
#define CLR_GREEN  "\033[32m"
#define CLR_CYAN   "\033[36m"
#define CLR_YELLOW "\033[33m"
#define CLR_RED    "\033[31m"
#define CLR_BLUE   "\033[34m"

/* ── open a short-lived TCP connection to a node ─────────────────────── */
static int connect_to_node(const char *ip, int port) {
    struct hostent *he = gethostbyname(ip);
    if (!he) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* 3-second connect timeout via SO_RCVTIMEO / SO_SNDTIMEO */
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    memcpy(&addr.sin_addr.s_addr, he->h_addr, he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── query one node's load (MSG_QUERY_LOAD) ──────────────────────────── */
static int query_node_load(NodeInfo *n) {
    int fd = connect_to_node(n->ip, n->port);
    if (fd < 0) {
        n->alive = 0;
        n->load  = UINT32_MAX;   /* treat as infinitely busy */
        return -1;
    }
    if (send_u32(fd, MSG_QUERY_LOAD) < 0 || recv_u32(fd, &n->load) < 0) {
        close(fd);
        n->alive = 0;
        n->load  = UINT32_MAX;
        return -1;
    }
    close(fd);
    n->alive = 1;
    return 0;
}

/* ── refresh load for every registered node ──────────────────────────── */
static void refresh_all_loads(void) {
    printf(CLR_CYAN "[server] Querying node loads...\n" CLR_RESET);
    for (int i = 0; i < g_node_count; i++)
        query_node_load(&g_nodes[i]);
}

/* ── compile a .c file locally, return binary in malloc'd buffer ─────── */
static char *compile_local(const char *src_path, uint32_t *out_len) {
    /* Temp file for the output binary */
    char bin_tpl[] = "./lab_server_bin_XXXXXX";
    int  bin_fd    = mkstemp(bin_tpl);
    if (bin_fd < 0) {
        fprintf(stderr, "[server] mkstemp for binary failed: %s\n", strerror(errno));
        return NULL;
    }
    close(bin_fd);   /* gcc will (re-)create/overwrite it */

    /* Temp file to capture compiler output (errors/warnings) */
    char err_tpl[] = "./lab_server_err_XXXXXX.txt";
    int  err_fd    = mkstemps(err_tpl, 4);
    if (err_fd < 0) {
        unlink(bin_tpl);
        return NULL;
    }
    close(err_fd);

    printf("[server] Compiling " CLR_BOLD "%s" CLR_RESET " locally...\n", src_path);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: redirect stderr → compiler error file */
        int ofd = open(err_tpl, O_WRONLY | O_TRUNC);
        if (ofd >= 0) { dup2(ofd, STDERR_FILENO); close(ofd); }
        execlp("gcc", "gcc", src_path, "-O2", "-o", bin_tpl, NULL);
        _exit(1);
    }
    if (pid < 0) {
        unlink(bin_tpl); unlink(err_tpl);
        fprintf(stderr, "[server] fork() failed.\n");
        return NULL;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    /* Show compiler errors/warnings regardless */
    FILE *ef = fopen(err_tpl, "r");
    if (ef) {
        char line[512];
        int  first = 1;
        while (fgets(line, sizeof(line), ef)) {
            if (first) { printf(CLR_YELLOW "[gcc] " CLR_RESET); first = 0; }
            printf("%s", line);
        }
        fclose(ef);
    }
    unlink(err_tpl);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, CLR_RED "[server] Compilation FAILED.\n" CLR_RESET);
        unlink(bin_tpl);
        return NULL;
    }

    printf(CLR_GREEN "[server] Compilation successful.\n" CLR_RESET);

    /* Read compiled binary into memory */
    FILE *fp = fopen(bin_tpl, "rb");
    if (!fp) {
        fprintf(stderr, "[server] Cannot open compiled binary.\n");
        unlink(bin_tpl);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp); unlink(bin_tpl);
        fprintf(stderr, "[server] Compiled binary is empty.\n");
        return NULL;
    }
    char *buf = (char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); unlink(bin_tpl); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp); (void)nr;
    fclose(fp);
    unlink(bin_tpl);

    *out_len = (uint32_t)sz;
    printf("[server] Binary size: %u bytes\n", *out_len);
    return buf;
}

/* ── send binary to one node, collect output ─────────────────────────── */
static char *dispatch_to_node(NodeInfo *n,
                               const char *binary, uint32_t bin_len,
                               uint32_t *out_len) {
    int fd = connect_to_node(n->ip, n->port);
    if (fd < 0) {
        n->alive = 0;
        const char *msg = "(node unreachable)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }

    /* Extend timeout for execution (binary might run a while) */
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Send message type, then binary payload */
    if (send_u32(fd, MSG_RUN_BINARY) < 0 ||
        send_string(fd, binary, bin_len) < 0) {
        close(fd);
        n->alive = 0;
        const char *msg = "(failed to send binary to node)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }

    /* Receive output */
    char *output = recv_string(fd, out_len);
    close(fd);

    if (!output) {
        const char *msg = "(no response from node)\n";
        *out_len = (uint32_t)strlen(msg);
        return strdup(msg);
    }
    return output;
}

/* ── list .c files in the current directory ───────────────────────────── */
static int list_c_files(char files[][256], int max) {
    DIR *d = opendir(".");
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max) {
        size_t n = strlen(e->d_name);
        if (n > 2 && strcmp(e->d_name + n - 2, ".c") == 0) {
            if (strcmp(e->d_name, "server.c") == 0 ||
                strcmp(e->d_name, "node.c")   == 0 ||
                strcmp(e->d_name, "common.c") == 0 ||
                strncmp(e->d_name, "client", 6) == 0) {
                continue;
            }
            snprintf(files[count], 256, "%s", e->d_name);
            count++;
        }
    }
    closedir(d);
    return count;
}

/* ── print the node table with load info ─────────────────────────────── */
static void print_node_table(void) {
    printf("\n  %-4s %-18s %-8s %-12s\n", "Idx", "IP Address", "Port", "Active Jobs");
    printf("  %-4s %-18s %-8s %-12s\n",
           "───", "──────────────────", "────────", "──────────");
    for (int i = 0; i < g_node_count; i++) {
        NodeInfo *n = &g_nodes[i];
        const char *status_clr = n->alive ? CLR_GREEN : CLR_RED;
        const char *load_str   = n->alive ? "" : " (OFFLINE)";
        printf("  " CLR_BOLD "[%d]" CLR_RESET " %-18s %-8d %s",
               i + 1, n->ip, n->port, status_clr);
        if (n->alive)
            printf("%-12u" CLR_RESET "%s\n", n->load, load_str);
        else
            printf("???%s" CLR_RESET "\n", load_str);
    }
    printf("\n");
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void) {
    char input[256];

    printf("╔══════════════════════════════════════════╗\n");
    printf("║     DISTRIBUTED EXECUTION SERVER (A)      ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* ── Step 1: Register nodes ── */
    printf("Register worker nodes (Machine B).\n");
    printf("Type 'done' when finished, or just press ENTER to skip if already added.\n\n");

    while (g_node_count < MAX_NODES) {
        printf("Node %d — IP address (or 'done'): ", g_node_count + 1);
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';
        if (strcmp(input, "done") == 0 || input[0] == '\0') break;

        NodeInfo *n = &g_nodes[g_node_count];
        snprintf(n->ip, INET_ADDRSTRLEN, "%.*s", INET_ADDRSTRLEN - 1, input);

        printf("Node %d — Port [%d]: ", g_node_count + 1, DEFAULT_NODE_BASE_PORT);
        fflush(stdout);
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';
        n->port  = (input[0] != '\0') ? atoi(input) : DEFAULT_NODE_BASE_PORT;
        n->alive = 0;
        n->load  = 0;
        g_node_count++;
        printf(CLR_GREEN "  → Node %d registered: %s:%d\n" CLR_RESET,
               g_node_count, n->ip, n->port);
    }

    if (g_node_count == 0) {
        fprintf(stderr, "[server] No nodes registered. Exiting.\n");
        return 1;
    }

    printf("\n" CLR_BOLD "[server] %d node(s) registered. Ready.\n\n" CLR_RESET,
           g_node_count);

    /* ── Main loop ── */
    char c_files[64][256];

    while (1) {
        printf("══════════════════════════════════════════\n");
        printf("Commands:\n");
        printf("  " CLR_BOLD "run" CLR_RESET "   — compile a .c file and dispatch to a node\n");
        printf("  " CLR_BOLD "nodes" CLR_RESET " — show registered nodes and their load\n");
        printf("  " CLR_BOLD "add"  CLR_RESET "   — register another node\n");
        printf("  " CLR_BOLD "quit" CLR_RESET "  — exit\n");
        printf("> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = '\0';

        /* ── 'nodes' ── */
        if (strcmp(input, "nodes") == 0) {
            refresh_all_loads();
            print_node_table();
            continue;
        }

        /* ── 'add' ── */
        if (strcmp(input, "add") == 0) {
            if (g_node_count >= MAX_NODES) {
                printf("[server] Maximum node limit reached.\n");
                continue;
            }
            NodeInfo *n = &g_nodes[g_node_count];
            printf("New node IP: "); fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            snprintf(n->ip, INET_ADDRSTRLEN, "%.*s", INET_ADDRSTRLEN - 1, input);

            printf("New node port [%d]: ", DEFAULT_NODE_BASE_PORT); fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\n")] = '\0';
            n->port  = (input[0] != '\0') ? atoi(input) : DEFAULT_NODE_BASE_PORT;
            n->alive = 0;
            n->load  = 0;
            g_node_count++;
            printf(CLR_GREEN "  → Node %d added: %s:%d\n\n" CLR_RESET,
                   g_node_count, n->ip, n->port);
            continue;
        }

        /* ── 'quit' ── */
        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) {
            printf("[server] Goodbye.\n");
            break;
        }

        /* ── 'run' ── */
        if (strcmp(input, "run") != 0) {
            printf("[server] Unknown command. Try: run / nodes / add / quit\n");
            continue;
        }

        /* 1. List .c files */
        int nfiles = list_c_files(c_files, 64);
        if (nfiles == 0) {
            printf("[server] No .c files found in the current directory.\n");
            continue;
        }
        printf("\n" CLR_BOLD "C files in current directory:\n" CLR_RESET);
        for (int i = 0; i < nfiles; i++)
            printf("  [%d] %s\n", i + 1, c_files[i]);
        printf("Pick a file (1-%d): ", nfiles);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        int choice = atoi(input);
        if (choice < 1 || choice > nfiles) {
            printf("[server] Invalid choice.\n");
            continue;
        }
        char *src_path = c_files[choice - 1];

        /* 2. Compile locally */
        uint32_t bin_len = 0;
        char *binary = compile_local(src_path, &bin_len);
        if (!binary) continue;   /* error already printed */

        /* 3. Refresh node loads */
        refresh_all_loads();
        print_node_table();

        /* ── Verify at least one alive node ── */
        int alive_count = 0;
        int least_loaded_idx = -1;
        uint32_t min_load = UINT32_MAX;

        for (int i = 0; i < g_node_count; i++) {
            if (g_nodes[i].alive) {
                alive_count++;
                if (g_nodes[i].load < min_load) {
                    min_load = g_nodes[i].load;
                    least_loaded_idx = i;
                }
            }
        }

        if (alive_count == 0) {
            printf(CLR_RED "[server] No nodes are online. Cannot dispatch.\n" CLR_RESET);
            free(binary);
            continue;
        }

        /* 4. Let user choose target node */
        printf("Select target node:\n");
        printf("  [0]  Auto — send to least-loaded node "
               CLR_CYAN "(Node %d, load=%u)" CLR_RESET "\n",
               least_loaded_idx + 1,
               g_nodes[least_loaded_idx].load);
        for (int i = 0; i < g_node_count; i++) {
            if (!g_nodes[i].alive) continue;
            printf("  [%d]  %s:%d  (load: %u)\n",
                   i + 1, g_nodes[i].ip, g_nodes[i].port, g_nodes[i].load);
        }
        printf("Choice [0 = auto]: ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) { free(binary); break; }
        int node_choice = atoi(input);

        NodeInfo *target = NULL;
        if (node_choice == 0) {
            target = &g_nodes[least_loaded_idx];
        } else if (node_choice >= 1 && node_choice <= g_node_count) {
            target = &g_nodes[node_choice - 1];
            if (!target->alive) {
                printf(CLR_RED "[server] Node %d is offline.\n" CLR_RESET, node_choice);
                free(binary);
                continue;
            }
        } else {
            printf("[server] Invalid selection.\n");
            free(binary);
            continue;
        }

        printf("\n" CLR_BOLD "[server] Dispatching '%s' (%u bytes) to %s:%d...\n"
               CLR_RESET, src_path, bin_len, target->ip, target->port);

        /* 5. Send binary and wait for output */
        uint32_t out_len = 0;
        char *output = dispatch_to_node(target, binary, bin_len, &out_len);

        printf("\n");
        printf("╔══════════════════════════════════════════╗\n");
        printf("║   OUTPUT from %s:%-5d                \n", target->ip, target->port);
        printf("╚══════════════════════════════════════════╝\n");
        printf("%s", output ? output : "(null output)");
        if (out_len > 0 && output[out_len - 1] != '\n') printf("\n");
        printf("══════════════════════════════════════════\n\n");

        free(output);
        free(binary);
    }

    return 0;
}
