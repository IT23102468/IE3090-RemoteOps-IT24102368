/* agent_368.c - RemoteOps Agent for IT24102368
 * Port: 9410 | SID: 8632 | Token: OPS-2368
 * Storage: ./agentfiles/IT24102368/
 * Log: remoteops_IT24102368.log
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/sysinfo.h>

#define PORT            9410
#define SID             "8632"
#define AUTH_TOKEN      "OPS-2368"
#define LOG_FILE        "remoteops_IT24102368.log"
#define STORAGE_DIR     "./agentfiles/IT24102368"
#define MAX_CLIENTS     32
#define BUF_SIZE        8192
#define MAX_LINE        4096
#define MAX_FILE_SIZE   (50 * 1024 * 1024)  /* 50 MB limit */

/* Per-client session */
typedef struct {
    int             tcp_fd;
    struct sockaddr_in addr;
    int             authenticated;
    int             monitor_active;
    int             monitor_udp_port;
    char            client_ip[INET_ADDRSTRLEN];
    pthread_t       thread;
    int             active;
} client_t;

static client_t clients[MAX_CLIENTS];
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;

/* ---------- Logging ---------- */
void log_msg(const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    FILE *fp = fopen(LOG_FILE, "a");
    if (fp) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char ts[64];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
        fprintf(fp, "[%s] ", ts);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        va_end(ap);
        fprintf(fp, "\n");
        fclose(fp);
    }
    pthread_mutex_unlock(&log_mutex);
}

/* ---------- Helpers ---------- */
void send_line(int fd, const char *msg) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s SID:%s\n", msg, SID);
    send(fd, buf, strlen(buf), 0);
}

void send_err(int fd, const char *code, const char *reason) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "ERR %s %s SID:%s\n", code, reason, SID);
    send(fd, buf, strlen(buf), 0);
}

/* Read exactly one line (handles partial recv) */
int recv_line(int fd, char *buf, size_t maxlen) {
    size_t pos = 0;
    while (pos < maxlen - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            buf[pos] = '\0';
            return (int)pos;
        }
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

/* Read exactly nbytes */
int recv_exact(int fd, void *buf, size_t nbytes) {
    size_t got = 0;
    char *p = buf;
    while (got < nbytes) {
        ssize_t n = recv(fd, p + got, nbytes - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* Send exactly nbytes */
int send_exact(int fd, const void *buf, size_t nbytes) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < nbytes) {
        ssize_t n = send(fd, p + sent, nbytes - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ---------- System info helpers ---------- */
void get_sysinfo(double *cpu, long *mem_mb, long *uptime) {
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        *uptime = si.uptime;
        *mem_mb = (si.totalram - si.freeram) / (1024 * 1024);
        /* Simple load average as CPU indicator */
        *cpu = si.loads[0] / 65536.0;   /* 1-min load */
    } else {
        *cpu = 0.5;
        *mem_mb = 1024;
        *uptime = 3600;
    }
}

/* ---------- Command handlers ---------- */
void handle_sysinfo(int fd) {
    double cpu;
    long mem, up;
    get_sysinfo(&cpu, &mem, &up);
    char resp[256];
    snprintf(resp, sizeof(resp), "OK SYSINFO %.2f %ld %ld", cpu, mem, up);
    send_line(fd, resp);
}

void handle_listproc(int fd) {
    FILE *fp = popen("ps -eo pid,comm --no-headers 2>/dev/null | head -n 40", "r");
    if (!fp) {
        send_err(fd, "003", "LISTPROC_FAILED");
        return;
    }
    char line[256];
    char out[4096] = "OK PROCS ";
    int first = 1;
    while (fgets(line, sizeof(line), fp)) {
        /* trim */
        char *p = line;
        while (*p && isspace(*p)) p++;
        char *end = p + strlen(p) - 1;
        while (end > p && isspace(*end)) *end-- = '\0';
        if (!first) strcat(out, ",");
        /* replace spaces with / for PID/name */
        char *sp = strchr(p, ' ');
        if (sp) *sp = '/';
        strncat(out, p, sizeof(out) - strlen(out) - 1);
        first = 0;
    }
    pclose(fp);
    send_line(fd, out);
}

void handle_exec(int fd, const char *name) {
    const char *cmd = NULL;
    if (strcmp(name, "DATE") == 0)          cmd = "date";
    else if (strcmp(name, "UPTIME") == 0)   cmd = "uptime -p";
    else if (strcmp(name, "DISKFREE") == 0) cmd = "df -h / | tail -1";
    else if (strcmp(name, "HOSTNAME") == 0) cmd = "hostname";
    else if (strcmp(name, "WHOAMI") == 0)   cmd = "whoami";
    else {
        send_err(fd, "002", "COMMAND_NOT_ALLOWED");
        return;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        send_err(fd, "003", "EXEC_FAILED");
        return;
    }
    char output[1024] = {0};
    size_t n = fread(output, 1, sizeof(output) - 1, fp);
    pclose(fp);
    /* strip trailing newline */
    while (n > 0 && (output[n-1] == '\n' || output[n-1] == '\r')) output[--n] = '\0';
    char resp[1200];
    snprintf(resp, sizeof(resp), "OK EXEC_RESULT %s", output);
    send_line(fd, resp);
}

void handle_put(int fd, const char *filename, long filesize) {
    if (filesize < 0 || filesize > MAX_FILE_SIZE) {
        send_err(fd, "004", "FILE_TOO_LARGE");
        /* drain the data if any */
       char drain[4096];
        long left = filesize;
        while (left > 0) {
            size_t chunk = (left > (long)sizeof(drain)) ? sizeof(drain) : (size_t)left;
            if (recv_exact(fd, drain, chunk) < 0) break;
            left -= (long)chunk;
        }
        return;
    }

    /* ensure storage directory exists */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, filename);
    mkdir("./agentfiles", 0755);
    mkdir(STORAGE_DIR, 0755);

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        send_err(fd, "003", "WRITE_FAILED");
        return;
    }

    char buf[BUF_SIZE];
    long remaining = filesize;
    while (remaining > 0) {
        size_t chunk = remaining > BUF_SIZE ? BUF_SIZE : remaining;
        if (recv_exact(fd, buf, chunk) < 0) {
            fclose(fp);
            unlink(path);
            return;
        }
        fwrite(buf, 1, chunk, fp);
        remaining -= chunk;
    }
    fclose(fp);

    char resp[256];
    snprintf(resp, sizeof(resp), "OK FILE_RECEIVED %s", filename);
    send_line(fd, resp);
    log_msg("PUT %s (%ld bytes) from %s", filename, filesize, "client");
}

void handle_get(int fd, const char *filename) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send_err(fd, "005", "FILE_NOT_FOUND");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char header[256];
    snprintf(header, sizeof(header), "OK FILE_SEND %s %ld SID:%s\n", filename, filesize, SID);
    send(fd, header, strlen(header), 0);

    char buf[BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send_exact(fd, buf, n) < 0) break;
    }
    fclose(fp);
    log_msg("GET %s (%ld bytes)", filename, filesize);
}

/* ---------- UDP monitoring thread ---------- */
void *monitor_thread(void *arg) {
    client_t *cli = (client_t *)arg;
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) return NULL;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(cli->monitor_udp_port);
    inet_pton(AF_INET, cli->client_ip, &dest.sin_addr);

    while (cli->monitor_active && running) {
        double cpu;
        long mem, up;
        get_sysinfo(&cpu, &mem, &up);
        char msg[256];
        snprintf(msg, sizeof(msg), "SYSINFO %.2f %ld %ld SID:%s\n", cpu, mem, up, SID);
        sendto(udp, msg, strlen(msg), 0, (struct sockaddr *)&dest, sizeof(dest));
        sleep(3);   /* interval: 3 seconds */
    }
    close(udp);
    return NULL;
}

/* ---------- Per-client thread ---------- */
void *client_handler(void *arg) {
    client_t *cli = (client_t *)arg;
    int fd = cli->tcp_fd;
    char line[MAX_LINE];

    log_msg("New connection from %s:%d", cli->client_ip, ntohs(cli->addr.sin_port));

    while (running) {
        int n = recv_line(fd, line, sizeof(line));
        if (n < 0) break;   /* client disconnected */

        log_msg("CMD from %s: %s", cli->client_ip, line);

        /* AUTH must be first */
        if (!cli->authenticated) {
            if (strncmp(line, "AUTH ", 5) == 0) {
                char *token = line + 5;
                if (strcmp(token, AUTH_TOKEN) == 0) {
                    cli->authenticated = 1;
                    send_line(fd, "OK AUTHENTICATED");
                    log_msg("AUTH success for %s", cli->client_ip);
                } else {
                    send_err(fd, "001", "AUTH_FAILED");
                    log_msg("AUTH failed for %s", cli->client_ip);
                }
            } else {
                send_err(fd, "001", "AUTH_REQUIRED");
            }
            continue;
        }

        /* Already authenticated */
        if (strcmp(line, "SYSINFO") == 0) {
            handle_sysinfo(fd);
        }
        else if (strcmp(line, "LISTPROC") == 0) {
            handle_listproc(fd);
        }
        else if (strncmp(line, "EXEC ", 5) == 0) {
            handle_exec(fd, line + 5);
        }
        else if (strncmp(line, "PUT ", 4) == 0) {
            char filename[256];
            long filesize;
            if (sscanf(line + 4, "%255s %ld", filename, &filesize) == 2) {
                handle_put(fd, filename, filesize);
            } else {
                send_err(fd, "003", "BAD_PUT_FORMAT");
            }
        }
        else if (strncmp(line, "GET ", 4) == 0) {
            handle_get(fd, line + 4);
        }
        else if (strncmp(line, "MONITOR START ", 14) == 0) {
            int port = atoi(line + 14);
            if (port > 0 && port < 65536) {
                cli->monitor_udp_port = port;
                cli->monitor_active = 1;
                pthread_t mt;
                pthread_create(&mt, NULL, monitor_thread, cli);
                pthread_detach(mt);
                send_line(fd, "OK MONITOR_STARTED");
                log_msg("MONITOR START port %d for %s", port, cli->client_ip);
            } else {
                send_err(fd, "003", "BAD_UDP_PORT");
            }
        }
        else if (strcmp(line, "MONITOR STOP") == 0) {
            cli->monitor_active = 0;
            send_line(fd, "OK MONITOR_STOPPED");
            log_msg("MONITOR STOP for %s", cli->client_ip);
        }
        else if (strcmp(line, "QUIT") == 0) {
            cli->monitor_active = 0;
            send_line(fd, "OK BYE");
            break;
        }
        else {
            send_err(fd, "003", "UNKNOWN_COMMAND");
        }
    }

    cli->monitor_active = 0;
    close(fd);
    log_msg("Connection closed from %s", cli->client_ip);

    pthread_mutex_lock(&clients_mutex);
    cli->active = 0;
    pthread_mutex_unlock(&clients_mutex);
    return NULL;
}

/* ---------- Main ---------- */
void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* create storage dir */
    mkdir("./agentfiles", 0755);
    mkdir(STORAGE_DIR, 0755);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen");
        exit(1);
    }

    printf("RemoteOps Agent started on port %d (SID:%s)\n", PORT, SID);
    log_msg("Agent started on port %d", PORT);

    while (running) {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* find free slot */
        pthread_mutex_lock(&clients_mutex);
        int idx = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active) {
                idx = i;
                break;
            }
        }
        if (idx == -1) {
            pthread_mutex_unlock(&clients_mutex);
            close(client_fd);
            continue;
        }

        clients[idx].tcp_fd = client_fd;
        clients[idx].addr = cli_addr;
        clients[idx].authenticated = 0;
        clients[idx].monitor_active = 0;
        clients[idx].active = 1;
        inet_ntop(AF_INET, &cli_addr.sin_addr, clients[idx].client_ip, INET_ADDRSTRLEN);
        pthread_mutex_unlock(&clients_mutex);

        pthread_create(&clients[idx].thread, NULL, client_handler, &clients[idx]);
        pthread_detach(clients[idx].thread);
    }

    close(listen_fd);
    log_msg("Agent shutting down NOW");
    return 0;
}
