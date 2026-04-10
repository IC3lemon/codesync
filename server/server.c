#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#define NUM_WORKERS 4

int epoll_fd;
int active_connections = 0;
client_t *clients[MAX_CLIENTS] = {0};
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 
 * QUESTION 1: Analyze how the TCP connection lifecycle and WebSocket protocol upgrade mechanism 
 * enable persistent bidirectional communication and justify the concurrency model adopted.
 * 
 * ANSWER:
 * The TCP connection lifecycle begins with the 3-way handshake (SYN, SYN-ACK, ACK), establishing 
 * a reliable byte-stream channel. Normally, HTTP closes this connection after a request/response 
 * cycle (or keeps it alive for subsequent requests). The WebSocket protocol hijacks this lifecycle 
 * by sending an HTTP Upgrade request (Connection: Upgrade, Upgrade: websocket). Once the server 
 * acknowledges with HTTP 101 Switching Protocols, the connection is kept alive indefinitely. Both 
 * the client and server can now write to the TCP socket asynchronously. Because TCP guarantees 
 * ordering and prevents data loss, it seamlessly serves as the transport for WebSocket frames.
 *
 * Concurrency Model Justification: 
 * We use an I/O multiplexing approach (epoll) combined with a worker thread pool.
 * - Epoll (Reactor): Instead of dedicating a thread per connection (which scales poorly due to 
 *   memory and context-switch overhead), epoll monitors thousands of sockets on a single thread.
 * - Thread Pool (Workers): When epoll detects socket activity (EPOLLIN/EPOLLOUT), it dispatches
 *   the readiness event to a worker thread. This prevents blocking the main event loop if one 
 *   client takes longer to process (e.g. framing parsing or heavy computation), perfectly suited 
 *   for managing multiple simultaneous editors with low latency.
 */

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int setup_server_socket(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* 
     * QUESTION 3: Evaluate socket options impact on responsiveness, scalability, and stability.
     * 
     * ANSWER:
     * - SO_REUSEADDR: Allows immediate rebinding to the port if the server crashes/restarts, 
     *   preventing "Address already in use" errors due to lingering TIME_WAIT TCP sockets. 
     *   Improves scalability/deployability.
     * - SO_KEEPALIVE: Periodically probes inactive connections to detect broken links (e.g. 
     *   client drops offline without FIN). Improves connection stability and cleans up dead FDs.
     * - SO_SNDBUF / SO_RCVBUF: Explicitly tuning these ensures ample kernel buffering for 
     *   spikes in collaborative text edits, which prevents packet dropping but must be balanced 
     *   to avoid buffer bloat.
     * - TCP_NODELAY: Disables Nagle's algorithm. For real-time keystroke collaboration, we 
     *   don't want the kernel buffering small packets waiting for ACKs. We need sub-millisecond 
     *   responsiveness, sending every keystroke immediately.
     */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));

    int buf_size = 1024 * 64; // 64KB
    setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

    int nodelay = 1;
    setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    set_nonblocking(server_fd);
    return server_fd;
}

void broadcast_message(const char *msg, size_t len, int exclude_fd) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->state == 1 && clients[i]->fd != exclude_fd) {
            send_websocket_frame(clients[i]->fd, msg, len);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void remove_client(client_t *client) {
    if (client == NULL) return;
    printf("Client disconnected from %s:%d (fd: %d)\n", client->ip, client->port, client->fd);
    fflush(stdout);
    
    int was_connected = 0;
    char left_name[32] = {0};

    if (client->state == 1) {
        was_connected = 1;
        strncpy(left_name, client->username, 31);
    }
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] == client) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
    close(client->fd);
    free(client);
    
    if (was_connected) {
        if (left_name[0] != '\0') {
            char toast_msg[128];
            snprintf(toast_msg, sizeof(toast_msg), "%s left the workspace", left_name);
            broadcast_toast(toast_msg);
        }
        broadcast_user_count();
    }
}

void *worker_thread(void *arg) {
    (void)arg; // Unused
    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            client_t *client = (client_t *)events[i].data.ptr;

            if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                remove_client(client);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                if (client->state == 0) {
                    if (handle_websocket_handshake(client) == 0) {
                        client->state = 1;
                        printf("WebSocket user fully established from %s:%d (fd: %d)\n", client->ip, client->port, client->fd);
                        fflush(stdout);
                        
                        // Send initial JSON state to newly connected client
                        char *files_json = get_all_files_json();
                        
                        // We will construct the init payload via cJSON directly in websocket.c,
                        // so let's call a specific initialize sender from websocket.c. Wait, we can just build it here since we have the string.
                        // Actually better to have broadcast/init functions in websocket.c. For now, manual json construct:
                        char init_buf[8192];
                        snprintf(init_buf, sizeof(init_buf), "{\"type\":\"init\",\"files\":%s}", files_json ? files_json : "[]");
                        send_websocket_frame(client->fd, init_buf, strlen(init_buf));
                        
                        if (files_json) free(files_json);

                        // CRITICAL: Rearm the socket after handshake since we use EPOLLONESHOT!
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                        ev.data.ptr = client;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
                    } else {
                        remove_client(client);
                    }
                } else if (client->state == 1) {
                    if (read_websocket_frame(client) < 0) {
                        remove_client(client);
                    }
                }
            }
        }
    }
    return NULL;
}

int main() {
    init_db();

    int server_fd = setup_server_socket(PORT);
    epoll_fd = epoll_create1(0);

    struct epoll_event ev;

    pthread_t workers[NUM_WORKERS];
    int worker_ids[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        worker_ids[i] = i;
        pthread_create(&workers[i], NULL, worker_thread, &worker_ids[i]);
    }

    printf("Server listening on port %d\n", PORT);

    // Main reactor loop accepting connections
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            usleep(1000);
            continue;
        }

        printf("Accepted new TCP connection from %s:%d (fd: %d)\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), client_fd);
        fflush(stdout);
        set_nonblocking(client_fd);

        client_t *new_client = (client_t *)malloc(sizeof(client_t));
        new_client->fd = client_fd;
        new_client->state = 0;
        new_client->buffer_len = 0;
        new_client->username[0] = '\0';
        strncpy(new_client->ip, inet_ntoa(client_addr.sin_addr), sizeof(new_client->ip) - 1);
        new_client->ip[sizeof(new_client->ip) - 1] = '\0';
        new_client->port = ntohs(client_addr.sin_port);

        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] == NULL) {
                clients[i] = new_client;
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT; // Using ONESHOT to ensure only 1 thread processes at a time
        ev.data.ptr = new_client;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
    }

    return 0;
}
