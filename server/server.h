#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#define MAX_EVENTS 1024
#define PORT 8080
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 1000

// Represents a connected client
typedef struct {
    int fd;
    int state; // 0: Handshake, 1: Connected
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
    char ip[16];
    int port;
    char username[32];
} client_t;

// WebSocket frame parsing result
typedef struct {
    int is_final;
    int opcode;
    uint8_t *payload;
    size_t payload_len;
} ws_frame_t;

// Function prototypes
int setup_server_socket(int port);
extern client_t *clients[MAX_CLIENTS];
extern pthread_mutex_t clients_mutex;

void set_nonblocking(int fd);
void handle_client_event(client_t *client);
void broadcast_message(const char *msg, size_t len, int exclude_fd);

// WebSocket
int handle_websocket_handshake(client_t *client);
int read_websocket_frame(client_t *client);
void send_websocket_frame(int fd, const char *msg, size_t len);

// Database
void init_db(void);
int create_file(const char *name);
void delete_file(int id);
void rename_file(int id, const char *new_name);
void save_file(int id, const char *content);
char* get_all_files_json(void);

// Connection state
extern int active_connections;
void broadcast_user_count(void);
void broadcast_toast(const char *msg);


#endif
