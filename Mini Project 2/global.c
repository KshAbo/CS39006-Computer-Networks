// 23CS30029 : Kshetrimayum Abo
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MAX_CLIENTS 100
#define MAX_USERS 1024
#define NAME_LEN 21
#define PASSWORD_LEN 31
#define SMTP2_MAXSIZE 512
#define SMP_MAXSIZE 512
#define PROTOCOL_MAXSIZE 512
#define BUFSIZE 512
#define MAX_RECIPIENTS 100
#define MAX_SEND_SIZE 100000
#define MODE_MAXSIZE 20
#define NONCE_LEN 9
#define TIMESTAMP_MAXSIZE 80
#define DIR_PATH_MAXSIZE 256
#define RECV_BUFFER_SIZE 1024

const char * const METADATA_FILE = ".metadata";

typedef struct {
    char name[NAME_LEN];
    char password[PASSWORD_LEN];
    int message_count;
} user;

typedef struct {
    user users[MAX_USERS];
    int size;
} user_list;

typedef enum {
    SMTP2_INIT,
    SMTP2_FROM,
    SMTP2_TO_SUB,
    SMTP2_BODY,
    SMTP2_FINISH,
} smtp2_state;

typedef struct {
    smtp2_state state;
    int client_fd;
    char name[NAME_LEN];
    char recipients[MAX_RECIPIENTS][NAME_LEN];
    int recipient_count;
    char subject[SMTP2_MAXSIZE];
    char body[SMTP2_MAXSIZE * 128];
} smtp2_context;

typedef enum {
    SMP_INIT,
    SMP_INIT_AUTH,
    SMP_AUTH,
    SMP_MAILBOX_READY,
    SMP_FINISH,
} smp_state;

typedef struct {
    smp_state state;
    int client_fd;
    user* user;
    char current_nonce[9];
    int auth_attempts;
} smp_context;

typedef struct {
    char command[32];
    char payload[SMTP2_MAXSIZE];
} protocol_message;

typedef enum {
    CONN_FREE,       // Slot is empty
    CONN_CONNECTED,  // Just connected, waiting for MODE
    CONN_SEND,       // Doing SMTP2
    CONN_RECV        // Doing SMP
} connection_status;

typedef struct {
    int fd;
    connection_status status;
    time_t connect_time;

    union {
        smtp2_context send_ctx;
        smp_context recv_ctx;
    } fsm;
} client_connection;

typedef enum {
    CLIENT_FREE,
    CLIENT_SEND,
    CLIENT_RECV,
} client_state;


void parseMessage(char *raw_msg, protocol_message *msg) {
    msg->command[0] = '\0';
    msg->payload[0] = '\0';

    char *space = strchr(raw_msg, ' ');

    if (space != NULL) {
        int cmd_len = space - raw_msg;
        
        strncpy(msg->command, raw_msg, cmd_len);
        msg->command[cmd_len] = '\0';

        strcpy(msg->payload, space + 1);
        msg->payload[strcspn(msg->payload, "\r\n")] = '\0';
    } else {
        strcpy(msg->command, raw_msg);
        msg->command[strcspn(msg->command, "\r\n")] = '\0';
    }
}

void sendCmd(int sockfd, const char *format, ...) {
    char buffer[SMTP2_MAXSIZE];
    
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer) - 3, format, args);
    va_end(args);

    if (len > 0) {
        strcat(buffer, "\r\n");
        send(sockfd, buffer, strlen(buffer), 0);
    }
}

// Returns the number of bytes read, 0 on disconnect, or -1 on error/buffer full
int recvMsg(int sockfd, char *buffer, int n, const char* terminator) {
    int term_len = strlen(terminator);
    int total_received = 0;
    char c;

    buffer[0] = '\0';

    while (total_received < n - 1) { 
        
        int bytes_read = recv(sockfd, &c, 1, 0);

        if (bytes_read > 0) {
            buffer[total_received++] = c;
            buffer[total_received] = '\0';

            if (total_received >= term_len) {
                if (strcmp(&buffer[total_received - term_len], terminator) == 0) {
                    return total_received;
                }
            }
        } 
        else if (bytes_read == 0) {
            return (total_received > 0) ? total_received : 0; 
        } 
        else {
            return -1; 
        }
    }
    
    return -1; 
}

unsigned long djb2(const char *str){
    unsigned long hash = 5381;
    int c;
    while((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}
