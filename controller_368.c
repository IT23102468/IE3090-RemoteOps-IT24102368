/* controller_368.c - RemoteOps Controller for IT24102368
 * Connects to Agent on port 9410
 * Auth token: OPS-2368
 * SID expected: 8632
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>

#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    9410
#define AUTH_TOKEN      "OPS-2368"
#define SID_TAG         "SID:8632"
#define BUF_SIZE        8192
#define MAX_LINE        4096

static int tcp_fd = -1;
static volatile int monitor_running = 0;
static int monitor_udp_sock = -1;

/* ---------- Helpers ---------- */
int send_line(int fd, const char *msg) {
    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    return send(fd, buf, strlen(buf), 0);
}

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

/* ---------- UDP monitor receiver thread ---------- */
void *udp_monitor_thread(void *arg) {
    (void)arg;
    char buf[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    printf("[MONITOR] Listening for UDP stats...\n");
    while (monitor_running) {
        ssize_t n = recvfrom(monitor_udp_sock, buf, sizeof(buf)-1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';
            printf("[UDP] %s", buf);
            if (buf[n-1] != '\n') printf("\n");
            fflush(stdout);
        }
    }
    return NULL;
}

/* ---------- Commands ---------- */
void do_auth(void) {
    char cmd[128], resp[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "AUTH %s", AUTH_TOKEN);
    send_line(tcp_fd, cmd);
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) {
        printf("Connection lost\n");
        return;
    }
    printf("← %s\n", resp);
}

void do_sysinfo(void) {
    char resp[MAX_LINE];
    send_line(tcp_fd, "SYSINFO");
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);
}

void do_listproc(void) {
    char resp[MAX_LINE];
    send_line(tcp_fd, "LISTPROC");
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);
}

void do_exec(const char *name) {
    char cmd[128], resp[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "EXEC %s", name);
    send_line(tcp_fd, cmd);
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);
}

void do_put(const char *localfile, const char *remotename) {
    FILE *fp = fopen(localfile, "rb");
    if (!fp) {
        printf("Cannot open local file: %s\n", localfile);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char header[512];
    snprintf(header, sizeof(header), "PUT %s %ld", remotename, filesize);
    send_line(tcp_fd, header);

    char buf[BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send_exact(tcp_fd, buf, n) < 0) {
            printf("Send error\n");
            fclose(fp);
            return;
        }
    }
    fclose(fp);

    char resp[MAX_LINE];
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);
}

void do_get(const char *remotename, const char *localfile) {
    char cmd[256], resp[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "GET %s", remotename);
    send_line(tcp_fd, cmd);

    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);

    /* Parse OK FILE_SEND <name> <size> SID:xxxx */
    if (strncmp(resp, "OK FILE_SEND ", 13) != 0) {
        return; /* error already printed */
    }

    char fname[256];
    long filesize = 0;
    if (sscanf(resp + 13, "%255s %ld", fname, &filesize) != 2) {
        printf("Bad FILE_SEND response\n");
        return;
    }

    FILE *fp = fopen(localfile, "wb");
    if (!fp) {
        printf("Cannot create local file: %s\n", localfile);
        /* still drain the data */
        char drain[BUF_SIZE];
        long left = filesize;
        while (left > 0) {
            size_t chunk = (left > (long)sizeof(drain)) ? sizeof(drain) : (size_t)left;
            if (recv_exact(tcp_fd, drain, chunk) < 0) break;
            left -= (long)chunk;
        }
        return;
    }

    char buf[BUF_SIZE];
    long remaining = filesize;
    while (remaining > 0) {
        size_t chunk = (remaining > (long)BUF_SIZE) ? BUF_SIZE : (size_t)remaining;
        if (recv_exact(tcp_fd, buf, chunk) < 0) {
            printf("Receive error\n");
            break;
        }
        fwrite(buf, 1, chunk, fp);
        remaining -= (long)chunk;
    }
    fclose(fp);
    printf("File saved as %s (%ld bytes)\n", localfile, filesize);
}

void do_monitor_start(int udp_port) {
    /* create UDP socket bound to the port the Agent will send to */
    monitor_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (monitor_udp_sock < 0) {
        perror("udp socket");
        return;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(udp_port);
    if (bind(monitor_udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("udp bind");
        close(monitor_udp_sock);
        monitor_udp_sock = -1;
        return;
    }

    monitor_running = 1;
    pthread_t tid;
    pthread_create(&tid, NULL, udp_monitor_thread, NULL);
    pthread_detach(tid);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "MONITOR START %d", udp_port);
    send_line(tcp_fd, cmd);

    char resp[MAX_LINE];
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);
}

void do_monitor_stop(void) {
    send_line(tcp_fd, "MONITOR STOP");
    char resp[MAX_LINE];
    if (recv_line(tcp_fd, resp, sizeof(resp)) < 0) return;
    printf("← %s\n", resp);

    monitor_running = 0;
    if (monitor_udp_sock >= 0) {
        close(monitor_udp_sock);
        monitor_udp_sock = -1;
    }
}

void do_quit(void) {
    send_line(tcp_fd, "QUIT");
    char resp[MAX_LINE];
    if (recv_line(tcp_fd, resp, sizeof(resp)) >= 0)
        printf("← %s\n", resp);
    monitor_running = 0;
}

/* ---------- Interactive menu ---------- */
void print_menu(void) {
    printf("\n===== RemoteOps Controller (IT24102368) =====\n");
    printf("1. AUTH\n");
    printf("2. SYSINFO\n");
    printf("3. LISTPROC\n");
    printf("4. EXEC <DATE|UPTIME|DISKFREE|HOSTNAME|WHOAMI>\n");
    printf("5. PUT <localfile> <remotename>\n");
    printf("6. GET <remotename> <localfile>\n");
    printf("7. MONITOR START <udp_port>\n");
    printf("8. MONITOR STOP\n");
    printf("9. QUIT\n");
    printf("0. Exit program\n");
    printf("Choice: ");
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    signal(SIGPIPE, SIG_IGN);

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        return 1;
    }

    printf("Connecting to %s:%d ...\n", host, port);
    if (connect(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    printf("Connected.\n");

    char input[512];
    while (1) {
        print_menu();
        if (!fgets(input, sizeof(input), stdin)) break;

        /* trim newline */
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "0") == 0 || strcasecmp(input, "exit") == 0) {
            break;
        }
        else if (strcmp(input, "1") == 0 || strcasecmp(input, "AUTH") == 0) {
            do_auth();
        }
        else if (strcmp(input, "2") == 0 || strcasecmp(input, "SYSINFO") == 0) {
            do_sysinfo();
        }
        else if (strcmp(input, "3") == 0 || strcasecmp(input, "LISTPROC") == 0) {
            do_listproc();
        }
        else if (strncmp(input, "4 ", 2) == 0 || strncasecmp(input, "EXEC ", 5) == 0) {
            char *name = (input[0] == '4') ? input + 2 : input + 5;
            while (*name == ' ') name++;
            do_exec(name);
        }
        else if (strncmp(input, "5 ", 2) == 0 || strncasecmp(input, "PUT ", 4) == 0) {
            char local[256], remote[256];
            if (sscanf(input + (input[0]=='5' ? 2 : 4), "%255s %255s", local, remote) == 2)
                do_put(local, remote);
            else
                printf("Usage: 5 <localfile> <remotename>\n");
        }
        else if (strncmp(input, "6 ", 2) == 0 || strncasecmp(input, "GET ", 4) == 0) {
            char remote[256], local[256];
            if (sscanf(input + (input[0]=='6' ? 2 : 4), "%255s %255s", remote, local) == 2)
                do_get(remote, local);
            else
                printf("Usage: 6 <remotename> <localfile>\n");
        }
        else if (strncmp(input, "7 ", 2) == 0 || strncasecmp(input, "MONITOR START ", 14) == 0) {
            int udpport = 0;
            if (sscanf(input + (input[0]=='7' ? 2 : 14), "%d", &udpport) == 1)
                do_monitor_start(udpport);
            else
                printf("Usage: 7 <udp_port>\n");
        }
        else if (strcmp(input, "8") == 0 || strcasecmp(input, "MONITOR STOP") == 0) {
            do_monitor_stop();
        }
        else if (strcmp(input, "9") == 0 || strcasecmp(input, "QUIT") == 0) {
            do_quit();
            break;
        }
        else {
            printf("Unknown choice\n");
        }
    }

    monitor_running = 0;
    if (tcp_fd >= 0) close(tcp_fd);
    if (monitor_udp_sock >= 0) close(monitor_udp_sock);
    printf("Controller exited.\n");
    return 0;
}
