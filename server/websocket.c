#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include "cJSON.h"

void broadcast_user_count(void) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", "users");
    
    cJSON *users_array = cJSON_CreateArray();
    
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] != NULL && clients[i]->state == 1 && clients[i]->username[0] != '\0') {
            cJSON_AddItemToArray(users_array, cJSON_CreateString(clients[i]->username));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    cJSON_AddItemToObject(payload, "users", users_array);
    char *json_str = cJSON_PrintUnformatted(payload);
    broadcast_message(json_str, strlen(json_str), -1);
    free(json_str);
    cJSON_Delete(payload);
}

void broadcast_toast(const char *msg) {
    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", "toast");
    cJSON_AddStringToObject(payload, "message", msg);
    char *json_str = cJSON_PrintUnformatted(payload);
    broadcast_message(json_str, strlen(json_str), -1);
    free(json_str);
    cJSON_Delete(payload);
}


#define WS_MAGIC_STRING "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// Base64 encoding using OpenSSL
void base64_encode(const unsigned char *buffer, size_t length, char **b64text) {
    BIO *bio, *b64;
    BUF_MEM *bufferPtr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, buffer, length);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &bufferPtr);

    *b64text = (char *)malloc((bufferPtr->length + 1) * sizeof(char));
    memcpy(*b64text, bufferPtr->data, bufferPtr->length);
    (*b64text)[bufferPtr->length] = '\0';

    BIO_free_all(bio);
}

int handle_websocket_handshake(client_t *client) {
    /* 
     * Security and Authentication (Question 3 Answer part 1):
     * The WebSocket handshake occurs over HTTP. In a production environment with secure 
     * communication (WSS over TLS), this plaintext HTTP upgrade is encrypted. 
     * Authentication can be implemented securely by:
     * 1. Passing a secure HTTP-Only cookie during the handshake, validating against the DB session.
     * 2. Passing a Bearer Token in the Subprotocols header (e.g. Sec-WebSocket-Protocol: token_<JWT>).
     * If authentication fails here, we simply return a 401 Unauthorized instead of 101 Switching Protocols.
     */
    ssize_t bytes_read = recv(client->fd, client->buffer + client->buffer_len, BUFFER_SIZE - client->buffer_len - 1, 0);
    if (bytes_read <= 0) return -1;
    
    client->buffer_len += bytes_read;
    client->buffer[client->buffer_len] = '\0';

    if (strstr(client->buffer, "\r\n\r\n") != NULL) {
        char *key_start = strstr(client->buffer, "Sec-WebSocket-Key: ");
        if (key_start) {
            key_start += 19;
            char *key_end = strchr(key_start, '\r');
            if (key_end) {
                int key_len = key_end - key_start;
                char key[256] = {0};
                strncpy(key, key_start, key_len);

                char concat_key[512];
                snprintf(concat_key, sizeof(concat_key), "%s%s", key, WS_MAGIC_STRING);

                unsigned char hash[SHA_DIGEST_LENGTH];
                SHA1((unsigned char *)concat_key, strlen(concat_key), hash);

                char *b64_hash;
                base64_encode(hash, SHA_DIGEST_LENGTH, &b64_hash);

                char response[1024];
                snprintf(response, sizeof(response),
                         "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: %s\r\n\r\n", b64_hash);

                send(client->fd, response, strlen(response), 0);
                free(b64_hash);
                
                // Clear buffer for incoming frames
                client->buffer_len = 0;
                memset(client->buffer, 0, BUFFER_SIZE);
                return 0; // Handshake success
            }
        }
    }
    return -1; // Handshake incomplete or invalid
}

/*
 * QUESTION 2: Implement a concurrent WebSocket server that synchronizes code edits 
 * among multiple users in real time while correctly handling partial transmissions, 
 * buffering, and message framing over TCP.
 * 
 * ANSWER:
 * Partial Transmissions and Buffering:
 * TCP is a stream protocol; it has no concept of "messages". A single `recv` might 
 * read half a WebSocket frame, exactly one frame, or multiple frames. To handle this, 
 * we must maintain a per-client buffer (`client->buffer`). 
 * 1. We append new data to the buffer.
 * 2. We parse the WebSocket header to determine the `payload_len`.
 * 3. We check if `buffer_len >= header_size + payload_len`. 
 *    - If NO: We have a partial transmission. We stop parsing and wait for the next `epoll` event.
 *    - If YES: We have a complete frame. We unmask the payload, process it (e.g. broadcast), 
 *      memmove the remaining buffer data to the start, and update `buffer_len`.
 */
int read_websocket_frame(client_t *client) {
    ssize_t bytes_read = recv(client->fd, client->buffer + client->buffer_len, BUFFER_SIZE - client->buffer_len, 0);
    if (bytes_read <= 0) return -1; // Client disconnected or error

    client->buffer_len += bytes_read;

    // We loop to process all complete frames available in the buffer
    while (client->buffer_len >= 2) {
        uint8_t *data = (uint8_t *)client->buffer;
        int opcode = data[0] & 0x0F;
        int masked = (data[1] & 0x80) != 0;
        uint64_t payload_len = data[1] & 0x7F;

        int header_len = 2;
        if (payload_len == 126) {
            if (client->buffer_len < 4) break; // Incomplete header
            payload_len = (data[2] << 8) | data[3];
            header_len += 2;
        } else if (payload_len == 127) {
            if (client->buffer_len < 10) break; // Incomplete header
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | data[2 + i];
            }
            header_len += 8;
        }

        if (masked) header_len += 4;

        // Check if we have the full frame in the buffer
        if (client->buffer_len < header_len + payload_len) {
            break; // Wait for more data
        }

        // We have a complete frame. Process it.
        uint8_t *masking_key = masked ? data + header_len - 4 : NULL;
        uint8_t *payload = data + header_len;

        if (masked) {
            for (uint64_t i = 0; i < payload_len; i++) {
                payload[i] ^= masking_key[i % 4];
            }
        }

        if (opcode == 0x8) { // Close connection
            return -1;
        }

        if (opcode == 0x1) { // Text frame
            char *text = (char *)malloc(payload_len + 1);
            memcpy(text, payload, payload_len);
            text[payload_len] = '\0';

            char log_text[51] = {0};
            strncpy(log_text, text, 50);
            if (payload_len > 50) strcat(log_text, "...");
            printf("[%s:%d - FD: %d] Received frame (%lu bytes): %s\n", client->ip, client->port, client->fd, payload_len, log_text);
            fflush(stdout);

            cJSON *json = cJSON_Parse(text);
            if (json) {
                cJSON *type = cJSON_GetObjectItem(json, "type");
                if (cJSON_IsString(type)) {
                    if (strcmp(type->valuestring, "edit") == 0) {
                        cJSON *idObj = cJSON_GetObjectItem(json, "fileId");
                        cJSON *contentObj = cJSON_GetObjectItem(json, "content");
                        if (cJSON_IsNumber(idObj) && cJSON_IsString(contentObj)) {
                            save_file(idObj->valueint, contentObj->valuestring);
                            broadcast_message(text, payload_len, client->fd);
                        }
                    } else if (strcmp(type->valuestring, "create") == 0) {
                        cJSON *nameObj = cJSON_GetObjectItem(json, "name");
                        if (cJSON_IsString(nameObj)) {
                            int new_id = create_file(nameObj->valuestring);
                            char resp[512];
                            snprintf(resp, sizeof(resp), "{\"type\":\"create_res\",\"fileId\":%d,\"name\":\"%s\"}", new_id, nameObj->valuestring);
                            broadcast_message(resp, strlen(resp), -1);
                        }
                    } else if (strcmp(type->valuestring, "join") == 0) {
                        cJSON *nameObj = cJSON_GetObjectItem(json, "username");
                        if (cJSON_IsString(nameObj) && strlen(nameObj->valuestring) > 0) {
                            strncpy(client->username, nameObj->valuestring, 31);
                            client->username[31] = '\0';
                            
                            char toast_msg[128];
                            snprintf(toast_msg, sizeof(toast_msg), "%s just synced in", client->username);
                            broadcast_toast(toast_msg);
                            broadcast_user_count();
                        }
                    } else if (strcmp(type->valuestring, "delete") == 0) {
                        cJSON *idObj = cJSON_GetObjectItem(json, "fileId");
                        if (cJSON_IsNumber(idObj)) {
                            delete_file(idObj->valueint);
                            broadcast_message(text, payload_len, -1);
                        }
                    } else if (strcmp(type->valuestring, "rename") == 0) {
                        cJSON *idObj = cJSON_GetObjectItem(json, "fileId");
                        cJSON *newNameObj = cJSON_GetObjectItem(json, "name");
                        if (cJSON_IsNumber(idObj) && cJSON_IsString(newNameObj)) {
                            rename_file(idObj->valueint, newNameObj->valuestring);
                            broadcast_message(text, payload_len, -1);
                        }
                    }
                }
                cJSON_Delete(json);
            }
            free(text);
        }

        // Shift remaining data to start of buffer
        size_t total_frame_len = header_len + payload_len;
        size_t remaining = client->buffer_len - total_frame_len;
        if (remaining > 0) {
            memmove(client->buffer, client->buffer + total_frame_len, remaining);
        }
        client->buffer_len = remaining;
    }

    // Rearm epoll oneshot
    extern int epoll_fd;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.ptr = client;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);

    return 1;
}

void send_websocket_frame(int fd, const char *msg, size_t len) {
    uint8_t header[10];
    int header_len = 2;

    header[0] = 0x81; // FIN + Text frame
    if (len <= 125) {
        header[1] = len;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        header_len += 2;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++) {
            header[2 + i] = (len >> ((7 - i) * 8)) & 0xFF;
        }
        header_len += 8;
    }

    // Since we are server, we DO NOT mask sent frames per RFC 6455
    send(fd, header, header_len, 0);
    send(fd, msg, len, 0);
}
