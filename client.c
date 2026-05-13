#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>
#include <termios.h>
#include <sys/select.h>

#define TCP_PORT 3000
#define MAX_ITEMS 1000

int sockfd;
char server_ip_global[256]; // Store IP for transparent reconnects
char current_path[2048] = "";
char status_msg[256] = "";

struct Item {
    char type; // 'D' or 'F'
    char name[256];
};
struct Item items[MAX_ITEMS];
int num_items = 0;
int selected_item = 0;

// Raw Terminal Mode
struct termios orig_termios;
void disableRawMode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); printf("\033[?25h"); }
void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios); atexit(disableRawMode);
    struct termios raw = orig_termios; raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw); printf("\033[?25l");
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

// Helper to silently drop and reconnect to clear all TCP buffers instantly
void reconnect_to_server() {
    close(sockfd);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, server_ip_global, &serv_addr.sin_addr);
    connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
}

void request_dir() {
    uint16_t op = htons(8); write(sockfd, &op, 2);
    uint16_t path_len = htons(strlen(current_path)); write(sockfd, &path_len, 2);
    if (strlen(current_path) > 0) write(sockfd, current_path, strlen(current_path));
    
    uint16_t res_op;
    if (read_all(sockfd, &res_op, 2) < 0) { strcpy(status_msg, "Connection dropped."); return; }
    res_op = ntohs(res_op);
    
    if (res_op == 5) {
        strcpy(status_msg, "Directory not found or Access Denied.");
        num_items = 0; return;
    }
    
    if (res_op == 9) {
        uint32_t payload_len; read_all(sockfd, &payload_len, 4);
        payload_len = ntohl(payload_len);
        
        char* buf = malloc(payload_len + 1);
        if(payload_len > 0) read_all(sockfd, buf, payload_len);
        buf[payload_len] = '\0';
        
        num_items = 0; status_msg[0] = '\0';
        char *line = strtok(buf, "\n");
        while(line != NULL && num_items < MAX_ITEMS) {
            if (strlen(line) >= 3) {
                items[num_items].type = line[0];
                strncpy(items[num_items].name, line + 2, 255);
                num_items++;
            }
            line = strtok(NULL, "\n");
        }
        free(buf);
    }
}

void render_ui() {
    printf("\033[H\033[J"); // clear
    printf("\033[1;36m=== Remote TCP File Browser ===\033[0m\r\n");
    printf("Path: /%s\r\n", current_path);
    if (strlen(status_msg) > 0) printf("\033[1;31m%s\033[0m\r\n", status_msg);
    printf("-------------------------------\r\n");
    
    if (num_items == 0) printf(" (Empty or Access Denied)\r\n");
    
    int start_idx = selected_item < 10 ? 0 : selected_item - 10;
    int end_idx = start_idx + 20;
    if (end_idx > num_items) end_idx = num_items;
    
    for(int i = start_idx; i < end_idx; i++) {
        if (i == selected_item) printf(" \033[1;33m>>\033[0m "); else printf("    ");
        
        if (items[i].type == 'D') printf("\033[1;34m[DIR]\033[0m  %s \r\n", items[i].name);
        else                      printf("[FILE] %s \r\n", items[i].name);
    }
    
    printf("-------------------------------\r\n");
    printf("Controls: \033[1mUp/Down\033[0m: Select | \033[1mRight\033[0m: Open Dir/Download File | \033[1mLeft\033[0m: Go Back | \033[1m'q'\033[0m: Quit\r\n");
    fflush(stdout);
}

void download_file(const char* filename) {
    char req_path[2048];
    if (strlen(current_path) > 0) snprintf(req_path, sizeof(req_path), "%s/%s", current_path, filename);
    else snprintf(req_path, sizeof(req_path), "%s", filename);
    
    uint16_t op = htons(1); write(sockfd, &op, 2);
    uint16_t path_len = htons(strlen(req_path)); write(sockfd, &path_len, 2);
    write(sockfd, req_path, strlen(req_path));
    
    uint16_t res_op;
    if (read_all(sockfd, &res_op, 2) < 0) { strcpy(status_msg, "Connection lost."); return; }
    res_op = ntohs(res_op);
    
    if (res_op == 5) { strcpy(status_msg, "Error: File not found or Access Denied."); return; }
    
    if (res_op == 7) {
        uint8_t size_buf[8];
        if (read_all(sockfd, size_buf, 8) < 0) { strcpy(status_msg, "Connection lost."); return; }

        uint64_t file_size = 0;
        for(int i = 0; i < 8; i++) file_size = (file_size << 8) | size_buf[i];
        
        FILE *file = fopen(filename, "wb");
        if (!file) { strcpy(status_msg, "Failed to create local file."); return; }
        
        printf("\r\n\033[1;36mDownloading %s (%.2f MB)...\033[0m\r\n\r\n", filename, (double)file_size/1048576);
        
        char data_buf[8192]; 
        uint64_t total_received = 0;
        int is_paused = 0;
        int selected_action = 0; // 0 = Pause/Resume, 1 = Cancel

        // Multiplexing Loop: Listens to BOTH the Server (for data) and the Keyboard (for commands)
        while (total_received < file_size) {
            
            // 1. Draw the interactive progress bar
            int bar_width = 30; 
            float progress = (float)total_received / file_size;
            int pos = bar_width * progress;
            
            printf("\r\033[K"); // Clear the line to prevent visual artifacts
            printf("[");
            for (int i=0; i<bar_width; ++i) {
                if (i < pos) printf("="); else if (i == pos) printf(">"); else printf(" ");
            }
            printf("] %3d%%  ", (int)(progress * 100.0));
            
            // Draw interactive buttons
            if (selected_action == 0) {
                printf("  \033[1;33m> [%s] <\033[0m     [ Cancel ]   ", is_paused ? "Resume" : "Pause ");
            } else {
                printf("    [%s]       \033[1;33m> [ Cancel ] <\033[0m ", is_paused ? "Resume" : "Pause ");
            }
            fflush(stdout);

            // 2. Setup Select to watch Keyboard AND Socket
            fd_set readfds; 
            FD_ZERO(&readfds);
            FD_SET(STDIN_FILENO, &readfds);
            
            // Only watch the socket for data if we are NOT paused
            if (!is_paused) FD_SET(sockfd, &readfds);

            struct timeval tv = {0, 50000}; // 50ms UI refresh rate
            int max_fd = sockfd > STDIN_FILENO ? sockfd : STDIN_FILENO;

            int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);

            if (activity > 0) {
                // Check if user pressed a key
                if (FD_ISSET(STDIN_FILENO, &readfds)) {
                    char c; 
                    if (read(STDIN_FILENO, &c, 1) > 0) {
                        if (c == '\033') { // Arrow Key sequence
                            char seq[2]; 
                            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                                if (seq[0] == '[') {
                                    if (seq[1] == 'C') selected_action = 1; // Right Arrow -> Cancel
                                    else if (seq[1] == 'D') selected_action = 0; // Left Arrow -> Pause/Resume
                                }
                            }
                        } else if (c == '\n' || c == '\r') { // Enter Key
                            if (selected_action == 1) { 
                                // CANCEL TRIGGERED
                                fclose(file);
                                remove(filename); // Delete incomplete file
                                strcpy(status_msg, "Download Cancelled.");
                                reconnect_to_server(); // Silently drop and reconnect to clear TCP buffers
                                request_dir();         // Fetch directory fresh
                                return;
                            } else { 
                                // PAUSE/RESUME TRIGGERED
                                is_paused = !is_paused;
                            }
                        }
                    }
                }

                // Check if server sent data (and we aren't paused)
                if (!is_paused && FD_ISSET(sockfd, &readfds)) {
                    int to_read = (file_size - total_received < sizeof(data_buf)) ? (file_size - total_received) : sizeof(data_buf);
                    int n = read(sockfd, data_buf, to_read);
                    if (n <= 0) { 
                        strcpy(status_msg, "Connection lost during download.");
                        fclose(file);
                        return;
                    }
                    fwrite(data_buf, 1, n, file);
                    total_received += n;
                }
            }
        }
        
        fclose(file);
        printf("\r\n\n\033[1;32mDownload Complete!\033[0m Press any key to continue...\r\n");
        tcflush(STDIN_FILENO, TCIFLUSH); // Clear accidental spam keys
        char dummy; read(STDIN_FILENO, &dummy, 1);
        strcpy(status_msg, "Download Successful!");
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) { printf("Usage: ./client <Server_IP>\n"); return 1; }
    
    strcpy(server_ip_global, argv[1]); // Save for reconnection
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(TCP_PORT);
    inet_pton(AF_INET, server_ip_global, &serv_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed"); return 1;
    }

    enableRawMode();
    request_dir(); // Initial fetch

    while(1) {
        render_ui();
        char c; read(STDIN_FILENO, &c, 1);
        if (c == 'q') break;
        
        if (c == '\033') { // Escape sequence detected
            char seq[2]; read(STDIN_FILENO, &seq[0], 1); read(STDIN_FILENO, &seq[1], 1);
            if (seq[0] == '[') {
                if (seq[1] == 'A' && selected_item > 0) selected_item--;
                else if (seq[1] == 'B' && selected_item < num_items - 1) selected_item++;
                else if (seq[1] == 'C') { // RIGHT
                    if (num_items > 0) {
                        if (items[selected_item].type == 'D') {
                            if (strlen(current_path) > 0) strcat(current_path, "/");
                            strcat(current_path, items[selected_item].name);
                            request_dir(); selected_item = 0;
                        } else download_file(items[selected_item].name); // FILE
                    }
                }
                else if (seq[1] == 'D') { // LEFT
                    char *last_slash = strrchr(current_path, '/');
                    if (last_slash != NULL) *last_slash = '\0';
                    else current_path[0] = '\0';
                    request_dir(); selected_item = 0;
                }
            }
        }
    }
    
    close(sockfd);
    return 0;
}