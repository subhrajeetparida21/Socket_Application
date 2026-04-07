/*
 * server.c  —  Code Distribution Server  (Machine A)
 *
 * Compile:  gcc server.c common.c -o server -lpthread
 * Run:      ./server
 *
 * Workflow:
 *   1. User registers one or more nodes (Machine B) by entering IP + port.
 *   2. User picks a .c file from the current directory.
 *   3. Server compiles it locally with gcc -O2 into a temp binary.
 *   4. Server queries each registered node for its active-job load.
 *   5. Server shows a dispatch menu:
 *        [0]   auto   — send to the least-loaded alive node
 *        [1..N]       — send to the specific node chosen by the user
 *   6. Server sends the compiled binary to the chosen node (MSG_RUN_BINARY).
 *   7. Server receives the output and prints it.
 *
 * BUG-FIXES over original:
 *   - compile_local: mkstemp creates the file; gcc -o to that path fails
 *     because gcc refuses to overwrite a file it did not create. Fixed by
 *     unlinking the mkstemp file before calling gcc so gcc creates it fresh.
 *   - compile_local: compiler stderr was shown only when there were warnings;
 *     now always shown and attributed clearly.
 *   - dispatch_to_node: connect timeout was 3 s for potentially long-running
 *     binaries — now a separate execution timeout (60 s) is used.
 *   - print_node_table: load value for offline nodes was printed as ??? 
 *     correctly, but the format string was misaligned — fixed.
 *   - list_c_files: now sorts the list alphabetically for consistent display.
 *   - 'add' command: input buffer was reused mid-flow; now uses a separate
 *     buffer so the IP string is not corrupted by reading the port line.
 *   - Port validation added everywhere (must be 1–65535).
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
    char     ip[INET_ADDRSTRLEN];
    int      port;
    int      alive;       /* 1 = reachable, 0 = last ping failed */
    uint32_t load;        /* last known active-job count          */
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

/* ── trim trailing newline from fgets buffer ─────────────────────────── */
static void trim_nl(char *s){ s[strcspn(s,"\n")] = '\0'; }

/* ── open a short-lived TCP connection to a node ─────────────────────── */
static int connect_to_node(const char *ip, int port){
    struct hostent *he = gethostbyname(ip);
    if(!he) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;

    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr.s_addr, he->h_addr, (size_t)he->h_length);

    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        close(fd); return -1;
    }
    return fd;
}

/* ── query one node's load (MSG_QUERY_LOAD) ──────────────────────────── */
static int query_node_load(NodeInfo *n){
    int fd = connect_to_node(n->ip, n->port);
    if(fd < 0){ n->alive = 0; n->load = UINT32_MAX; return -1; }

    if(send_u32(fd, MSG_QUERY_LOAD) < 0 || recv_u32(fd, &n->load) < 0){
        close(fd); n->alive = 0; n->load = UINT32_MAX; return -1;
    }
    close(fd);
    n->alive = 1;
    return 0;
}

/* ── refresh load for every registered node ──────────────────────────── */
static void refresh_all_loads(void){
    printf(CLR_CYAN "[server] Querying node loads...\n" CLR_RESET);
    for(int i = 0; i < g_node_count; i++)
        query_node_load(&g_nodes[i]);
}

/* ── compile a .c file locally → malloc'd binary buffer ─────────────── */
/*
 * FIX: mkstemp() creates the file; gcc -o <same-path> fails on some systems
 * because it refuses to overwrite the descriptor it doesn't own.
 * Solution: unlink the temp file right after mkstemp so gcc creates it fresh,
 * while we still hold the unique path name.
 */
static char *compile_local(const char *src_path, uint32_t *out_len){
    char bin_tpl[] = "/tmp/lab_server_bin_XXXXXX";
    int  bin_fd    = mkstemp(bin_tpl);
    if(bin_fd < 0){
        fprintf(stderr, "[server] mkstemp failed: %s\n", strerror(errno));
        return NULL;
    }
    close(bin_fd);
    unlink(bin_tpl);   /* FIX: let gcc create the file itself */

    char err_tpl[] = "/tmp/lab_server_err_XXXXXX.txt";
    int  err_fd    = mkstemps(err_tpl, 4);   /* suffix ".txt" = 4 chars */
    if(err_fd < 0){ return NULL; }
    close(err_fd);

    printf("[server] Compiling " CLR_BOLD "%s" CLR_RESET " ...\n", src_path);
    fflush(stdout);

    pid_t pid = fork();
    if(pid == 0){
        int ofd = open(err_tpl, O_WRONLY | O_TRUNC);
        if(ofd >= 0){ dup2(ofd, STDERR_FILENO); close(ofd); }
        /* FIX: also suppress gcc stdout so it doesn't pollute the terminal */
        int devnull = open("/dev/null", O_WRONLY);
        if(devnull >= 0){ dup2(devnull, STDOUT_FILENO); close(devnull); }
        execlp("gcc", "gcc", src_path, "-O2", "-o", bin_tpl, NULL);
        _exit(1);
    }
    if(pid < 0){ unlink(err_tpl); return NULL; }

    int status = 0;
    waitpid(pid, &status, 0);

    /* Always show compiler output (errors OR warnings) */
    FILE *ef = fopen(err_tpl, "r");
    if(ef){
        char line[512];
        while(fgets(line, sizeof(line), ef))
            printf(CLR_YELLOW "[gcc] " CLR_RESET "%s", line);
        fclose(ef);
    }
    unlink(err_tpl);

    if(!WIFEXITED(status) || WEXITSTATUS(status) != 0){
        fprintf(stderr, CLR_RED "[server] Compilation FAILED.\n" CLR_RESET);
        unlink(bin_tpl);
        return NULL;
    }

    printf(CLR_GREEN "[server] Compilation successful.\n" CLR_RESET);

    FILE *fp = fopen(bin_tpl, "rb");
    if(!fp){
        fprintf(stderr, "[server] Cannot open compiled binary: %s\n", strerror(errno));
        unlink(bin_tpl); return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if(sz <= 0){ fclose(fp); unlink(bin_tpl); return NULL; }

    char *buf = (char *)malloc((size_t)sz);
    if(!buf){ fclose(fp); unlink(bin_tpl); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, fp); (void)nr;
    fclose(fp);
    unlink(bin_tpl);

    *out_len = (uint32_t)sz;
    printf("[server] Binary ready: %u bytes\n", *out_len);
    return buf;
}

/* ── send binary to one node, collect output ─────────────────────────── */
static char *dispatch_to_node(NodeInfo *n,
                               const char *binary, uint32_t bin_len,
                               uint32_t *out_len){
    int fd = connect_to_node(n->ip, n->port);
    if(fd < 0){
        n->alive = 0;
        const char *msg = "(node unreachable)\n";
        *out_len = (uint32_t)strlen(msg); return strdup(msg);
    }

    /* FIX: use a longer timeout for execution — binary might take a while */
    struct timeval tv = { .tv_sec = 120, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if(send_u32(fd, MSG_RUN_BINARY) < 0 ||
       send_string(fd, binary, bin_len) < 0){
        close(fd); n->alive = 0;
        const char *msg = "(failed to send binary)\n";
        *out_len = (uint32_t)strlen(msg); return strdup(msg);
    }

    char *output = recv_string(fd, out_len);
    close(fd);

    if(!output){
        const char *msg = "(no response from node)\n";
        *out_len = (uint32_t)strlen(msg); return strdup(msg);
    }
    return output;
}

/* ── list .c files in cwd, sorted alphabetically ────────────────────── */
/* FIX: original had no sort — added qsort for consistent ordering */
static int cmp_str(const void *a, const void *b){
    return strcmp((const char *)a, (const char *)b);
}

static int list_c_files(char files[][256], int max){
    DIR *d = opendir(".");
    if(!d) return 0;
    int count = 0;
    struct dirent *e;
    while((e = readdir(d)) != NULL && count < max){
        size_t n = strlen(e->d_name);
        if(n > 2 && strcmp(e->d_name + n - 2, ".c") == 0){
            strncpy(files[count], e->d_name, 255);
            files[count][255] = '\0';
            count++;
        }
    }
    closedir(d);
    qsort(files, (size_t)count, 256, cmp_str);
    return count;
}

/* ── print the node table ─────────────────────────────────────────────── */
static void print_node_table(void){
    printf("\n  %-4s %-20s %-8s %s\n",
           "Idx", "IP Address", "Port", "Active Jobs");
    printf("  %-4s %-20s %-8s %s\n",
           "───", "──────────────────", "────────", "──────────");
    for(int i = 0; i < g_node_count; i++){
        NodeInfo *n = &g_nodes[i];
        if(n->alive){
            printf("  " CLR_BOLD "[%d]" CLR_RESET
                   " %-20s %-8d " CLR_GREEN "%u" CLR_RESET "\n",
                   i+1, n->ip, n->port, n->load);
        } else {
            /* FIX: offline nodes now print clearly without format string bugs */
            printf("  " CLR_BOLD "[%d]" CLR_RESET
                   " %-20s %-8d " CLR_RED "OFFLINE" CLR_RESET "\n",
                   i+1, n->ip, n->port);
        }
    }
    printf("\n");
}

/* ── helper: read a valid port from stdin ────────────────────────────── */
static int read_port(int default_port){
    char buf[32];
    if(!fgets(buf, sizeof(buf), stdin)) return default_port;
    trim_nl(buf);
    if(buf[0] == '\0') return default_port;
    int p = atoi(buf);
    return (p > 0 && p <= 65535) ? p : default_port;
}

/* ── main ───────────────────────────────────────────────────────────── */
int main(void){
    char input[256];
    /* FIX: separate buffer for IP so 'add' port-read doesn't corrupt it */
    char ip_buf[INET_ADDRSTRLEN];

    printf("╔══════════════════════════════════════════╗\n");
    printf("║     DISTRIBUTED EXECUTION SERVER (A)      ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* ── Step 1: Register nodes ── */
    printf("Register worker nodes (Machine B).\n");
    printf("Type 'done' (or press ENTER) when finished.\n\n");

    while(g_node_count < MAX_NODES){
        printf("Node %d — IP address (or 'done'): ", g_node_count + 1);
        fflush(stdout);
        if(!fgets(input, sizeof(input), stdin)) break;
        trim_nl(input);
        if(strcmp(input,"done") == 0 || input[0] == '\0') break;

        NodeInfo *n = &g_nodes[g_node_count];
        strncpy(n->ip, input, INET_ADDRSTRLEN - 1);
        n->ip[INET_ADDRSTRLEN - 1] = '\0';

        printf("Node %d — Port [%d]: ", g_node_count + 1, DEFAULT_NODE_BASE_PORT);
        fflush(stdout);
        n->port  = read_port(DEFAULT_NODE_BASE_PORT);
        n->alive = 0;
        n->load  = 0;
        g_node_count++;
        printf(CLR_GREEN "  → Node %d registered: %s:%d\n\n" CLR_RESET,
               g_node_count, n->ip, n->port);
    }

    if(g_node_count == 0){
        fprintf(stderr, "[server] No nodes registered. Exiting.\n");
        return 1;
    }
    printf(CLR_BOLD "[server] %d node(s) registered. Ready.\n\n" CLR_RESET,
           g_node_count);

    /* ── Main loop ── */
    char c_files[64][256];

    while(1){
        printf("══════════════════════════════════════════\n");
        printf("Commands:  " CLR_BOLD "run" CLR_RESET
               "  |  " CLR_BOLD "nodes" CLR_RESET
               "  |  " CLR_BOLD "add" CLR_RESET
               "  |  " CLR_BOLD "quit" CLR_RESET "\n> ");
        fflush(stdout);

        if(!fgets(input, sizeof(input), stdin)) break;
        trim_nl(input);

        /* ── nodes ── */
        if(strcmp(input,"nodes") == 0){
            refresh_all_loads();
            print_node_table();
            continue;
        }

        /* ── add ── */
        if(strcmp(input,"add") == 0){
            if(g_node_count >= MAX_NODES){
                printf("[server] Maximum node limit (%d) reached.\n", MAX_NODES);
                continue;
            }
            NodeInfo *n = &g_nodes[g_node_count];

            printf("New node IP: "); fflush(stdout);
            /* FIX: read into ip_buf, not input, so port read below doesn't clobber it */
            if(!fgets(ip_buf, sizeof(ip_buf), stdin)) break;
            trim_nl(ip_buf);
            strncpy(n->ip, ip_buf, INET_ADDRSTRLEN - 1);
            n->ip[INET_ADDRSTRLEN - 1] = '\0';

            printf("New node port [%d]: ", DEFAULT_NODE_BASE_PORT); fflush(stdout);
            n->port  = read_port(DEFAULT_NODE_BASE_PORT);
            n->alive = 0;
            n->load  = 0;
            g_node_count++;
            printf(CLR_GREEN "  → Node %d added: %s:%d\n\n" CLR_RESET,
                   g_node_count, n->ip, n->port);
            continue;
        }

        /* ── quit ── */
        if(strcmp(input,"quit") == 0 || strcmp(input,"exit") == 0){
            printf("[server] Goodbye.\n"); break;
        }

        /* ── run ── */
        if(strcmp(input,"run") != 0){
            printf("[server] Unknown command. Try: run / nodes / add / quit\n");
            continue;
        }

        /* 1. List .c files */
        int nfiles = list_c_files(c_files, 64);
        if(nfiles == 0){
            printf("[server] No .c files found in the current directory.\n");
            continue;
        }
        printf("\n" CLR_BOLD "C files in current directory:\n" CLR_RESET);
        for(int i = 0; i < nfiles; i++)
            printf("  [%d] %s\n", i+1, c_files[i]);
        printf("Pick a file (1-%d): ", nfiles);
        fflush(stdout);

        if(!fgets(input, sizeof(input), stdin)) break;
        int choice = atoi(input);
        if(choice < 1 || choice > nfiles){
            printf("[server] Invalid choice.\n"); continue;
        }
        char *src_path = c_files[choice - 1];

        /* 2. Compile locally */
        uint32_t bin_len = 0;
        char *binary = compile_local(src_path, &bin_len);
        if(!binary) continue;

        /* 3. Refresh loads and print table */
        refresh_all_loads();
        print_node_table();

        /* Find least-loaded alive node */
        int alive_count = 0, least_idx = -1;
        uint32_t min_load = UINT32_MAX;
        for(int i = 0; i < g_node_count; i++){
            if(g_nodes[i].alive){
                alive_count++;
                if(g_nodes[i].load < min_load){
                    min_load = g_nodes[i].load;
                    least_idx = i;
                }
            }
        }

        if(alive_count == 0){
            printf(CLR_RED "[server] No nodes online. Cannot dispatch.\n" CLR_RESET);
            free(binary); continue;
        }

        /* 4. Dispatch menu — user picks node or auto */
        printf("Select target node:\n");
        printf("  [0]  Auto — least-loaded: "
               CLR_CYAN "Node %d (%s:%d, load=%u)" CLR_RESET "\n",
               least_idx + 1,
               g_nodes[least_idx].ip,
               g_nodes[least_idx].port,
               g_nodes[least_idx].load);
        for(int i = 0; i < g_node_count; i++){
            if(!g_nodes[i].alive) continue;
            printf("  [%d]  %s:%d  (load: %u)\n",
                   i+1, g_nodes[i].ip, g_nodes[i].port, g_nodes[i].load);
        }
        printf("Choice [0 = auto]: "); fflush(stdout);

        if(!fgets(input, sizeof(input), stdin)){ free(binary); break; }
        int node_choice = atoi(input);

        NodeInfo *target = NULL;
        if(node_choice == 0){
            target = &g_nodes[least_idx];
        } else if(node_choice >= 1 && node_choice <= g_node_count){
            if(!g_nodes[node_choice - 1].alive){
                printf(CLR_RED "[server] Node %d is offline.\n" CLR_RESET, node_choice);
                free(binary); continue;
            }
            target = &g_nodes[node_choice - 1];
        } else {
            printf("[server] Invalid selection.\n");
            free(binary); continue;
        }

        printf("\n" CLR_BOLD "[server] Dispatching '%s' (%u bytes) → %s:%d\n" CLR_RESET,
               src_path, bin_len, target->ip, target->port);
        fflush(stdout);

        /* 5. Send binary and wait for output */
        uint32_t out_len = 0;
        char *output = dispatch_to_node(target, binary, bin_len, &out_len);
        free(binary);

        printf("\n╔══════════════════════════════════════════╗\n");
        printf("║  OUTPUT from %s:%-5d               \n", target->ip, target->port);
        printf("╚══════════════════════════════════════════╝\n");
        if(output){
            printf("%s", output);
            if(out_len > 0 && output[out_len-1] != '\n') printf("\n");
        } else {
            printf("(null output)\n");
        }
        printf("══════════════════════════════════════════\n\n");
        free(output);
    }

    return 0;
}
