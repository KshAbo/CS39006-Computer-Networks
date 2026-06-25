// 23CS30029 : Kshetrimayum Abo
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "global.c"


int listen_fd;
int port;

client_connection clients[MAX_CLIENTS];
user_list users;

void insertUser(char name[NAME_LEN], char password[PASSWORD_LEN]){
    if(users.size >= MAX_USERS){
        printf("user list full\n");
        return;
    }

    strcpy(users.users[users.size].name, name);
    strcpy(users.users[users.size].password, password);
    users.size++;
    return;
}

void printUserList(){
    for(int i = 0; i < users.size; i++){
        printf("%s %s\n", users.users[i].name, users.users[i].password);
    }
}

int findUser(char* name){
    for(int i = 0; i < users.size; i++){
        if(strcmp(users.users[i].name, name) == 0) return i;
    }
    return -1;
}

// Checks if there is a METADATA_FILE file under the user folder that stores the mails received
// if there is none, it fallbacks to finding the maximum number of the mail
int getNextMailId(const char *username) {
    char dir_path_buf[DIR_PATH_MAXSIZE];
    char meta_path_buf[DIR_PATH_MAXSIZE + 10];

    sprintf(dir_path_buf, "mailboxes/%s", username);
    sprintf(meta_path_buf, "%s/%s", dir_path_buf, METADATA_FILE);

    // Finding the metadata file
    FILE *meta_f = fopen(meta_path_buf, "r");
    if (meta_f != NULL) {
        int next_id;
        if (fscanf(meta_f, "%d", &next_id) == 1) {
            fclose(meta_f);
            return next_id;
        }
        fclose(meta_f);
    }

    // Fallback of listing the files and finding the max id
    DIR *d = opendir(dir_path_buf);
    int max_id = 0;
    
    if (d) {
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".txt")) {
                int id = atoi(dir->d_name);
                if (id > max_id) {
                    max_id = id;
                }
            }
        }
        closedir(d);
    }
    return max_id + 1;
}

void printLog(const char* format, ...){

    time_t rawtime;
    time(&rawtime);
    struct tm *timeinfo;
    timeinfo = localtime(&rawtime);

    char timestamp_buf[TIMESTAMP_MAXSIZE];
    strftime(timestamp_buf, sizeof(timestamp_buf), "%Y-%m-%d %H:%M:%S", timeinfo);

    printf("[%s] ", timestamp_buf);

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

int createTcpConnection(){
    return socket(AF_INET, SOCK_STREAM, 0);
}

struct sockaddr_in* createAddress(char* ip, int port){
    struct sockaddr_in* addr = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    if (strlen(ip) > 0){
        inet_pton(AF_INET, ip, &addr->sin_addr);
    }else{
        addr->sin_addr.s_addr = INADDR_ANY;
    }
    return addr;
}


// Initialise an internal data structure for valid users from the provided user file
void init(const char* file){

    if(mkdir("mailboxes", 0777) == -1){
        if (errno != EEXIST) {
            perror("Failed to create directory");
            exit(1);
        }
    }

    FILE* file_ptr = fopen(file, "r");
    if(file_ptr == NULL){
        perror("userfile couldn't be opened");
        exit(1);
    }

    users.size = 0;

    char msg_line[NAME_LEN + PASSWORD_LEN + 1];
    while(fgets(msg_line, NAME_LEN + PASSWORD_LEN + 1, file_ptr)){
        char name[NAME_LEN], password[PASSWORD_LEN];

        strcpy(name, strtok(msg_line, " \n"));
        strcpy(password, strtok(NULL, " \n"));

        insertUser(name, password);

        char dir_name[NAME_LEN + 100];
        sprintf(dir_name, "mailboxes/%s", name);

        if(mkdir(dir_name, 0777) == -1 && errno != EEXIST){
            perror("mkdir failed");
        }

    }

    printLog("Loaded %d users from %s\n", users.size, file);
}

int userExists(char* name){
    for(int i = 0; i < users.size; i++){
        if(strcmp(users.users[i].name, name) == 0){
            return 1;
        }
    }
    return 0;
}

void initSendContext(smtp2_context *context, int client_fd){
    context->client_fd = client_fd; 
    context->subject[0] = 0;
    context->state = SMTP2_INIT;
    context->body[0] = 0;
    context->recipient_count = 0;
    for(int i = 0; i < MAX_RECIPIENTS; i++) context->recipients[i][0] = 0;
}

void initRetrieveContext(smp_context *context, int client_fd){
    context->state = SMP_INIT;
    context->client_fd = client_fd;
    context->auth_attempts = 0;
    context->current_nonce[0] = 0;
    context->user = NULL;
}

void handleSend(smtp2_context* context, char* msg){

    protocol_message parsed_msg;
    parseMessage(msg, &parsed_msg);

    switch(context->state){
        case SMTP2_INIT:
            break;

        case SMTP2_FROM: 
            {
                if(strcmp(parsed_msg.command, "FROM") != 0) {
                    sendCmd(context->client_fd, "ERR Bad sequence");
                    return;
                }

                strcpy(context->name, parsed_msg.payload);

                printLog("Name: %s\n", parsed_msg.payload);

                sendCmd(context->client_fd, "OK Sender accepted");

                context->state = SMTP2_TO_SUB;
            }
            break;

        case SMTP2_TO_SUB:
            {
                if(strcmp(parsed_msg.command, "TO") == 0){      // To
                    char* name = parsed_msg.payload;
                    if(name == NULL || strlen(name) <= 0) break;
                    
                    if(!userExists(name)){
                        sendCmd(context->client_fd, "ERR No such user");
                    }
                    else{
                        strcpy(context->recipients[context->recipient_count], name);
                        context->recipient_count++;
                        sendCmd(context->client_fd, "OK Recipient accepted");
                    }
                }else if(strcmp(parsed_msg.command, "SUB") == 0){ // Subject
                    char *subject = parsed_msg.payload;
                    if(strlen(subject) == 0){
                        strcpy(context->subject, "(no subject)");
                    }else{
                        strcpy(context->subject, subject);
                    }
                    sendCmd(context->client_fd, "OK Subject accepted");
                    context->state = SMTP2_BODY;
                    break;
                }else{      // Bad sequence
                    sendCmd(context->client_fd, "ERR Bad sequence");
                    return;
                }
            }
            break;

        case SMTP2_BODY:
            {
                if(strcmp(parsed_msg.command, "BODY") != 0) {
                    sendCmd(context->client_fd, "ERR Bad sequence");
                    break;
                }

                if(context->recipient_count > 0) {
                    sendCmd(context->client_fd, "OK Send body, end with CRLF.CRLF");
                } else {
                    sendCmd(context->client_fd, "ERR No valid recipients");
                    context->state = SMTP2_INIT;
                    break;
                }

                printLog("Body start\n");
                char msg_line[SMTP2_MAXSIZE];
                char body[SMTP2_MAXSIZE * 128];
                
                body[0] = '\0'; 
                int total_bytes = 0;
                int is_overflow = 0;

                // Keep on taking in till we encounter ".\r\n" or if the input bytes exceeds 65536
                while(1){
                    int n = recvMsg(context->client_fd, msg_line, SMTP2_MAXSIZE, "\r\n");
                    if (n <= 0) break;

                    printLog("%s", msg_line);

                    if(strcmp(msg_line, ".\r\n") == 0) {
                        break;
                    }

                    if (n >= 2) {
                        msg_line[n - 2] = '\n'; 
                        msg_line[n - 1] = '\0';
                    }

                    char *write_ptr = msg_line;
                    if (msg_line[0] == '.') {
                        write_ptr = msg_line + 1; 
                    }

                    if (!is_overflow) {
                        total_bytes += strlen(write_ptr);
                        
                        if(total_bytes > sizeof(body) - 1) {
                            is_overflow = 1; // Trigger overflow, but DO NOT break!
                        } else {
                            strcat(body, write_ptr); 
                        }
                    }
                }
                
                printLog("Body end\n");
                
                if(!is_overflow){
                    strcpy(context->body, body);
                    context->state = SMTP2_FINISH;
                }else{
                    sendCmd(context->client_fd, "ERR Body too large");
                    context->state = SMTP2_INIT;
                }
            }
            break;

        case SMTP2_FINISH:
            {
                char recipient_list[NAME_LEN * MAX_RECIPIENTS];
                strcpy(recipient_list, context->recipients[0]);
                for(int i = 1; i < context->recipient_count; i++){
                    char recipient[NAME_LEN + 2];
                    sprintf(recipient, ", %s", context->recipients[i]);
                    strcat(recipient_list, recipient);
                }

                for(int i = 0; i < context->recipient_count; i++){
                    int msg_id = getNextMailId(context->recipients[i]);
                            
                    // The metadata will store the next available msg id
                    char meta_path[DIR_PATH_MAXSIZE];
                    sprintf(meta_path, "mailboxes/%s/%s", context->recipients[i], METADATA_FILE);
                    FILE *meta_ptr = fopen(meta_path, "w");
                    if (meta_ptr) {
                        fprintf(meta_ptr, "%d", msg_id + 1); 
                        fclose(meta_ptr);
                    }

                    char file_name[NAME_LEN * 5];
                    sprintf(file_name, "mailboxes/%s/%d.txt", context->recipients[i], msg_id);

                    FILE *file_ptr = fopen(file_name, "w");
                    if (file_ptr == NULL) {
                        continue;
                    }

                    time_t rawtime;
                    time(&rawtime);
                    struct tm *timeinfo = localtime(&rawtime);
                    char t[80];
                    strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", timeinfo);

                    fprintf(file_ptr, "From: %s\n"
                                      "To: %s\n"
                                      "Subject: %s\n"
                                      "Date: %s\n"
                                      "---\n"
                                      "%s", context->name, recipient_list, context->subject, t, context->body);
                    
                    fclose(file_ptr);
                }

                printLog("Mail delivered from \"%s\" to [%s] (%d recipient(s))\n", context->name, recipient_list, context->recipient_count);

                sendCmd(context->client_fd, "OK Delivered to %d mailboxes", context->recipient_count);

                context->state = SMTP2_INIT;
            }
            break;
    }

}

void generateNonce(char *nonce_buffer) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    int charset_size = sizeof(charset) - 1; 

    for (int i = 0; i < 8; i++) {
        int random_index = rand() % charset_size;
        nonce_buffer[i] = charset[random_index];
    }
    
    nonce_buffer[8] = '\0'; 
}

void handleRetrieve(smp_context *context, char *msg){
    protocol_message parsed_msg;
    parseMessage(msg, &parsed_msg);

    switch (context->state) {
        case SMP_INIT: 
            break;

        case SMP_INIT_AUTH:
            {
                char nonce_buf[NONCE_LEN];
                generateNonce(nonce_buf);
                sendCmd(context->client_fd, "AUTH REQUIRED %s", nonce_buf);
                strcpy(context->current_nonce, nonce_buf);

                context->state = SMP_AUTH;
            }
            break;

        case SMP_AUTH:
            {
                if(context->auth_attempts >= 3){
                    sendCmd(context->client_fd, "ERR Too many failures");
                    close(context->client_fd);
                    context->state = SMP_INIT;
                    break;
                }

                if(strcmp(parsed_msg.command, "AUTH") != 0){
                    return;
                }

                char username[NAME_LEN];
                unsigned long hash;
                sscanf(parsed_msg.payload, "%s %lu", username, &hash);

                int idx = findUser(username);
                if(idx < 0) {
                    sendCmd(context->client_fd, "ERR Authentication failed");
                    context->auth_attempts += 1;
                    break;
                }

                char pass[PASSWORD_LEN + 9];
                strcpy(pass, users.users[idx].password);
                strcat(pass, context->current_nonce);

                unsigned long computed_hash = djb2(pass);

                if(computed_hash != hash){
                    sendCmd(context->client_fd, "ERR Authentication failed");
                    context->auth_attempts += 1;
                    break;
                }else{
                    printLog("Authentication successful for user %s\n", username);
                    sendCmd(context->client_fd, "OK Welcome %s", users.users[idx].name);
                    context->user = &users.users[idx];
                    context->state = SMP_MAILBOX_READY;
                }
            }
            break;

        case SMP_MAILBOX_READY:
            {
                if(strcmp(parsed_msg.command, "LIST") == 0){

                    char file_path[DIR_PATH_MAXSIZE];
                    DIR *directory;
                    struct dirent *entry;

                    sprintf(file_path, "mailboxes/%s", context->user->name);

                    directory = opendir(file_path);
                    if(directory == NULL){
                        return;
                    }

                    int file_count = 0;
                    while((entry = readdir(directory)) != NULL){
                        if(entry->d_type != DT_REG || strcmp(entry->d_name, METADATA_FILE) == 0) continue;
                        file_count++;
                    }
                    closedir(directory);

                    sendCmd(context->client_fd, "OK %d messages", file_count);

                    directory = opendir(file_path);
                    if (directory == NULL){
                        return;
                    }

                    while((entry = readdir(directory)) != NULL){
                        if(entry->d_type != DT_REG || strcmp(entry->d_name, METADATA_FILE) == 0) continue;

                        int id = atoi(entry->d_name);
                        char from[NAME_LEN], subject[SMTP2_MAXSIZE], date[TIMESTAMP_MAXSIZE];

                        char mail_path[DIR_PATH_MAXSIZE];
                        sprintf(mail_path, "mailboxes/%s/%d.txt", context->user->name, id);

                        FILE *file_ptr = fopen(mail_path, "r");
                        if (file_ptr == NULL){
                            return;
                        }

                        char line_buf[BUFSIZE];

                        fgets(line_buf, BUFSIZE, file_ptr); // From
                        line_buf[strcspn(line_buf, "\n")] = 0;
                        strcpy(from, line_buf + 6);

                        fgets(line_buf, BUFSIZE, file_ptr); // To --- We ignore this one

                        fgets(line_buf, BUFSIZE, file_ptr); // Subject
                        line_buf[strcspn(line_buf, "\n")] = 0;
                        strcpy(subject, line_buf + 9);

                        fgets(line_buf, BUFSIZE, file_ptr); // Date
                        line_buf[strcspn(line_buf, "\n")] = 0;
                        strcpy(date, line_buf + 6);            

                        sendCmd(context->client_fd, "%d\t%s\t%s\t%s", id, from, subject, date);

                        fclose(file_ptr);
                    }
                    sendCmd(context->client_fd, ".");

                    closedir(directory);

                }else if(strcmp(parsed_msg.command, "READ") == 0){

                    int id = atoi(parsed_msg.payload);

                    char file_path[DIR_PATH_MAXSIZE];
                    FILE *file_ptr = NULL;
                    
                    sprintf(file_path, "mailboxes/%s/%d.txt", context->user->name, id);
                    
                    file_ptr = fopen(file_path, "r");

                    if(file_ptr == NULL) {
                        sendCmd(context->client_fd, "ERR No such message");
                        break;
                    }

                    printLog("User %s READ message %d\n", context->user->name, id);
                    sendCmd(context->client_fd, "OK");

                    char send_buf[SMP_MAXSIZE];
                    while(fgets(send_buf, sizeof(send_buf), file_ptr)){
                        send_buf[strcspn(send_buf, "\n")] = 0;
                        sendCmd(context->client_fd, send_buf);
                    }
                    sendCmd(context->client_fd, ".");

                }else if(strcmp(parsed_msg.command, "DELETE") == 0){
                    
                    int id = atoi(parsed_msg.payload);
                    char file_path[DIR_PATH_MAXSIZE];

                    sprintf(file_path, "mailboxes/%s/%d.txt", context->user->name, id);

                    if(remove(file_path) == 0){
                        printLog("User %s DELETE message %d\n", context->user->name, id);
                        sendCmd(context->client_fd, "OK Deleted");
                    }else{
                        sendCmd(context->client_fd, "ERR No such message");
                    }


                }else if(strcmp(parsed_msg.command, "COUNT") == 0){

                    char file_path[DIR_PATH_MAXSIZE];
                    DIR *directory;
                    struct dirent *entry;
                    
                    sprintf(file_path, "mailboxes/%s", context->user->name);
                    directory = opendir(file_path);
                    
                    int file_count = 0;
                    if (directory != NULL) {
                        while((entry = readdir(directory)) != NULL){
                            if(entry->d_type == DT_REG && strcmp(entry->d_name, METADATA_FILE) != 0) file_count++;
                        }
                        closedir(directory);
                    }
                    
                    sendCmd(context->client_fd, "OK %d", file_count);

                }else if(strcmp(parsed_msg.command, "QUIT") == 0){

                    sendCmd(context->client_fd, "BYE");
                    context->state = SMP_FINISH;
                    close(context->client_fd);

                }else{

                    sendCmd(context->client_fd, "ERR Unknown command");

                }
            }
            break;

        case SMP_FINISH:
            {
                // Cleanup
            }
            break;
    }
}

void handleInterrupt(int sig){
    close(listen_fd);
    exit(0);
}


int main(int argc, char const *argv[]){
    if(argc != 3){
        printf("Usage: \n");
        printf("\t%s <port> <userfile>\n", argv[0]);
        return 1;
    }
    signal(SIGINT, handleInterrupt);

    port = atoi(argv[1]);

    // print_user_list();

    listen_fd = createTcpConnection();
    struct sockaddr_in *server_addr = createAddress("", port);

    if(bind(listen_fd, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0){
        perror("bind");
        exit(1);
    };

    if(listen(listen_fd, MAX_CLIENTS) < 0){
        perror("listen"); 
        exit(1);
    }

    printLog("Server started on port %s\n", argv[1]);

    init(argv[2]);

    while(1){

        fd_set readfds;
        FD_ZERO(&readfds);

        FD_SET(listen_fd, &readfds);
        int max_fd = listen_fd;


        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = clients[i].fd;
            if (clients[i].status != CONN_FREE) {
                FD_SET(fd, &readfds);
            }
            if (fd > max_fd) max_fd = fd;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        select(max_fd + 1, &readfds, NULL, NULL, &tv);

        time_t current_time = time(NULL);
        for(int i = 0; i < MAX_CLIENTS; i++){
            if(clients[i].status == CONN_CONNECTED){
                if(difftime(current_time, clients[i].connect_time) >= 30){
                    printLog("Connection %d timed out after 30 seconds.\n", clients[i].fd);
                    close(clients[i].fd);
                    clients[i].status = CONN_FREE;
                }
            }
        }

        struct sockaddr_in client_addr;
        int len = sizeof(client_addr);

        if (FD_ISSET(listen_fd, &readfds)) {
            int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, (socklen_t *)&len);
            if (client_fd < 0)
                continue;
            for(int i = 0; i < MAX_CLIENTS; i++){
                if (clients[i].status == CONN_FREE){
                    clients[i].status = CONN_CONNECTED;
                    clients[i].fd = client_fd;
                    clients[i].connect_time = time(NULL);
                    printLog("New connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), client_addr.sin_port);
                    sendCmd(client_fd, "WELCOME SimpleMail v1.0");
                    break;
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!(clients[i].status != CONN_FREE && FD_ISSET(clients[i].fd, &readfds))) continue;

            char msg[PROTOCOL_MAXSIZE];
            int n = recvMsg(clients[i].fd, msg, sizeof(msg), "\r\n");

            if (n <= 0) {
                close(clients[i].fd);
                clients[i].status = CONN_FREE;
                continue;
            }

            switch (clients[i].status) {
                case CONN_FREE:
                    break;

                case CONN_CONNECTED:
                    {
                        // Check if msg is "MODE SEND" or "MODE RECV"
                        // Update status, initialize the specific FSM context, send OK.

                        protocol_message parsed_msg;
                        int client_fd = clients[i].fd;
                        parseMessage(msg, &parsed_msg);

                        if(strcmp(parsed_msg.command, "MODE") == 0){

                            if(strcmp(parsed_msg.payload, "SEND") == 0){

                                printLog("Client selected MODE SEND\n");
                                initSendContext(&clients[i].fsm.send_ctx, client_fd);
                                clients[i].fsm.send_ctx.state = SMTP2_FROM;
                                clients[i].status = CONN_SEND;
                                sendCmd(client_fd, "OK");

                            }else if(strcmp(parsed_msg.payload, "RECV") == 0){

                                printLog("Client selected MODE RECV\n");
                                initRetrieveContext(&clients[i].fsm.recv_ctx, client_fd);
                                clients[i].fsm.recv_ctx.state = SMP_INIT_AUTH;
                                clients[i].status = CONN_RECV;
                                sendCmd(client_fd, "OK");

                                handleRetrieve(&clients[i].fsm.recv_ctx, "");

                            }
                        }else if(strcmp(parsed_msg.command, "QUIT") == 0){

                            printLog("Sent bye\n");
                            sendCmd(client_fd, "BYE");
                            close(client_fd);
                            clients[i].status = CONN_FREE;
                            printLog("Client disconnected\n");
                            break;

                        }else
                            sendCmd(client_fd, "ERR Unknown mode");
                    }
                    break;

                case CONN_SEND:
                    {
                        protocol_message parsed_msg;
                        parseMessage(msg, &parsed_msg);
                        if(strcmp(parsed_msg.command, "QUIT") == 0){
                            printLog("Sent bye\n");
                            sendCmd(clients[i].fd, "BYE");
                            close(clients[i].fd);
                            clients[i].status = CONN_FREE;
                            printLog("Client disconnected\n");
                            break;
                        }

                        handleSend(&clients[i].fsm.send_ctx, msg);

                        if (clients[i].fsm.send_ctx.state == SMTP2_FINISH) {
                            handleSend(&clients[i].fsm.send_ctx, ""); 
                            clients[i].status = CONN_CONNECTED;
                        }
                    }
                    break;

                case CONN_RECV:
                    {
                        handleRetrieve(&clients[i].fsm.recv_ctx, msg);

                        if (clients[i].fsm.recv_ctx.state == SMP_FINISH) {
                            handleRetrieve(&clients[i].fsm.recv_ctx, ""); 
                            clients[i].status = CONN_FREE;
                        }

                    }
                    break;

                default:
                    break;
            } 
        }
    }

    return 0;
}
