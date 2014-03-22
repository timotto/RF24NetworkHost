/*
 * RF24NetworkHost.h
 *
 *  Created on: Mar 21, 2014
 *      Author: tim
 */

#ifndef RF24NETWORKHOST_H_
#define RF24NETWORKHOST_H_

#include <pthread.h>

typedef struct s_tcpclient {
	int sockfd;
	pthread_t thread_id;
} t_tcpclient;

struct clientlist {
	t_tcpclient *client;
//	struct clientlist *prev;
	struct clientlist *next;
};

struct messagelist {
	char message[1024];
	struct messagelist *next;
};

#endif /* RF24NETWORKHOST_H_ */
