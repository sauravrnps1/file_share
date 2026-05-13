#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <termios.h>
#include <sys/select.h>
#include <signal.h> // Added for SIGPIPE

#define TCP_PORT 3000
#define BUFFER_SIZE 65536
#define SERVE_DIR "/mnt/c/Users/saura" // Change this to your directory
#define MAX_CLIENTS 100

// Thread-safe client tracker
struct ClientInfo {
    int sock;
    char ip[INET_ADDRSTRLEN];
    volatile int allowed;
    volatile int active;
};

struct ClientInfo clients[MAX_CLIENTS];
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
int selected_client = 0;

// Raw Terminal Mode for TUI
struct termios orig_termios;
void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}
void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

int read_all(int sock, void* buf, int len) {
    int total = 0;
    while(total < len) {
        int n = read(sock, (char*)buf + total, len - total);
        if(n <= 0) return -1;
        total += n;
    }
    return 0;
}

void render_server_ui() {
    printf("\033[H\033[J"); // Clear screen
    printf("\033[1;36m=== TCP File Server Admin ===\033[0m\r\n");
    printf("Serving: %s\r\n", SERVE_DIR);
    printf("-----------------------------\r\n");
    
    pthread_mutex_lock(&clients_mutex);
    int any = 0;
    for(int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            any = 1;
            if (i == selected_client) printf(" \033[1;33m>>\033[0m "); 
            else printf("    ");
            
            printf("Client IP: %-15s | Status: ", clients[i].ip);
            if (clients[i].allowed) printf("\033[1;32m[ALLOWED]\033[0m \r\n");
            else printf("\033[1;31m[DENIED]\033[0m  \r\n");
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    
    if (!any) printf(" No clients connected.\r\n");
    
    printf("-----------------------------\r\n");
    printf("Controls: \033[1mUp/Down\033[0m: Select | \033[1mLeft/Right\033[0m: Allow/Deny | \033[1m'q'\033[0m: Quit\r\n");
    fflush(stdout);
}

// Background thread that handles a single client's requests
void* client_handler(void* arg) {
    int client_id = *((int*)arg);
    free(arg);
    int sock = clients[client_id].sock;
    
    while(1) {
        uint16_t opcode;
        if (read_all(sock, &opcode, 2) < 0) break;
        opcode = ntohs(opcode);
        
        uint16_t path_len;
        if (read_all(sock, &path_len, 2) < 0) break;
        path_len = ntohs(path_len);
        
        char path[2048] = {0};
        if (path_len > 0) {
            if (read_all(sock, path, path_len) < 0) break;
        }
        
        // Security checks
        pthread_mutex_lock(&clients_mutex);
        int allowed = clients[client_id].allowed;
        pthread_mutex_unlock(&clients_mutex);
        
        if (!allowed || strstr(path, "..") != NULL) {
            uint16_t err = htons(5); write(sock, &err, 2);
            continue;
        }
        
        char full_path[4096];
        if (strlen(path) > 0) snprintf(full_path, sizeof(full_path), "%s/%s", SERVE_DIR, path);
        else snprintf(full_path, sizeof(full_path), "%s", SERVE_DIR);

        if (opcode == 8) { // DIR REQUEST
            DIR *d; struct dirent *dir; struct stat st; char filepath[4096];
            int offset = 0; char dir_buf[65536] = {0};
            
            // Pass 1: Folders
            if ((d = opendir(full_path))) {
                while ((dir = readdir(d)) != NULL) {
                    if (dir->d_name[0] == '.') continue;
                    snprintf(filepath, sizeof(filepath), "%s/%s", full_path, dir->d_name);
                    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
                        if (offset + 512 >= sizeof(dir_buf)) break;
                        offset += snprintf(dir_buf + offset, sizeof(dir_buf) - offset, "D %s\n", dir->d_name);
                    }
                } closedir(d);
            }
            // Pass 2: Files
            if ((d = opendir(full_path))) {
                while ((dir = readdir(d)) != NULL) {
                    if (dir->d_name[0] == '.') continue;
                    snprintf(filepath, sizeof(filepath), "%s/%s", full_path, dir->d_name);
                    if (stat(filepath, &st) == 0 && S_ISREG(st.st_mode)) {
                        if (offset + 512 >= sizeof(dir_buf)) break;
                        offset += snprintf(dir_buf + offset, sizeof(dir_buf) - offset, "F %s\n", dir->d_name);
                    }
                } closedir(d);
            }
            
            uint16_t ok_op = htons(9); write(sock, &ok_op, 2);
            uint32_t len = htonl(offset); write(sock, &len, 4);
            write(sock, dir_buf, offset);
            
        } else if (opcode == 1) { // GET REQUEST
            FILE *file = fopen(full_path, "rb");
            if (!file) {
                uint16_t err_op = htons(5); write(sock, &err_op, 2);
                continue;
            }

            fseek(file, 0, SEEK_END);
            uint64_t file_size = ftell(file);
            rewind(file);

            char header[10];
            uint16_t size_op = htons(7);
            memcpy(header, &size_op, 2);
            for(int i = 0; i < 8; i++) header[2+i] = (file_size >> (56 - (8*i))) & 0xFF;
            write(sock, header, 10);

            size_t bytes_read;
            char file_buffer[BUFFER_SIZE];
            int client_dropped = 0; // NEW: Track if client disconnects during transfer

            while ((bytes_read = fread(file_buffer, 1, BUFFER_SIZE, file)) > 0) {
                int sent = 0;
                while (sent < bytes_read) {
                    int n = write(sock, file_buffer + sent, bytes_read - sent);
                    if (n <= 0) { 
                        client_dropped = 1; // Mark client as dropped
                        break; 
                    } 
                    sent += n;
                }
                if (client_dropped) break; // Break out of file reading loop entirely
            }
            fclose(file);
        }
    }
    
    // Cleanup on disconnect
    close(sock);
    pthread_mutex_lock(&clients_mutex);
    clients[client_id].active = 0;
    pthread_mutex_unlock(&clients_mutex);
    render_server_ui();
    return NULL;
}

// Background thread to constantly accept new clients
void* accept_clients(void* arg) {
    int server_fd = *(int*)arg;
    while(1) {
        struct sockaddr_in address;
        socklen_t addrlen = sizeof(address);
        int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket < 0) continue;
        
        pthread_mutex_lock(&clients_mutex);
        int client_id = -1;
        for(int i=0; i<MAX_CLIENTS; i++) {
            if (!clients[i].active) { client_id = i; break; }
        }
        
        if (client_id == -1) { close(new_socket); pthread_mutex_unlock(&clients_mutex); continue; }
        
        clients[client_id].sock = new_socket;
        inet_ntop(AF_INET, &(address.sin_addr), clients[client_id].ip, INET_ADDRSTRLEN);
        clients[client_id].allowed = 1; // Default allow
        clients[client_id].active = 1;
        pthread_mutex_unlock(&clients_mutex);
        
        int* arg_id = malloc(sizeof(int));
        *arg_id = client_id;
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, arg_id);
        pthread_detach(tid);
        render_server_ui();
    }
}

int main() {
    // NEW: Ignore SIGPIPE so writing to a disconnected client doesn't crash the server
    signal(SIGPIPE, SIG_IGN);

    int server_fd; struct sockaddr_in address; int opt = 1;
    
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) exit(1);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(TCP_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) exit(1);
    if (listen(server_fd, 10) < 0) exit(1);

    memset(clients, 0, sizeof(clients));

    pthread_t accept_tid;
    pthread_create(&accept_tid, NULL, accept_clients, &server_fd);

    enableRawMode();

    // Main UI Event Loop
    while (1) {
        render_server_ui();
        
        struct timeval tv = {0, 100000}; // 100ms refresh checking 
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
            char c; read(STDIN_FILENO, &c, 1);
            if (c == 'q') break;
            if (c == '\033') {
                char seq[2];
                read(STDIN_FILENO, &seq[0], 1); read(STDIN_FILENO, &seq[1], 1);
                
                pthread_mutex_lock(&clients_mutex);
                int active_map[MAX_CLIENTS]; int num_active = 0;
                for(int i=0; i<MAX_CLIENTS; i++) if (clients[i].active) active_map[num_active++] = i;
                
                if (num_active > 0) {
                    int active_idx = 0;
                    for(int i=0; i<num_active; i++) if (active_map[i] == selected_client) active_idx = i;
                    
                    if (seq[1] == 'A') { // UP
                        active_idx = (active_idx - 1 + num_active) % num_active;
                        selected_client = active_map[active_idx];
                    } else if (seq[1] == 'B') { // DOWN
                        active_idx = (active_idx + 1) % num_active;
                        selected_client = active_map[active_idx];
                    } else if (seq[1] == 'C') { // RIGHT
                        clients[selected_client].allowed = 1;
                    } else if (seq[1] == 'D') { // LEFT
                        clients[selected_client].allowed = 0;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
            }
        }
    }
    return 0;
}
