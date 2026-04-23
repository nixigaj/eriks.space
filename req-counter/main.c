#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>

#define SUCCESS 0
#define ERR 1
#define ERR_PRINT 2

#define DEFAULT_PORT 8080
#define DEFAULT_VISITORS_PATH "visitors.bin"
#define MAX_IP_LEN 46 // INET6_ADDRSTRLEN
#define HTTP_BUF_SIZE 4096
#define HTTP_TIMEOUT_SEC 5
#define EXPIRATION_SEC 86400 // 24 Hours

#define USAGE_STR \
    "Usage: %s -p <listening-port> -v <visitors-path>\n" \
    "Use '-h' flag for usage\n"

// --- Structs ---

typedef struct {
    uint16_t port;
    char visitors_path[4096];
} cfg_t;

typedef struct visitor_node {
    char ip[MAX_IP_LEN];
    time_t timestamp;
    struct visitor_node *next;
} visitor_node_t;

typedef struct {
    uint64_t total_visits;
    visitor_node_t *head;
    visitor_node_t *tail;
    pthread_mutex_t lock;
    atomic_int active_clients; // NEW: Track active threads for safe shutdown
} app_state_t;

typedef struct {
    app_state_t *state;
    cfg_t *cfg;
} hk_args_t;

typedef struct {
    int fd;
    app_state_t *state;
} client_args_t;

// POSIX signal handlers cannot take arguments, so these remain static atomics
static atomic_bool shutdown_requested = false;
static atomic_bool force_exit = false;

// --- Function Prototypes ---
static int handle_args(int argc, char **argv, cfg_t *cfg);
static void handle_signal(int sig);
static void ignore_sigpipe(void);
static int init_state(app_state_t *state);
static int load_state(app_state_t *state, const cfg_t *cfg);
static int save_state(app_state_t *state, const cfg_t *cfg);
static void free_state(app_state_t *state);
static uint64_t process_visit(app_state_t *state, const char *ip);
static uint64_t get_total_visits(app_state_t *state);
static void *housekeeping_thread_func(void *arg);
static void *client_thread_func(void *arg);
static int setup_server_socket(uint16_t port);
static void send_http_response(int client_fd, uint64_t visits);

// --- Main Execution ---

int main(const int argc, char **argv) {
    cfg_t cfg;
    cfg.port = DEFAULT_PORT;
    memset(cfg.visitors_path, 0, sizeof(cfg.visitors_path));

    if (handle_args(argc, argv, &cfg) != SUCCESS) {
        return ERR;
    }

    if (cfg.visitors_path[0] == '\0') {
        strcpy(cfg.visitors_path, DEFAULT_VISITORS_PATH);
    }

    ignore_sigpipe();

    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    app_state_t state;
    if (init_state(&state) != SUCCESS) {
        return ERR;
    }

    if (load_state(&state, &cfg) != SUCCESS) {
        if (fprintf(stderr, "Failed or skipped loading state. Starting fresh.\n") < 0) {
            return ERR_PRINT;
        }
    }

    int server_fd = setup_server_socket(cfg.port);
    if (server_fd < 0) {
        free_state(&state);
        return ERR;
    }

    if (printf("Listening on port: %u (IPv6 only)\n", cfg.port) < 0) {
        return ERR_PRINT;
    }

    hk_args_t hk_args;
    hk_args.state = &state;
    hk_args.cfg = &cfg;

    pthread_t hk_thread;
    if (pthread_create(&hk_thread, nullptr, housekeeping_thread_func, &hk_args) != 0) {
        if (fprintf(stderr, "Failed to create housekeeping thread.\n") < 0) {
            return ERR_PRINT;
        }
        close(server_fd);
        free_state(&state);
        return ERR;
    }

    struct pollfd pfd;
    pfd.fd = server_fd;
    pfd.events = POLLIN;

    while (!atomic_load(&shutdown_requested)) {
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0 && errno != EINTR) {
            break;
        }

        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                continue;
            }

            client_args_t *c_args = malloc(sizeof(client_args_t));
            if (c_args == nullptr) {
                close(client_fd);
                continue;
            }
            c_args->fd = client_fd;
            c_args->state = &state;

            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            // Increment active clients BEFORE creating the thread to avoid a race condition
            atomic_fetch_add(&state.active_clients, 1);

            pthread_t client_thread;
            if (pthread_create(&client_thread, &attr, client_thread_func, c_args) != 0) {
                // If thread creation fails, clean up and decrement
                close(client_fd);
                free(c_args);
                atomic_fetch_sub(&state.active_clients, 1);
            }
            pthread_attr_destroy(&attr);
        }
    }

    if (printf("\nShutting down gracefully. Waiting for active connections to finish...\n") < 0) {
        return ERR_PRINT;
    }

    // Wait for all detached client threads to finish (they have a 5 sec timeout, so this won't hang forever)
    while (atomic_load(&state.active_clients) > 0 && !atomic_load(&force_exit)) {
        usleep(10000); // 10ms
    }

    if (printf("Saving state...\n") < 0) {
        return ERR_PRINT;
    }

    pthread_join(hk_thread, nullptr);
    save_state(&state, &cfg);
    close(server_fd);
    free_state(&state);

    return EXIT_SUCCESS;
}

// --- Signals ---

static void handle_signal(int sig) {
    if (atomic_load(&shutdown_requested)) {
        atomic_store(&force_exit, true);
        _exit(ERR);
    }
    atomic_store(&shutdown_requested, true);
}

static void ignore_sigpipe(void) {
    struct sigaction sa = {0};
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, nullptr);
}

// --- State Management ---

static int init_state(app_state_t *state) {
    state->total_visits = 0;
    state->head = nullptr;
    state->tail = nullptr;
    atomic_init(&state->active_clients, 0); // Initialize thread tracker

    if (pthread_mutex_init(&state->lock, nullptr) != 0) {
        if (fprintf(stderr, "Failed to initialize mutex.\n") < 0) {
            return ERR_PRINT;
        }
        return ERR;
    }
    return SUCCESS;
}

static void free_state(app_state_t *state) {
    pthread_mutex_lock(&state->lock);
    visitor_node_t *curr = state->head;
    while (curr != nullptr) {
        visitor_node_t *next = curr->next;
        free(curr);
        curr = next;
    }
    state->head = nullptr;
    state->tail = nullptr;
    pthread_mutex_unlock(&state->lock);
    pthread_mutex_destroy(&state->lock);
}

static int load_state(app_state_t *state, const cfg_t *cfg) {
    FILE *f = fopen(cfg->visitors_path, "rb");
    if (f == nullptr) {
        return ERR;
    }

    if (fread(&state->total_visits, sizeof(state->total_visits), 1, f) != 1) {
        fclose(f);
        return ERR;
    }

    visitor_node_t temp;
    while (fread(&temp.ip, sizeof(temp.ip), 1, f) == 1 &&
           fread(&temp.timestamp, sizeof(temp.timestamp), 1, f) == 1) {

        visitor_node_t *node = malloc(sizeof(visitor_node_t));
        if (node == nullptr) {
            break;
        }

        memcpy(node->ip, temp.ip, sizeof(temp.ip));
        node->timestamp = temp.timestamp;
        node->next = nullptr;

        if (state->head == nullptr) {
            state->head = node;
            state->tail = node;
        } else {
            state->tail->next = node;
            state->tail = node;
        }
    }
    fclose(f);
    return SUCCESS;
}

static int save_state(app_state_t *state, const cfg_t *cfg) {
    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cfg->visitors_path);

    FILE *f = fopen(tmp_path, "wb");
    if (f == nullptr) {
        return ERR;
    }

    pthread_mutex_lock(&state->lock);

    if (fwrite(&state->total_visits, sizeof(state->total_visits), 1, f) != 1) {
        pthread_mutex_unlock(&state->lock);
        fclose(f);
        return ERR;
    }

    visitor_node_t *curr = state->head;
    while (curr != nullptr) {
        if (fwrite(curr->ip, sizeof(curr->ip), 1, f) != 1 ||
            fwrite(&curr->timestamp, sizeof(curr->timestamp), 1, f) != 1) {

            pthread_mutex_unlock(&state->lock);
            fclose(f);
            return ERR;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&state->lock);
    fclose(f);

    if (rename(tmp_path, cfg->visitors_path) != 0) {
        return ERR;
    }
    return SUCCESS;
}

static uint64_t process_visit(app_state_t *state, const char *ip) {
    time_t now = time(nullptr);
    uint64_t current_total = 0;
    int found = 0;

    pthread_mutex_lock(&state->lock);

    visitor_node_t *curr = state->head;
    while (curr != nullptr) {
        if (strncmp(curr->ip, ip, MAX_IP_LEN) == 0) {
            found = 1;
            break;
        }
        curr = curr->next;
    }

    if (!found) {
        visitor_node_t *new_node = malloc(sizeof(visitor_node_t));
        if (new_node != nullptr) {
            strncpy(new_node->ip, ip, MAX_IP_LEN - 1);
            new_node->ip[MAX_IP_LEN - 1] = '\0';
            new_node->timestamp = now;
            new_node->next = nullptr;

            if (state->head == nullptr) {
                state->head = new_node;
                state->tail = new_node;
            } else {
                state->tail->next = new_node;
                state->tail = new_node;
            }
            state->total_visits++;
        }
    }

    current_total = state->total_visits;
    pthread_mutex_unlock(&state->lock);

    return current_total;
}

// Thread-safe getter to fix the data race issue
static uint64_t get_total_visits(app_state_t *state) {
    pthread_mutex_lock(&state->lock);
    uint64_t total = state->total_visits;
    pthread_mutex_unlock(&state->lock);
    return total;
}

// --- Background Threads ---

static void *housekeeping_thread_func(void *arg) {
    hk_args_t *args = (hk_args_t *)arg;

    while (!atomic_load(&shutdown_requested)) {
        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);

        int min = tm_buf.tm_min;
        int sec = tm_buf.tm_sec;
        int seconds_until_next_hour = 3600 - ((min * 60) + sec);

        while (seconds_until_next_hour > 0 && !atomic_load(&shutdown_requested)) {
            int sleep_time = seconds_until_next_hour > 2 ? 2 : seconds_until_next_hour;
            sleep(sleep_time);
            seconds_until_next_hour -= sleep_time;
        }

        if (atomic_load(&shutdown_requested)) {
            break;
        }

        now = time(nullptr);
        pthread_mutex_lock(&args->state->lock);

        while (args->state->head != nullptr && (now - args->state->head->timestamp) > EXPIRATION_SEC) {
            visitor_node_t *temp = args->state->head;
            args->state->head = args->state->head->next;
            free(temp);
        }
        if (args->state->head == nullptr) {
            args->state->tail = nullptr;
        }

        pthread_mutex_unlock(&args->state->lock);

        save_state(args->state, args->cfg);
    }
    return nullptr;
}

static void *client_thread_func(void *arg) {
    client_args_t *args = (client_args_t *)arg;
    int fd = args->fd;
    app_state_t *state = args->state;
    free(args);

    struct timeval tv;
    tv.tv_sec = HTTP_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    char buf[HTTP_BUF_SIZE];
    ssize_t bytes_read = recv(fd, buf, sizeof(buf) - 1, 0);

    if (bytes_read > 0) {
        buf[bytes_read] = '\0';

        char *header = strstr(buf, "CF-Connecting-IP: ");
        if (header == nullptr) {
            header = strstr(buf, "cf-connecting-ip: ");
        }

        if (header != nullptr) {
            header += 18;
            char *end_r = strchr(header, '\r');
            char *end_n = strchr(header, '\n');

            // Fix: Safe evaluation of pointers to avoid UB
            char *end;
            if (end_r != nullptr && end_n != nullptr) {
                end = (end_r < end_n) ? end_r : end_n;
            } else {
                end = (end_r != nullptr) ? end_r : end_n;
            }

            if (end != nullptr && (end - header) < MAX_IP_LEN) {
                char ip[MAX_IP_LEN] = {0};
                strncpy(ip, header, end - header);

                uint64_t total = process_visit(state, ip);
                send_http_response(fd, total);
            } else {
                send_http_response(fd, get_total_visits(state));
            }
        } else {
            send_http_response(fd, get_total_visits(state));
        }
    }

    close(fd);

    // Decrement the active client counter before exiting
    atomic_fetch_sub(&state->active_clients, 1);
    return nullptr;
}

// --- Network Setup & HTTP ---

static int setup_server_socket(uint16_t port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        if (fprintf(stderr, "Failed to create socket.\n") < 0) {
            return -1;
        }
        return -1;
    }

    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(fd);
        return -1;
    }

    if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt)) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (fprintf(stderr, "Failed to bind to port %u.\n", port) < 0) {
            return -1;
        }
        close(fd);
        return -1;
    }

    if (listen(fd, SOMAXCONN) < 0) {
        if (fprintf(stderr, "Failed to listen on socket.\n") < 0) {
            return -1;
        }
        close(fd);
        return -1;
    }

    return fd;
}

static void send_http_response(int client_fd, uint64_t visits) {
    char body[128];
    int body_len = snprintf(body, sizeof(body), "%lu\n", visits);

    char response[512];
    int resp_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", body_len, body);

    ssize_t total_sent = 0;
    while (total_sent < resp_len) {
        ssize_t sent = send(client_fd, response + total_sent, resp_len - total_sent, MSG_NOSIGNAL);
        if (sent <= 0) {
            break;
        }
        total_sent += sent;
    }
}

// --- Argument Handling ---

static int handle_args(const int argc, char **argv, cfg_t *cfg) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            if (printf(USAGE_STR, argv[0]) < 0) {
                return ERR_PRINT;
            }
            return SUCCESS;
        }
        if (strcmp(argv[i], "-p") == 0) {
            if (i + 1 >= argc) {
                if (fprintf(stderr, "%s: -p requires a port number argument.\n", argv[0]) < 0) {
                    return ERR_PRINT;
                }
                return ERR;
            }

            i++;
            char *arg_str = argv[i];
            char *check_ptr = arg_str;
            while (isspace((unsigned char)*check_ptr)) {
                check_ptr++;
            }
            if (*check_ptr == '-') {
                if (fprintf(stderr, "%s: negative port numbers are not allowed: %s\n", argv[0], arg_str) < 0) {
                    return ERR_PRINT;
                }
                return ERR;
            }

            char *end_ptr;
            errno = 0;
            unsigned long parsed_port = strtoul(arg_str, &end_ptr, 10);

            if (errno != 0 || end_ptr == arg_str || *end_ptr != '\0' || parsed_port > UINT16_MAX) {
                if (fprintf(stderr, "%s: invalid port number: %s\n", argv[0], arg_str) < 0) {
                    return ERR_PRINT;
                }
                return ERR;
            }

            cfg->port = (uint16_t)parsed_port;
        } else if (strcmp(argv[i], "-v") == 0) {
            if (i + 1 >= argc) {
                if (fprintf(stderr, "%s: -v requires a file path argument.\n", argv[0]) < 0) {
                    return ERR_PRINT;
                }
                return ERR;
            }

            i++;
            char *arg_str = argv[i];
            strncpy(cfg->visitors_path, arg_str, sizeof(cfg->visitors_path) - 1);
            cfg->visitors_path[sizeof(cfg->visitors_path) - 1] = '\0';
        } else {
            if (fprintf(stderr, "%s: unknown argument: %s\n", argv[0], argv[i]) < 0) {
                return ERR_PRINT;
            }
            if (fprintf(stderr, USAGE_STR, argv[0]) < 0) {
                return ERR_PRINT;
            }
            return ERR;
        }
    }
    return SUCCESS;
}