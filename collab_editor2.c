#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <time.h>
#include <errno.h>

#define PORT 8080
#define WS_PORT 8081
#define BUFFER_SIZE 65536
#define MAX_CLIENTS 50

typedef struct Client {
    int socket;
    char username[64];
    char current_file[256];
    int cursor_pos;
    char color[16];
    int active;
    pthread_mutex_t lock;
    struct Client* next;
} Client;

Client* clients = NULL;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

const char* colors[] = {"#FF6B6B", "#4ECDC4", "#45B7D1", "#FFA07A", "#98D8C8", "#F7DC6F", "#BB8FCE", "#85C1E2", "#FF69B4", "#20B2AA"};

void add_client(Client* client) {
    pthread_mutex_lock(&clients_mutex);
    client->next = clients;
    clients = client;
    client->active = 1;
    strcpy(client->color, colors[rand() % 10]);
    pthread_mutex_init(&client->lock, NULL);
    pthread_mutex_unlock(&clients_mutex);
    printf("Client added: %s (socket %d)\n", client->username, client->socket);
}

void remove_client(int socket) {
    pthread_mutex_lock(&clients_mutex);
    Client** curr = &clients;
    while (*curr) {
        if ((*curr)->socket == socket) {
            Client* temp = *curr;
            *curr = (*curr)->next;
            printf("Client removed: %s (socket %d)\n", temp->username, temp->socket);
            pthread_mutex_destroy(&temp->lock);
            close(temp->socket);
            free(temp);
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

void ws_send_frame(int socket, const char* message) {
    int len = strlen(message);
    unsigned char frame[BUFFER_SIZE];
    int idx = 0;
    
    frame[idx++] = 0x81;
    
    if (len < 126) {
        frame[idx++] = len;
    } else if (len < 65536) {
        frame[idx++] = 126;
        frame[idx++] = (len >> 8) & 0xFF;
        frame[idx++] = len & 0xFF;
    } else {
        frame[idx++] = 127;
        for (int i = 7; i >= 0; i--) {
            frame[idx++] = (len >> (i * 8)) & 0xFF;
        }
    }
    
    memcpy(&frame[idx], message, len);
    int result = send(socket, frame, idx + len, MSG_NOSIGNAL);
    
    if (result < 0) {
        printf("Failed to send WebSocket frame: %s\n", strerror(errno));
    }
}

void broadcast_message(const char* message, int exclude_socket) {
    pthread_mutex_lock(&clients_mutex);
    Client* curr = clients;
    int count = 0;
    while (curr) {
        if (curr->socket != exclude_socket && curr->active) {
            pthread_mutex_lock(&curr->lock);
            ws_send_frame(curr->socket, message);
            count++;
            pthread_mutex_unlock(&curr->lock);
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&clients_mutex);
    printf("Broadcast to %d clients: %.100s\n", count, message);
}

void send_response(int socket, const char* status, const char* content_type, const char* body) {
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, strlen(body));
    
    send(socket, header, strlen(header), 0);
    send(socket, body, strlen(body), 0);
}

void list_files(int socket) {
    DIR* dir = opendir("./files");
    if (!dir) {
        mkdir("./files", 0755);
        dir = opendir("./files");
    }
    
    char json[BUFFER_SIZE] = "[";
    struct dirent* entry;
    int first = 1;
    
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            if (!first) strcat(json, ",");
            strcat(json, "\"");
            strcat(json, entry->d_name);
            strcat(json, "\"");
            first = 0;
        }
    }
    strcat(json, "]");
    closedir(dir);
    
    send_response(socket, "200 OK", "application/json", json);
}

void read_file(int socket, const char* filename) {
    char path[512];
    snprintf(path, sizeof(path), "./files/%s", filename);
    
    FILE* fp = fopen(path, "r");
    if (!fp) {
        send_response(socket, "404 Not Found", "application/json", "{\"content\":\"\"}");
        return;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    fread(content, 1, size, fp);
    content[size] = '\0';
    fclose(fp);
    
    char* escaped = malloc(size * 2 + 1024);
    char* p = escaped + sprintf(escaped, "{\"content\":\"");
    for (char* c = content; *c; c++) {
        if (*c == '"' || *c == '\\') *p++ = '\\';
        if (*c == '\n') { *p++ = '\\'; *p++ = 'n'; continue; }
        if (*c == '\r') { *p++ = '\\'; *p++ = 'r'; continue; }
        if (*c == '\t') { *p++ = '\\'; *p++ = 't'; continue; }
        *p++ = *c;
    }
    strcpy(p, "\"}");
    
    send_response(socket, "200 OK", "application/json", escaped);
    free(content);
    free(escaped);
}

void write_file(int socket, const char* body) {
    char filename[256] = {0};
    char* content_start = strstr(body, "\"content\":\"");
    char* filename_start = strstr(body, "\"filename\":\"");
    
    if (!filename_start || !content_start) {
        send_response(socket, "400 Bad Request", "application/json", "{\"error\":\"Invalid request\"}");
        return;
    }
    
    filename_start += 12;
    char* filename_end = strchr(filename_start, '"');
    strncpy(filename, filename_start, filename_end - filename_start);
    
    content_start += 11;
    char* content_end = strstr(content_start, "\"}");
    
    char path[512];
    snprintf(path, sizeof(path), "./files/%s", filename);
    
    FILE* fp = fopen(path, "w");
    if (!fp) {
        send_response(socket, "500 Internal Server Error", "application/json", "{\"error\":\"Could not write file\"}");
        return;
    }
    
    for (char* p = content_start; p < content_end; p++) {
        if (*p == '\\' && *(p+1) == 'n') { fputc('\n', fp); p++; }
        else if (*p == '\\' && *(p+1) == 'r') { fputc('\r', fp); p++; }
        else if (*p == '\\' && *(p+1) == 't') { fputc('\t', fp); p++; }
        else if (*p == '\\' && (*(p+1) == '"' || *(p+1) == '\\')) { fputc(*(++p), fp); }
        else fputc(*p, fp);
    }
    
    fclose(fp);
    send_response(socket, "200 OK", "application/json", "{\"success\":true}");
}

void delete_file_handler(int socket, const char* filename) {
    char path[512];
    snprintf(path, sizeof(path), "./files/%s", filename);
    
    if (remove(path) == 0) {
        send_response(socket, "200 OK", "application/json", "{\"success\":true}");
    } else {
        send_response(socket, "404 Not Found", "application/json", "{\"error\":\"File not found\"}");
    }
}

void base64_encode(const unsigned char* input, int length, char* output) {
    const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0, j = 0;
    
    for (i = 0; i < length - 2; i += 3) {
        output[j++] = b64[(input[i] >> 2) & 0x3F];
        output[j++] = b64[((input[i] & 0x3) << 4) | ((input[i+1] & 0xF0) >> 4)];
        output[j++] = b64[((input[i+1] & 0xF) << 2) | ((input[i+2] & 0xC0) >> 6)];
        output[j++] = b64[input[i+2] & 0x3F];
    }
    
    if (i < length) {
        output[j++] = b64[(input[i] >> 2) & 0x3F];
        if (i == length - 1) {
            output[j++] = b64[((input[i] & 0x3) << 4)];
            output[j++] = '=';
        } else {
            output[j++] = b64[((input[i] & 0x3) << 4) | ((input[i+1] & 0xF0) >> 4)];
            output[j++] = b64[((input[i+1] & 0xF) << 2)];
        }
        output[j++] = '=';
    }
    output[j] = '\0';
}

char* ws_handshake(const char* key) {
    char combined[256];
    snprintf(combined, sizeof(combined), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((unsigned char*)combined, strlen(combined), hash);
    
    char base64[64];
    base64_encode(hash, SHA_DIGEST_LENGTH, base64);
    
    char* response = malloc(512);
    snprintf(response, 512,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", base64);
    
    return response;
}

char* ws_read_frame(unsigned char* buffer, int bytes, int* out_len) {
    if (bytes < 2) return NULL;
    
    int opcode = buffer[0] & 0x0F;
    if (opcode == 0x8) return NULL;
    
    int masked = buffer[1] & 0x80;
    int len = buffer[1] & 0x7F;
    int idx = 2;
    
    if (len == 126) {
        if (bytes < 4) return NULL;
        len = (buffer[2] << 8) | buffer[3];
        idx = 4;
    } else if (len == 127) {
        if (bytes < 10) return NULL;
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | buffer[2 + i];
        }
        idx = 10;
    }
    
    unsigned char mask[4];
    if (masked) {
        if (bytes < idx + 4) return NULL;
        memcpy(mask, &buffer[idx], 4);
        idx += 4;
    }
    
    if (bytes < idx + len) return NULL;
    
    char* message = malloc(len + 1);
    for (int i = 0; i < len; i++) {
        message[i] = masked ? (buffer[idx + i] ^ mask[i % 4]) : buffer[idx + i];
    }
    message[len] = '\0';
    *out_len = len;
    
    return message;
}

void* handle_websocket(void* arg) {
    Client* client = (Client*)arg;
    int socket = client->socket;
    
    printf("WebSocket client connected: %s\n", client->username);
    
    char init_msg[256];
    snprintf(init_msg, sizeof(init_msg), "{\"type\":\"init\",\"color\":\"%s\"}", client->color);
    ws_send_frame(socket, init_msg);
    
    char join_msg[512];
    snprintf(join_msg, sizeof(join_msg), "{\"type\":\"user_joined\",\"username\":\"%s\"}", client->username);
    broadcast_message(join_msg, socket);
    
    pthread_mutex_lock(&clients_mutex);
    char users_msg[BUFFER_SIZE] = "{\"type\":\"users_list\",\"users\":[";
    Client* curr = clients;
    int first = 1;
    while (curr) {
        if (curr->active) {
            if (!first) strcat(users_msg, ",");
            char user_data[512];
            snprintf(user_data, sizeof(user_data), 
                "{\"username\":\"%s\",\"color\":\"%s\",\"file\":\"%s\",\"cursor_pos\":%d}",
                curr->username, curr->color, curr->current_file, curr->cursor_pos);
            strcat(users_msg, user_data);
            first = 0;
        }
        curr = curr->next;
    }
    strcat(users_msg, "]}");
    pthread_mutex_unlock(&clients_mutex);
    
    ws_send_frame(socket, users_msg);
    
    unsigned char buffer[BUFFER_SIZE];
    while (1) {
        int bytes = recv(socket, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            printf("Client disconnected: %s\n", client->username);
            break;
        }
        
        int msg_len;
        char* message = ws_read_frame(buffer, bytes, &msg_len);
        if (!message) continue;
        
        if (strstr(message, "\"type\":\"join\"")) {
            char* name = strstr(message, "\"username\":\"");
            if (name) {
                name += 12;
                char* end = strchr(name, '"');
                if (end) {
                    pthread_mutex_lock(&client->lock);
                    strncpy(client->username, name, end - name);
                    client->username[end - name] = '\0';
                    pthread_mutex_unlock(&client->lock);
                }
            }
        }
        else if (strstr(message, "\"type\":\"content_change\"")) {
            char forward_msg[BUFFER_SIZE];
            char* username = strstr(message, "\"username\":\"");
            char* file = strstr(message, "\"file\":\"");
            char* content = strstr(message, "\"content\":\"");
            
            if (username && file && content) {
                username += 12;
                char* uend = strchr(username, '"');
                
                file += 8;
                char* fend = strchr(file, '"');
                
                char uname[64], fname[256];
                strncpy(uname, username, uend - username);
                uname[uend - username] = '\0';
                strncpy(fname, file, fend - file);
                fname[fend - file] = '\0';
                
                content += 11;
                char* cend = strstr(content, "\",\"");
                if (!cend) cend = strstr(content, "\"}");
                
                snprintf(forward_msg, sizeof(forward_msg), 
                    "{\"type\":\"content_update\",\"username\":\"%s\",\"file\":\"%s\",\"content\":\"", 
                    uname, fname);
                strncat(forward_msg, content, cend - content);
                strcat(forward_msg, "\"}");
                
                broadcast_message(forward_msg, socket);
            }
        }
        else if (strstr(message, "\"type\":\"cursor_move\"")) {
            char* pos = strstr(message, "\"position\":");
            char* file = strstr(message, "\"file\":\"");
            char* username = strstr(message, "\"username\":\"");
            
            if (pos && file && username) {
                pos += 11;
                int position = atoi(pos);
                
                pthread_mutex_lock(&client->lock);
                client->cursor_pos = position;
                
                file += 8;
                char* fend = strchr(file, '"');
                strncpy(client->current_file, file, fend - file);
                client->current_file[fend - file] = '\0';
                
                username += 12;
                char* uend = strchr(username, '"');
                strncpy(client->username, username, uend - username);
                client->username[uend - username] = '\0';
                pthread_mutex_unlock(&client->lock);
                
                char cursor_msg[512];
                snprintf(cursor_msg, sizeof(cursor_msg),
                    "{\"type\":\"cursor_update\",\"username\":\"%s\",\"position\":%d,\"color\":\"%s\",\"file\":\"%s\"}",
                    client->username, position, client->color, client->current_file);
                broadcast_message(cursor_msg, socket);
            }
        }
        else if (strstr(message, "\"type\":\"file_change\"")) {
            char* file = strstr(message, "\"file\":\"");
            if (file) {
                file += 8;
                char* fend = strchr(file, '"');
                pthread_mutex_lock(&client->lock);
                strncpy(client->current_file, file, fend - file);
                client->current_file[fend - file] = '\0';
                pthread_mutex_unlock(&client->lock);
            }
        }
        
        free(message);
    }
    
    char leave_msg[512];
    snprintf(leave_msg, sizeof(leave_msg), "{\"type\":\"user_left\",\"username\":\"%s\"}", client->username);
    broadcast_message(leave_msg, socket);
    
    remove_client(socket);
    return NULL;
}

void send_html(int socket);

void* handle_http_client(void* arg) {
    int socket = *(int*)arg;
    free(arg);
    
    char buffer[BUFFER_SIZE];
    int bytes = recv(socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        close(socket);
        return NULL;
    }
    buffer[bytes] = '\0';
    
    char method[16], path[512];
    sscanf(buffer, "%s %s", method, path);
    
    if (strcmp(method, "OPTIONS") == 0) {
        send_response(socket, "200 OK", "text/plain", "");
    }
    else if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        send_html(socket);
    }
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/files", 10) == 0) {
        list_files(socket);
    }
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/file?name=", 15) == 0) {
        char filename[256];
        sscanf(path + 15, "%[^&]", filename);
        char* p = filename;
        while (*p) {
            if (*p == '+') *p = ' ';
            if (*p == '%' && *(p+1) && *(p+2)) {
                char hex[3] = {*(p+1), *(p+2), 0};
                *p = (char)strtol(hex, NULL, 16);
                memmove(p+1, p+3, strlen(p+3)+1);
            }
            p++;
        }
        read_file(socket, filename);
    }
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/api/file", 9) == 0) {
        char* body = strstr(buffer, "\r\n\r\n");
        if (body) write_file(socket, body + 4);
    }
    else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/file?name=", 15) == 0) {
        char filename[256];
        sscanf(path + 15, "%[^&]", filename);
        char* p = filename;
        while (*p) {
            if (*p == '+') *p = ' ';
            if (*p == '%' && *(p+1) && *(p+2)) {
                char hex[3] = {*(p+1), *(p+2), 0};
                *p = (char)strtol(hex, NULL, 16);
                memmove(p+1, p+3, strlen(p+3)+1);
            }
            p++;
        }
        delete_file_handler(socket, filename);
    }
    else {
        send_response(socket, "404 Not Found", "text/html", "<h1>404 Not Found</h1>");
    }
    
    close(socket);
    return NULL;
}

void* websocket_server(void* arg) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(WS_PORT);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("WebSocket bind failed: %s\n", strerror(errno));
        return NULL;
    }
    
    listen(server_fd, 10);
    printf("WebSocket server running on port %d\n", WS_PORT);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) continue;
        
        char buffer[BUFFER_SIZE];
        int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            close(client_socket);
            continue;
        }
        buffer[bytes] = '\0';
        
        char* key = strstr(buffer, "Sec-WebSocket-Key: ");
        if (!key) {
            close(client_socket);
            continue;
        }
        
        key += 19;
        char ws_key[64];
        sscanf(key, "%[^\r\n]", ws_key);
        
        char* response = ws_handshake(ws_key);
        send(client_socket, response, strlen(response), 0);
        free(response);
        
        Client* client = malloc(sizeof(Client));
        client->socket = client_socket;
        sprintf(client->username, "User%d", rand() % 10000);
        client->current_file[0] = '\0';
        client->cursor_pos = 0;
        
        add_client(client);
        
        pthread_t thread;
        pthread_create(&thread, NULL, handle_websocket, client);
        pthread_detach(thread);
    }
    
    close(server_fd);
    return NULL;
}

void send_html(int socket) {
    FILE *f = fopen("editor.html", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *html = malloc(size + 1);
        fread(html, 1, size, f);
        html[size] = '\0';
        fclose(f);
        send_response(socket, "200 OK", "text/html", html);
        free(html);
        return;
    }
    
    const char* html = "<!DOCTYPE html><html><head><title>Collaborative Editor</title></head><body><h1>Real-time Collaborative Text Editor</h1><p>WebSocket collaboration enabled!</p></body></html>";
    send_response(socket, "200 OK", "text/html", html);
}

int main() {
    srand(time(NULL));
    mkdir("./files", 0755);
    
    printf("Starting Collaborative Text Editor Server...\n");
    
    pthread_t ws_thread;
    if (pthread_create(&ws_thread, NULL, websocket_server, NULL) != 0) {
        printf("Failed to create WebSocket thread\n");
        return 1;
    }
    
    sleep(1);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("HTTP bind failed: %s\n", strerror(errno));
        return 1;
    }
    
    listen(server_fd, 10);
    
    printf("HTTP server running on http://0.0.0.0:%d\n", PORT);
    printf("Access from other devices using your IP address\n");
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) continue;
        
        int* socket_ptr = malloc(sizeof(int));
        *socket_ptr = client_socket;
        
        pthread_t thread;
        pthread_create(&thread, NULL, handle_http_client, socket_ptr);
        pthread_detach(thread);
    }
    
    close(server_fd);
    return 0;
}