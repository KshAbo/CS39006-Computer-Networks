// 23CS30029 : Kshetrimayum Abo
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "global.c"

int port;
char ip[100];
int listen_fd;
int is_connected = 0;

client_state state;

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

void connectServer(){
    listen_fd = createTcpConnection();
    struct sockaddr_in *server_addr = createAddress(ip, port);

    if(connect(listen_fd, (struct sockaddr*)server_addr, sizeof(struct sockaddr)) < 0){
        perror("connect");
        exit(1);
    };

    char recv_buf[RECV_BUFFER_SIZE];
    recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
    printf("%s\n", recv_buf);

    is_connected = 1;
}

int isExpectedResponse(char* res){
    char protocol_resp[SMTP2_MAXSIZE];
    recvMsg(listen_fd, protocol_resp, sizeof(protocol_resp), "\r\n");
    if(strcmp(protocol_resp, res) == 0) return 1;
    else return 0;
}

void closeSession() {
    sendCmd(listen_fd, "QUIT");
    
    char recv_buf[SMTP2_MAXSIZE];
    protocol_message parsed_msg;
    
    recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
    parseMessage(recv_buf, &parsed_msg);

    is_connected = 0;
    
    if(strcmp(parsed_msg.command, "BYE") == 0) {
        close(listen_fd);
    } else {
        close(listen_fd); 
    }
}

void quit(){
    printf("Quit\n");
    sendCmd(listen_fd, "QUIT");
    
    char recv_buf[BUFSIZE];
    protocol_message parsed_msg;
    recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
    parseMessage(recv_buf, &parsed_msg);
    printf("Received: %s\n", parsed_msg.command);

    if(strcmp(parsed_msg.command, "BYE") == 0)
        exit(0);
};

void sendMail(){

    char protocol_buf[PROTOCOL_MAXSIZE];
    protocol_message parsed_msg;

    sendCmd(listen_fd, "MODE SEND");
    int n = recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");
    if (n <= 0) {
        printf("\n[Error] The server closed the connection (30-second timeout).\n\n");
        is_connected = 0;
        state = CLIENT_FREE;
        return;
    }
    parseMessage(protocol_buf, &parsed_msg);

    if(strcmp(parsed_msg.command, "OK") != 0) {
        printf("Unsuccessful in sending mail\n");
        return;
    }


    // From part
    char name[NAME_LEN];
    printf("From (your name): ");
    char c;
    while((c = getchar()) != '\n' && c != EOF);
    fgets(name, NAME_LEN, stdin);
    name[strcspn(name, "\n")] = 0;

    sendCmd(listen_fd, "FROM %s", name);

    recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");

    if(strcmp(protocol_buf, "OK Sender accepted\r\n") != 0) {
        printf("Failed\n");
        return;
    }
    
    // To part
    while(1){
        printf("To (recipient username, empty line to finish): ");
        char recipient_name[NAME_LEN];
        fgets(recipient_name, NAME_LEN, stdin);
        recipient_name[strcspn(recipient_name, "\n")] = 0;  // Replace newline with null

        if(strlen(recipient_name) == 0) break;
        
        sendCmd(listen_fd, "TO %s", recipient_name);

        char protocol_resp_inner[SMTP2_MAXSIZE];
        recvMsg(listen_fd, protocol_resp_inner, sizeof(protocol_resp_inner), "\r\n");
        if(strcmp(protocol_resp_inner, "OK Recipient accepted\r\n") != 0){
            printf("\t-> Error: user '%s' does not exists on this server.\n", recipient_name);
        }else{
            printf("\t-> Recipient '%s' accepted.\n", recipient_name);
        }
    }

    // Sub part
    printf("Subject: ");
    char subject[SMTP2_MAXSIZE];
    fgets(subject, SMTP2_MAXSIZE, stdin);
    subject[strcspn(subject, "\n")] = 0;

    sendCmd(listen_fd, "SUB %s", subject);

    recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");
    parseMessage(protocol_buf, &parsed_msg);
    if(strcmp(parsed_msg.command, "OK") != 0) {
        printf("SUBJECT FAILED: %s\n", protocol_buf);
        return;
    }


    // Body part
    sendCmd(listen_fd, "BODY");
    recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");
    parseMessage(protocol_buf, &parsed_msg);
    if(strcmp(parsed_msg.command, "OK") != 0) {
        printf("BODY FAILED: %s\n", protocol_buf);
        return;
    }

    printf("Body (type '.' on a line by itself to finish):\n");
    char body[SMTP2_MAXSIZE];
    while(strcmp(fgets(body, SMTP2_MAXSIZE, stdin), ".\n") != 0){
        strcpy(protocol_buf, body);
        protocol_buf[strcspn(protocol_buf, "\n")] = 0;
        if (protocol_buf[0] == '.') {
            sendCmd(listen_fd, ".%s", protocol_buf); 
        } else {
            sendCmd(listen_fd, "%s", protocol_buf);
        }
    }
    sendCmd(listen_fd, ".");

    recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");
    parseMessage(protocol_buf, &parsed_msg);

    if(strcmp(parsed_msg.command, "OK") != 0) printf("Failed to deliver message\n");
}

void checkMail(){

    char protocol_buf[PROTOCOL_MAXSIZE];
    protocol_message parsed_msg;

    sendCmd(listen_fd, "MODE RECV");
    int n = recvMsg(listen_fd, protocol_buf, sizeof(protocol_buf), "\r\n");
    if (n <= 0) {
        printf("\n[Error] The server closed the connection (30-second timeout).\n\n");
        is_connected = 0;
        state = CLIENT_FREE;
        return;
    }
    parseMessage(protocol_buf, &parsed_msg);

    if(strcmp(parsed_msg.command, "OK") != 0) {
        printf("Unsuccessful in checking mail\n");
        printf("%s %s\n", parsed_msg.command, parsed_msg.payload);
        return;
    }

    // Authentication
    char username[NAME_LEN];
    for(int i = 0; i <= 3; i++){

        if(i == 3){
            return;
        }

        char c;
        while((c = getchar()) != '\n' && c != EOF);

        printf("Username: ");
        fgets(username, NAME_LEN, stdin);
        username[strcspn(username, "\n")] = 0;

        printf("Password: ");
        char password[PASSWORD_LEN + NONCE_LEN];
        scanf("%s", password);

        char smp_msg[SMP_MAXSIZE];
        recvMsg(listen_fd, smp_msg, SMP_MAXSIZE, "\r\n");


        protocol_message parsed_msg;
        parseMessage(smp_msg, &parsed_msg);

        char requirement[20], nonce_buf[NONCE_LEN];
        sscanf(parsed_msg.payload, "%s %s", requirement, nonce_buf);

        strcat(password, nonce_buf);

        printf("Received nonce: %s\n", nonce_buf);

        unsigned long hash = djb2(password);

        sendCmd(listen_fd, "AUTH %s %lu", username, hash);

        char smp_resp[SMP_MAXSIZE];
        recvMsg(listen_fd, smp_resp, SMP_MAXSIZE, "\r\n");

        parseMessage(smp_resp, &parsed_msg);

        if(strcmp(parsed_msg.command, "OK") == 0) break;
    }


    int logout = 0;

    // Options
    while(!logout){

        char recv_buf[SMP_MAXSIZE];
        protocol_message parsed_msg;

        sendCmd(listen_fd, "COUNT");
        recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
        parseMessage(recv_buf, &parsed_msg);

        printf("Mailbox for %s (%s messages)\n", username, parsed_msg.payload);
        printf("1. List all messages\n");
        printf("2. Read a message\n");
        printf("3. Delete a message\n");
        printf("4. Logout\n");

        int resp;
        scanf("%d", &resp);

        if(!(1 <= resp && resp <= 4)) break;

        switch (resp) {
            case 1:     // List
                {

                    sendCmd(listen_fd, "LIST");
                    char recv_buf[SMP_MAXSIZE];
                    recvMsg(listen_fd, recv_buf, SMP_MAXSIZE, "\r\n");
                    protocol_message parsed_msg;
                    parseMessage(recv_buf, &parsed_msg);

                    if(strcmp(parsed_msg.command, "OK") != 0) {
                        printf("LIST FAILED\n");
                        break;
                    }

                    int msg_cnt;
                    sscanf(parsed_msg.payload, "%d", &msg_cnt);

                    printf("\n%-5s  %-20s  %-30s  %s\n", "ID", "From", "Subject", "Date");
                    printf("%-5s  %-20s  %-30s  %s\n", "--", "----", "-------", "----");

                    while(1){
                        recvMsg(listen_fd, recv_buf, SMP_MAXSIZE, "\r\n");

                        if(strcmp(recv_buf, ".\r\n") == 0) {
                            break;
                        }

                        recv_buf[strcspn(recv_buf, "\r")] = 0;

                        char *ptr = recv_buf;
                        char *id = strsep(&ptr, "\t");
                        char *from = strsep(&ptr, "\t");
                        char *subject = strsep(&ptr, "\t");
                        char *date = strsep(&ptr, "\t");

                        if(id && from && subject && date) {
                            printf("%-5s  %-20s  %-30s  %s\n", id, from, subject, date);
                        }
                    }
                    printf("\n");

                }
                break;
            case 2:     // Read
                {

                    int id;
                    printf("Enter message ID: ");
                    scanf("%d", &id);

                    sendCmd(listen_fd, "READ %d", id);

                    char recv_buf[SMP_MAXSIZE];
                    protocol_message parsed_msg;
                    recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
                    parseMessage(recv_buf, &parsed_msg);

                    if(strcmp(parsed_msg.command, "OK") != 0){
                        printf("READ FAILED\n");
                        break;
                    }

                    printf("\n");
                    while(1){
                        recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
                        if(strcmp(recv_buf, ".\r\n") == 0) break;

                        recv_buf[strcspn(recv_buf, "\r")] = 0;

                        printf("%s\n", recv_buf);
                    }
                    printf("\n");

                }
                break;
            case 3:     // Delete
                {

                    int id;
                    printf("Enter Message ID: ");
                    scanf("%d", &id);

                    sendCmd(listen_fd, "DELETE %d", id);

                    char recv_buf[SMP_MAXSIZE];
                    protocol_message parsed_msg;
                    recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
                    parseMessage(recv_buf, &parsed_msg);

                    if(strcmp(parsed_msg.command, "OK") == 0) printf("Message %d deleted\n", id);
                    else printf("DELETE FAILED\n");

                }
                break;
            case 4:     // Logout
                {

                    printf("Logged out\n");
                    closeSession();
                    state = CLIENT_FREE;
                    logout = 1;

                }
                break;
        }
    }


}


int main(int argc, char const *argv[]){
    if(argc != 3){
        printf("Usage: \n");
        printf("\t%s <server_ip> <port>\n", argv[0]);
        return 1;
    }

    strcpy(ip, argv[1]);
    port = atoi(argv[2]);

    state = CLIENT_FREE;

    // char recv_buf[RECV_BUFFER_SIZE];
    // recvMsg(listen_fd, recv_buf, sizeof(recv_buf), "\r\n");
    // printf("%s\n", recv_buf);

    while(1){

        if(!is_connected){
            connectServer();
        }

        printf("Connected to SimpleMail server\n");
        printf("1. Send a mail\n");
        printf("2. Check my mail\n");
        printf("3. Quit\n");

        int res;
        printf("> ");
        fflush(stdout);
        scanf("%d", &res);

        while(!(1 <= res && res <= 3)){
            printf("Try again (1 2 3)...\n> ");
            scanf("%d", &res);
        }



        switch(res){
        case 1:
            state = CLIENT_SEND;
            sendMail();
            break;
        case 2:
            if(state != CLIENT_FREE) {
                closeSession();
                connectServer();
            }
            state = CLIENT_RECV;
            checkMail();
            break;
        case 3:
            if(state == CLIENT_FREE) exit(0);
            quit();
            break;
        default:
            break;
        }
    }


    return 0;
}
