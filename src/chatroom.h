#ifndef CHATROOM_H
#define CHATROOM_H

#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

int8_t new_connection(int fd);

#endif