#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include <RF24.h>
#include <RF24Network.h>

#include "RF24NetworkHost.h"

#define DLEN 24

int log_d(const char *format, ...);
void *connection_handler(void *);
void *rf24_handler(void *);
void *tcptx_handler(void *);

void msgput(struct messagelist * msg, struct messagelist ** box, pthread_mutex_t * mutex);
struct messagelist * msgget(struct messagelist ** box, pthread_mutex_t * mutex);
struct messagelist * mkmsg();

int parsehexmsg(char* hexmsg, char* buffer);

struct clientlist *clients = NULL;
struct messagelist *outbox = NULL;
struct messagelist *inbox = NULL;

pthread_mutex_t clientslistmutex, rf24outboxmutex, rf24inboxmutex, tcptxmutex;
pthread_cond_t tcptxcond;
pthread_t rf24threadid, tcptxthreadid;

RF24 radio(BCM2835_SPI_CS0, RPI_V2_GPIO_P1_22, BCM2835_SPI_SPEED_8MHZ);
RF24Network network(radio);

int main(int argc, char** argv) {

	int server_socket, client_socket, c;
	struct sockaddr_in server, client;
	pthread_attr_t attr;

	radio.begin();
	radio.setDataRate(RF24_250KBPS);
	radio.setRetries(7,7);
	delay(5);
	network.begin(90, 0);

	if (-1 == (server_socket = socket(AF_INET, SOCK_STREAM, 0))) {
		perror("socket");
		return 1;
	}

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(8765);
	int reuse = 1;

	if (0 > setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse))) {
		perror("setsockopt");
		return 1;
	}

	if (0 > bind(server_socket, (struct sockaddr*)&server, sizeof(server))) {
		perror("bind");
		return 1;
	}

	if (-1 == listen(server_socket, 5)) {
		perror("listen");
		return 1;
	}

	c = sizeof(struct sockaddr_in);

	printf("listening\n");
	pthread_mutex_init(&clientslistmutex, NULL);
	pthread_mutex_init(&rf24outboxmutex, NULL);
	pthread_mutex_init(&rf24inboxmutex, NULL);
	pthread_mutex_init(&tcptxmutex, NULL);
	pthread_cond_init(&tcptxcond, NULL);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	if (
			(pthread_create(&rf24threadid, &attr, rf24_handler, NULL) < 0)
			|| (pthread_create(&tcptxthreadid, &attr, tcptx_handler, NULL) < 0)) {
		perror("some thread");
	} else {
		while((client_socket = accept(server_socket, (struct sockaddr*)&client, (socklen_t*)&c))) {
			printf("connection accepted\n");

			t_tcpclient *client = (t_tcpclient*)malloc(sizeof(t_tcpclient));
			if (client == NULL) {
				perror("malloc t_tcpclient");
				break;
			}
			client->sockfd = client_socket;

			if (pthread_create(&(client->thread_id), &attr, connection_handler, (void*)client) < 0) {
				perror("pthread_create");
				break;
			}

			struct clientlist * newclient = (struct clientlist*)malloc(sizeof(struct clientlist));
			if (newclient == NULL) {
				perror("malloc newclient");
				break;
			}
			newclient->client = client;
			newclient->next = NULL;

			pthread_mutex_lock(&clientslistmutex);
			struct clientlist * prev = clients;
			if (prev == NULL) {
				// 1.!
				clients = newclient;
			} else {
				while (prev->next != NULL) {
					prev = prev->next;
				}
				prev->next = newclient;
			}
			pthread_mutex_unlock(&clientslistmutex);

			printf("new client thread started\n");
		}
	}
	pthread_attr_destroy(&attr);
	pthread_mutex_destroy(&clientslistmutex);
	pthread_mutex_destroy(&rf24outboxmutex);
	pthread_mutex_destroy(&rf24inboxmutex);
	pthread_mutex_destroy(&tcptxmutex);
	pthread_cond_destroy(&tcptxcond);

	if (client_socket < 0) {
		perror("accept");
		return 1;
	}
	close(server_socket);
	return 0;
}

void *rf24_handler(void *arg) {
	struct messagelist * msg;
	struct messagelist * rf24in;

	rf24in = mkmsg();
	if (rf24in != NULL) {
		for(;;) {
			network.update();
			//log_d("rf24_handler 1\n");
			while (network.available()) {
				log_d("rf24_handler 3 >\n");
				RF24NetworkHeader header;
				network.peek(header);
				network.read(header, &(rf24in->message[6]), sizeof(rf24in->message) - 6);
				rf24in->message[0] = (header.from_node >> 8) & 0xff;
				rf24in->message[1] = (header.from_node) & 0xff;
				rf24in->message[2] = (header.to_node >> 8) & 0xff;
				rf24in->message[3] = (header.to_node) & 0xff;
				rf24in->message[4] = 0;
				rf24in->message[5] = (header.type) & 0xff;

				msgput(rf24in, &inbox, &rf24inboxmutex);

				pthread_mutex_lock(&tcptxmutex);
				pthread_cond_signal(&tcptxcond);
				pthread_mutex_unlock(&tcptxmutex);

				if ((rf24in = mkmsg()) == NULL) {
					break;
				}
				log_d("rf24_handler 3 <\n");
			}

			if ((msg = msgget(&outbox, &rf24outboxmutex)) != NULL) {
				log_d("rf24_handler 2\n");
				RF24NetworkHeader header;
				header.from_node = msg->message[0] >> 8 | msg->message[1];
				header.to_node = msg->message[2] >> 8 | msg->message[3];
				header.type = msg->message[5];

				network.write(header, &(msg->message[6]), DLEN);
				// TODO send to RF24
				//printf("rf24 send [%s]\n", msg->message);
				free(msg);
			}

			// sleep not so much
			delay(100);
		}
	}
	if (rf24in != NULL)
		free(rf24in);

	pthread_exit(NULL);
}

void *tcptx_handler(void *arg) {
	struct messagelist * msg;
	char txbuf[1024];
	int i;

	for(;;) {
		log_d("tcptx_handler 1\n");

		msg = msgget(&inbox, &rf24inboxmutex);
		if (msg != NULL) {
			log_d("tcptx_handler 2\n");
			snprintf(txbuf, sizeof(txbuf), "%02x%02x %02x%02x %02x",
					msg->message[0], msg->message[1], msg->message[2], msg->message[3], msg->message[5]);
			for(i=0;i<DLEN;i++)
				snprintf(&(txbuf[12+(3*i)]), sizeof(txbuf) - 12 - (3*i), " %02x", msg->message[6+i]);

			snprintf(&(txbuf[12 + (3*DLEN)]), sizeof(txbuf) - (12 + (3*DLEN)), "\n");

			pthread_mutex_lock(&clientslistmutex);
			struct clientlist * c = clients;
			while (c != NULL) {
				log_d("tcptx_handler 3\n");
				write(c->client->sockfd, txbuf, strlen(txbuf));
				c = c->next;
			}
			pthread_mutex_unlock(&clientslistmutex);
			free(msg);
		} else {
			log_d("tcptx_handler 4\n");
			pthread_mutex_lock(&tcptxmutex);
			pthread_cond_wait(&tcptxcond, &tcptxmutex);
			pthread_mutex_unlock(&tcptxmutex);
		}
	}
	pthread_exit(NULL);
}

void *connection_handler(void *arg) {
	t_tcpclient *me = (t_tcpclient*)arg;
	int read_size;
	struct messagelist * newmessage;
	char rxbuf[1024];
	int from, to, type;

	if ((newmessage = mkmsg()) != NULL) {
		while( (read_size = recv(me->sockfd, rxbuf, sizeof(rxbuf), 0)) > 0) {
			log_d("connection_handler 1\n");
			rxbuf[read_size-1] = 0;
			printf("Client sent [%s]\n", rxbuf);

			if (parsehexmsg(rxbuf, newmessage->message) == 0) {
				log_d("forwarding message\n");
				msgput(newmessage, &outbox, &rf24outboxmutex);

				if ((newmessage = mkmsg()) == NULL)
					break;
			}

		}
	}

	if (newmessage != NULL)
		free(newmessage);

	pthread_mutex_lock(&clientslistmutex);
	struct clientlist *prev = NULL;
	struct clientlist *current = clients;
	while (current != NULL) {
		if (current->client == me) {
			if (prev == NULL) {
				// was head
				clients = current->next;
//				clients->prev = NULL;
			} else {
				prev->next = current->next;
			}
			free(current);

			break;
		}
		prev = current;
		current = current->next;
	}
	pthread_mutex_unlock(&clientslistmutex);

	if (read_size == 0) {
		log_d("client disconnected\n");
	} else {
		log_d("recv failed\n");
	}
	close(me->sockfd);

	free(me);

	pthread_exit(NULL);
}

void msgput(struct messagelist * msg, struct messagelist ** box, pthread_mutex_t * mutex) {
	pthread_mutex_lock(mutex);
	msg->next = NULL;
	struct messagelist * prev = *box;
	if (prev == NULL) {
		// 1.!
		*box = msg;
		log_d("1. msg\n");
	} else {
		while (prev->next != NULL) {
			prev = prev->next;
		}
		prev->next = msg;
		log_d("n. msg\n");
	}
	pthread_mutex_unlock(mutex);
}

struct messagelist * msgget(struct messagelist ** box, pthread_mutex_t * mutex) {
	pthread_mutex_lock(mutex);
	struct messagelist * msg;

	/*
	msg = *box;
	int i=0;
	while (msg != NULL) {
		i++;
		msg = msg->next;
	}
	printf("box [%d] has %d\n", box, i);
	*/

	msg = *box;
	if (msg != NULL)
		*box = msg->next;

	pthread_mutex_unlock(mutex);
	return msg;
}

struct messagelist * mkmsg() {
	struct messagelist * ret = (struct messagelist*)malloc(sizeof(struct messagelist));
	if (ret == NULL) {
		perror("malloc mkmsg");
	} else {
		ret->next = NULL;
		memset(ret->message, 0, sizeof(ret->message));
	}
	return ret;
}

int parsehexmsg(char* hexmsg, char* buffer) {
	char *start, *end;
	int i, from, to, type;
	memset(buffer, 0, DLEN + 6);

	start = hexmsg;
	from = strtol(start, &end, 16);
	if (start == end || end == 0) {
		log_d("parsehexmsg: fail from\n");
		return 1;
	}

	start = &(end[1]);
	to = strtol(start, &end, 16);
	if (start == end || end == 0) {
		log_d("parsehexmsg: fail to\n");
		return 1;
	}

	start = &(end[1]);
	type = strtol(start, &end, 16);
	if (start == end || end == 0) {
		log_d("parsehexmsg: fail type\n");
		return 1;
	}

	for(i=0;i<DLEN;i++) {
		log_d("parsehexmsg: data\n");
		start = &(end[1]);
		buffer[6+i] = strtol(start, &end, 16);
		if (start == end) {
			if (i == 0) {
				log_d("parsehexmsg: fail data\n");
				return 1;
			} else {
				break;
			}
		}
		if (end == 0)
			break;
	}
	buffer[0] = (from >> 8) & 0xff;
	buffer[1] = (from) & 0xff;
	buffer[2] = (to >> 8) & 0xff;
	buffer[3] = (to) & 0xff;
	buffer[5] = (type) & 0xff;

	printf("from: %04X to: %02X type: %02X\n\t", from, to, type);
	for(i=0;i<DLEN;i++)
		printf("%02x ", buffer[6 + i]);
	printf("\n");
	return 0;
}

int log_d(const char *format, ...)
{
    va_list args;
    va_start(args, format);

//    if(priority & PRIO_LOG)
            vprintf(format, args);

    va_end(args);
    return 0;
}
