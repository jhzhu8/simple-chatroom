#include <pthread.h>
#include <semaphore.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <semaphore.h>

#include "chatroom.h"
#define CONN_TIMEOUT_SECS   (30)

#define SEND_BUFF_LEN       (32)
#define MAX_MSG_SIZE        (20000)
#define JOIN_BUFF_SIZE      (100)
#define MIN_JOIN_MSG_LEN    (8)
#define MAX_NAME_LEN        (20)
#define SEM_PSHARE          (0) // semaphore within a process

typedef struct client_s client_t;

typedef enum {
    BROADCAST_MSG,
    ERROR_MSG,
    NUM_MSG_TYPE,
} msg_type_t;

typedef struct send_buff_item_s {
    char msg[MAX_MSG_SIZE];
    uint16_t size;
    msg_type_t type;
    client_t* client;     // ignored for broadcast msg
} send_buff_item_t;

typedef struct send_buff_s {
    send_buff_item_t buff[SEND_BUFF_LEN];
    pthread_mutex_t insertMutex; // only single consumer, so mutex protects writes to buff only
    sem_t empty;
    sem_t full;
    uint8_t insertIdx;
    uint8_t removeIdx;
} send_buff_t;

typedef struct client_s {
    char name[MAX_NAME_LEN+1]; // add space for :
    int fd;
    struct client_s* next;
    struct client_s* prev;
    pthread_t tid;
    uint8_t isActive;
    send_buff_t* sendBuff;
} client_t;

typedef struct chatroom_s {
    char name[MAX_NAME_LEN+1];
    send_buff_t sendBuff;
    pthread_mutex_t clientListMutex;
    client_t* clientList;
    client_t* clientListTail;
    pthread_t tid;
    struct chatroom_s* next;
    struct chatroom_s* prev;
} chatroom_t;


static chatroom_t* chatroom_list_head = NULL;
static chatroom_t* chatroom_list_tail = NULL;

static void close_chatroom(chatroom_t* room)
{
    sem_destroy(&room->sendBuff.empty);
    sem_destroy(&room->sendBuff.full);
    pthread_mutex_destroy(&room->clientListMutex);
    pthread_mutex_destroy(&room->sendBuff.insertMutex);
    free(room);
}

static void delete_client(client_t* client)
{
    close(client->fd);
    client->isActive = 0;
    free(client);
}

// room becomes invalidated
static void remove_chatroom(chatroom_t* room)
{
    if(room == NULL) {
        return;
    }

    if(room->prev == NULL && room->next == NULL) {
        // 1 element list
        chatroom_list_head = NULL;
        chatroom_list_tail = NULL;
    } else if(room->prev == NULL) {
        // room is pointing to head
        chatroom_list_head = room->next;
        room->next->prev = NULL;
    } else if(room->next == NULL) {
        // room is pointing to tail
        chatroom_list_tail = room->prev;
        room->prev->next = NULL;
    } else {
        room->prev->next = room->next;
        room->next->prev = room->prev;
    }

    close_chatroom(room);
}

// Return NULL if none found
static chatroom_t* find_chatroom(char* name)
{
    if(chatroom_list_head == NULL) {
        return NULL;
    }

    chatroom_t* it = chatroom_list_head;
    // insert new chatroom in list
    while(it != NULL) {
        // Check if active chatroom with matching name exists
        if(strcmp(name, it->name) == 0 && it->clientList != NULL) {
            // Found active chatroom
            break;
        }

        // If dormant chatroom, delete it
        if(it->clientList == NULL) {
            chatroom_t* remove = it;
            it = it->next;
            remove_chatroom(remove);
            close_chatroom(remove);
        } else {
            it = it->next;
        }
    }
    
    return it;
}

static void insert_left_room_msg(chatroom_t* room, char* name)
{
    if(sem_wait(&room->sendBuff.empty) < 0) {
        printf("ERROR: Failed to wait on empty sem room %s\n", room->name);
        return;
    }

    pthread_mutex_lock(&room->sendBuff.insertMutex);
    uint8_t idx = room->sendBuff.insertIdx;
    send_buff_item_t* item = &room->sendBuff.buff[idx];
    char* insert = item->msg;
    
    // Construct message "user: msg"
    strcpy(insert, name);
    insert += strlen(name);
    strcpy(insert, " has left\n");
    
    item->type = BROADCAST_MSG;
    item->size = strlen(item->msg);
    room->sendBuff.insertIdx = (idx+1)%SEND_BUFF_LEN;
    pthread_mutex_unlock(&room->sendBuff.insertMutex);

    if(sem_post(&room->sendBuff.full) < 0) {
        printf("ERROR: Failed to wait on empty sem room %s\n", room->name);
        return;
    }
}

// client becomes invalidated
static void remove_client(client_t* client, chatroom_t* room)
{
    if(client == NULL) {
        return;
    }

    if(client->prev == NULL && client->next == NULL) {
        // 1 element list
        room->clientList = NULL;
        room->clientListTail = NULL;
    } else if(client->prev == NULL) {
        // client is pointing to head
        room->clientList = client->next;
        client->next->prev = NULL;
    } else if(room->next == NULL) {
        // room is pointing to tail
        room->clientListTail = client->prev;
        client->prev->next = NULL;
    } else {
        client->prev->next = client->next;
        client->next->prev = client->prev;
    }

    // If there are remaining clients, send the "left room" msg
    if(room->clientList != NULL) {
        insert_left_room_msg(room, client->name);
    }

    delete_client(client);
}

void* chatroom_sender(void* input)
{
    chatroom_t* room = (chatroom_t*)input;

    while(1) {        
        // Pick up new messages from the buffer
        if(sem_wait(&room->sendBuff.full) < 0) {
            printf("ERROR: Failed to wait on empty sem client in room %s\n", room->name);
            return NULL;
        }
        uint8_t idx = room->sendBuff.removeIdx;
        send_buff_item_t* item = &room->sendBuff.buff[idx];

        if(item->type == ERROR_MSG) {
            if(item->client->isActive == 1) {
                // Send error message to specified client
                if(send(item->client->fd, item->msg, item->size, MSG_NOSIGNAL) != 0) {
                    int err = errno; 
                    printf("Failed to send error msg on socket to client in room %s with err=%d\n", 
                            room->name, err);
                    pthread_cancel(item->client->tid);
                    remove_client(item->client, room);
                }
            } else {
                remove_client(item->client, room);
            }
            
        } else if(item->type == BROADCAST_MSG) {
            // Iterate through the client list and broadcast to all clients
            pthread_mutex_lock(&room->clientListMutex);
            client_t* it = room->clientList;
            while(it != NULL) {
                if(it->isActive == 1) {
                    if(send(it->fd, item->msg, item->size, MSG_NOSIGNAL) < 0) {
                        int err = errno; 
                        printf("Failed to send msg on socket to client %s in room %s with err=%d\n", 
                            it->name, room->name, err);
                        // Close the client
                        pthread_cancel(it->tid);
                        remove_client(it, room);
                        it = it->next;
                        continue;
                    }
                } else {
                    // Client disconnected, delete it
                    remove_client(it, room);
                    it = it->next;
                    continue;
                }
                it = it->next;            
            }
            pthread_mutex_unlock(&room->clientListMutex);
        }
        
        room->sendBuff.removeIdx = (idx+1)%SEND_BUFF_LEN;
        if(sem_post(&room->sendBuff.empty) < 0) {
            printf("ERROR: Failed to wait on empty sem client in room %s\n", room->name);
            break;
        }

        // Check if there are remaining clients
        if(room->clientList == NULL) {
            // No more client, exit
            break;
        }
    }
    remove_chatroom(room);
    return NULL;
}

static int8_t insert_broadcast_msg(client_t* client, char* msg, uint32_t len, uint8_t appendName)
{
    if(len > (MAX_MSG_SIZE-1)-(strlen(client->name)+1)) { // -1 compensates for extra \n
        printf("ERROR: Message size too large from %s\n", client->name);
        return -1;
    }
    
    if(sem_wait(&client->sendBuff->empty) < 0) {
        printf("ERROR: Failed to wait on empty sem client %s\n", client->name);
        return -1;
    }

    pthread_mutex_lock(&client->sendBuff->insertMutex);
    uint8_t idx = client->sendBuff->insertIdx;
    send_buff_item_t* item = &client->sendBuff->buff[idx];
    char* insert = item->msg;
    
    
    // Construct message "user: msg"
    item->size = len;
    if(appendName) {
        size_t nameLen = strlen(client->name);
        strcpy(insert, client->name);
        insert += nameLen;
        *insert = ':';
        insert++;
        item->size += nameLen + 1;
    }    
    memcpy(insert, msg, len);

    // Check if ends in new line
    if(insert[len-1] != '\n') {
        insert[len] = '\n';
        item->size++;
    }
    
    item->type = BROADCAST_MSG;
    client->sendBuff->insertIdx = (idx+1)%SEND_BUFF_LEN;
    pthread_mutex_unlock(&client->sendBuff->insertMutex);

    if(sem_post(&client->sendBuff->full) < 0) {
        printf("ERROR: Failed to wait on empty sem client %s\n", client->name);
        return -1;
    }

    return 0;
}

static int8_t insert_error_msg(client_t* client, char* msg, uint32_t len)
{
    if(sem_wait(&client->sendBuff->empty) < 0) {
        printf("ERROR: Failed to wait on empty sem client %s\n", client->name);
        return -1;
    }

    pthread_mutex_lock(&client->sendBuff->insertMutex);
    uint8_t idx = client->sendBuff->insertIdx;
    send_buff_item_t* item = &client->sendBuff->buff[idx];

    strcpy(item->msg, msg);

    item->type = ERROR_MSG;
    item->size = len;
    item->client = client;
    client->sendBuff->insertIdx = (idx+1)%SEND_BUFF_LEN;
    pthread_mutex_unlock(&client->sendBuff->insertMutex);


    if(sem_post(&client->sendBuff->full) < 0) {
        printf("ERROR: Failed to wait on empty sem client %s\n", client->name);
        return -1;
    }

    return 0;
}

// Expects C-string input (with terminating null)
// Returns leftover bytes which are not full message
static int32_t tokenize_msg(client_t* client, char* buff, uint32_t len)
{
    uint32_t lenRemain = len;
    char* store = buff;
    char* token = strtok(buff, "\n");
    while(token != NULL) {
        size_t tokenSize = strlen(token);
        lenRemain -= tokenSize+1; // +1 for the NULL char inputted
        store += tokenSize+1;

        // Handle carriage return
        if(token[tokenSize-1] == '\r') {
            token[tokenSize-1] = '\0';
            tokenSize--;
        }
        
        if(insert_broadcast_msg(client, token, tokenSize, 1) != 0) {
            printf("Error: Failed to add broadcast msg to buffer for client %s\n", client->name);
            return -1;
        }        
        token = strtok(NULL, "\n");
    }

    // move the remaining chars to front of buffer
    if(store != buff) {
        memmove(buff, store, lenRemain);
    }

    return lenRemain;
}

void* chatroom_client(void* input)
{
    client_t* client = (client_t*)input;
    char recvBuff[MAX_MSG_SIZE];

    int joinMsgSize = sprintf(recvBuff, "%s has joined\n", client->name);
    if(joinMsgSize < 0) {
        printf("ERROR: Client %s failed to construct has joined msg\n", client->name);
        return NULL;
    }
    insert_broadcast_msg(client, recvBuff, joinMsgSize, 0); // do not include terminating null char
    
    ssize_t numBytes = 0;
    int32_t leftOver = 0;
    while(1) {
        // MAX_MSG_SIZE-1 so we can append null char
        numBytes = recv(client->fd, recvBuff+leftOver, (MAX_MSG_SIZE-1)-leftOver, 0);
        if(numBytes <= 0) {
            int err = errno;
            printf("ERROR: Client %s failed to recv with ret=%lu and err=%d\n", client->name, numBytes, err);
            char error_msg[] = "ERROR\n";
            insert_error_msg(client, error_msg, strlen(error_msg));
            client->isActive = 0;
            break;
        }
        uint32_t totalSize = leftOver+numBytes;        
        // Append null char
        recvBuff[totalSize] = '\0';
        //printf("Recvd: %s\n", recvBuff+leftOver);

        // Split recvd data into messages
        leftOver = tokenize_msg(client, recvBuff, totalSize);
        if(leftOver < 0) {
            // Message too long
            char error_msg[] = "ERROR\n";
            insert_error_msg(client, error_msg, strlen(error_msg));
            break;
        }
    }

    return NULL;
}

int8_t add_client(int fd, chatroom_t* room, char* name)
{
    if(room == NULL || fd < 0 || strlen(name) > MAX_NAME_LEN) {
        printf("ERROR: Invalid args to add_client for client %s\n", name);
        return -1;
    }

    client_t* newClient = (client_t*)calloc(1, sizeof(client_t));
    newClient->fd = fd;
    newClient->isActive = 1;
    newClient->prev = NULL;
    newClient->next = NULL;
    strcpy(newClient->name, name);
    newClient->sendBuff = &room->sendBuff;

    // Add to client list
    pthread_mutex_lock(&room->clientListMutex);
    if(room->clientList == NULL) {
        // Initializing client list
        room->clientList = newClient;
        room->clientListTail = newClient;
    } else {
        room->clientListTail->next = newClient;
        newClient->prev = room->clientListTail;
        room->clientListTail = newClient;
    }
    pthread_mutex_unlock(&room->clientListMutex);

    // Spawn thread
    if(pthread_create(&newClient->tid, NULL, chatroom_client, newClient)) {
        printf("ERROR: Failed to start client thread for %s\n", name);
        remove_client(newClient, room);
        return -1;
    }

    return 0;
}

static void add_chatroom(chatroom_t* room)
{
    if(room == NULL) {
        return;
    }

    pthread_mutex_lock(&room->clientListMutex);

    // Empty list
    if(chatroom_list_head == NULL) {
        chatroom_list_head = room;
        chatroom_list_tail = room;
        room->prev = NULL;
        room->next = NULL;
    } else {
        chatroom_list_tail->next = room;
        room->prev = chatroom_list_tail;
        chatroom_list_tail = room;
        room->next = NULL;
    }

    pthread_mutex_unlock(&room->clientListMutex);
}

// initialize chatroom
static chatroom_t* init_chatroom(int fd, char* name)
{
    // Add new room to room list
    chatroom_t* newRoom = (chatroom_t*)calloc(1, sizeof(chatroom_t));

    // Initialize mutexes and semaphores
    if(pthread_mutex_init(&newRoom->sendBuff.insertMutex, NULL) != 0) {
        printf("ERROR: Failed to initialize send buffer mutex %s\n", name);
        free(newRoom);
        return NULL;
    }
    
    if(pthread_mutex_init(&newRoom->clientListMutex, NULL) != 0) {
        printf("ERROR: Failed to initialize client list mutex %s\n", name);
        free(newRoom);
        return NULL;
    }

    if(sem_init(&newRoom->sendBuff.empty, SEM_PSHARE, SEND_BUFF_LEN) != 0) {
        printf("ERROR: Failed to initialize send buff empty sem %s\n", name);
        free(newRoom);
        return NULL;
    }
    
    if(sem_init(&newRoom->sendBuff.full, SEM_PSHARE, 0) != 0) {
        printf("ERROR: Failed to initialize send buff empty sem %s\n", name);
        free(newRoom);
        return NULL;
    }
    newRoom->sendBuff.removeIdx = 0;
    newRoom->sendBuff.insertIdx = 0;

    // Name
    strcpy(newRoom->name, name);

    // Start the sender thread
    if(pthread_create(&newRoom->tid, NULL, chatroom_sender, newRoom) != 0) {
        printf("ERROR: Failed to start chatroom %s sender thread\n", name);
        free(newRoom);
        return NULL;
    }

    // Add new room to list
    add_chatroom(newRoom);

    return newRoom;
}

// Add client to chatroom
// If name doesn't correspond, create new
static int8_t init_client(int fd, char* roomName, char* clientName)
{
    // Check name length
    if(strlen(roomName) > MAX_NAME_LEN || strlen(clientName) > MAX_NAME_LEN) {
        return -1;
    }
    
    chatroom_t* room = find_chatroom(roomName);
    if(room != NULL) {
        // Found active chatroom
        // Just add new client to it
        if(add_client(fd, room, clientName) != 0) {
            printf("Failed to add client %s to %s\n", clientName, roomName);
            return -1;
        }
    } else {
        // Need to create new chatroom
        room = init_chatroom(fd, roomName);
        if(room == NULL) {
            printf("ERROR: Failed to initialize chatroom %s\n", roomName);
            return -1;
        }

        // Add first client
        if(add_client(fd, room, clientName) != 0) {
            printf("Failed to initialize first client %s to %s\n", clientName, roomName);
            return -1;
        }
    }

    return 0;
}

// Tokenize
static int8_t parse_join_msg(char* msg, char** clientName, char** roomName)
{
    if(strlen(msg) < MIN_JOIN_MSG_LEN) {
        printf("ERROR: Join message too short\n");
        return -1;
    }

    char* token = strtok(msg, " ");
    if(token == NULL || strcmp(token, "JOIN") != 0) {
        printf("ERROR: Invalid message header %s\n", token);
        return -1;
    }

    token = strtok(NULL, " \r\n");
    if(token == NULL) {
        printf("ERROR: No room name present\n");
        return -1;
    }
    *clientName = token;
    printf("INFO: Got client name %s\n", *clientName);

    if(strlen(*clientName) > MAX_NAME_LEN ) {
        *clientName = NULL;
        *roomName = NULL;
        printf("Client name too long %s\n", *clientName);
        return -1;
    }

    token = strtok(NULL, " \r\n");
    if(token == NULL) {
        printf("INFO: Got room name %s\n", *roomName);
        *clientName = NULL;
        return -1;
    }
    *roomName = token;

    token = strtok(NULL, " \r\n");
    if(token != NULL) {
        printf("ERROR: Room name with space\n");
        *clientName = NULL;
        *roomName = NULL;
        return -1;
    }    

    if(strlen(*roomName) > MAX_NAME_LEN) {
        *clientName = NULL;
        *roomName = NULL;
        printf("Room name too long %s\n", *roomName);
        return -1;
    }

    //printf("Client: %s Room: %s\n", *clientName, *roomName);

    return 0;
}

static int8_t send_join_error_msg(int fd)
{
    char msg[] = "ERROR\n";

    if(send(fd, msg, strlen(msg), MSG_NOSIGNAL) != 0) {
        int err = errno;
        printf("Failed to send error msg in response to invalid join with err=%d\n", err);
        return -1;
    }

    return 0;
}

// New connection
int8_t new_connection(int fd)
{
    // Set a timeout for the first connection message
    struct timeval tv;
    tv.tv_sec = CONN_TIMEOUT_SECS;
    tv.tv_usec = 0;
    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) != 0) {
        int err = errno;
        printf("Failed to set timeout on fd %d with err=%d\n", fd, err);
        return -1;
    }

    // Receive first message
    ssize_t numBytes = 0;
    char buff[JOIN_BUFF_SIZE];
    numBytes = recv(fd, buff, JOIN_BUFF_SIZE-1, 0); // -1 so we can add null char
    if(numBytes <= 0) {
        int err = errno;
        printf("ERROR: Failed to receive join msg on fd %d with err=%d\n", fd, err);
        return -1;
    }

    // Append null char
    buff[numBytes] = '\0';

    // Extract room name and client name from message
    char* clientName = NULL;
    char* roomName = NULL;
    if(parse_join_msg(buff, &clientName, &roomName) != 0) {
        printf("ERROR: Invalid JOIN msg. Sending error and closing\n");
        send_join_error_msg(fd);
        return -1;
    }

    // Initialize the client connection
    if(init_client(fd, roomName, clientName) != 0) {
        printf("ERROR: Failed to init client. Discarding connection\n");
        send_join_error_msg(fd);
        return -1;
    }

    // Set no timeout for the subsequent messages
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv) != 0) {
        int err = errno;
        printf("ERROR: Failed to remove timeout on fd %d with err=%d\n", fd, err);
        return -1;
    }

    return 0;
}