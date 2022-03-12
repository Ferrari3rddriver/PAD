#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/types.h>
#include <signal.h>

#define MAX_CLIENTS_ACCEPTED 10
#define BUFFER_SIZE 1024

static _Atomic unsigned int clients_connected_count = 0;
static int uid = 10;

//Client structure
typedef struct { 
	struct sockaddr_in address;
	int sockfd;
	int uid;
	char name[32];
	char password[32]
} client_t;

client_t* clients[MAX_CLIENTS_ACCEPTED];

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

void str_overwrite_stdout() {
	printf("\r%s", "> ");

    fflush(stdout);
}

void str_trim_lf (char* arr, int length) {
  int i;
  for (i = 0; i < length; i++) { 
    if (arr[i] == '\n') {
      arr[i] = '\0';
      break;
    }
  }
}

void print_client_addr (struct sockaddr_in addr) {
    printf("%d.%d.%d.%d",
        addr.sin_addr.s_addr & 0xff,
        (addr.sin_addr.s_addr & 0xff00) >> 8,
        (addr.sin_addr.s_addr & 0xff0000) >> 16,
        (addr.sin_addr.s_addr & 0xff000000) >> 24);
}

//Add clients to queue
void queue_add_client (client_t* client_to_add) {
	pthread_mutex_lock (&clients_mutex);

	for(int i=0; i < MAX_CLIENTS_ACCEPTED; ++i){
		if(!clients[i]) {
			clients[i] = client_to_add;
			break;
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

//Remove clients fro  queue
void queue_remove_client (int uid) {
	pthread_mutex_lock(&clients_mutex);

	for(int i=0; i < MAX_CLIENTS_ACCEPTED; ++i) {
		if(clients[i]) {
			if(clients[i]->uid == uid) {
				clients[i] = NULL;
				break;
			}
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

//Sends message to all clients connected
void send_message (char* string_to_send, int uid) {
	pthread_mutex_lock(&clients_mutex);

	for(int i=0; i<MAX_CLIENTS_ACCEPTED; ++i) {
		if(clients[i]) {
			if(clients[i]->uid != uid){
				if(write (clients[i]->sockfd, string_to_send, strlen(string_to_send)) < 0) {
					
					perror("ERROR: write to descriptor failed");
					
					break;
				}
			}
		}
	}

	pthread_mutex_unlock(&clients_mutex);
}

//Handle communication with client
void* handle_client(void *arg) {
	char buff_out[BUFFER_SIZE];
	char name[32];
	char password[32];
	int leave_flag = 0;

	clients_connected_count++;
	client_t* cli = (client_t* ) arg;

	//Client name
	if((recv(cli->sockfd, name, 32, 0) <= 0 || strlen(name) <  2 || strlen(name) >= 32-1) || (recv(cli->sockfd, password, 32, 0) <= 0 || strlen(password) <  2 || strlen(password) >= 32-1)) {
		printf("Didn't enter the name or password.\n");
		
		leave_flag = 1;
	} else {
		strcpy(cli->name, name);
		strcpy(cli->password, password);
		sprintf(buff_out, "%s has joined\n", cli->name);
		printf("%s", buff_out);
		send_message(buff_out, cli->uid);
	}

	bzero(buff_out, BUFFER_SIZE);

	while(1) {
		if (leave_flag) {
			break;
		}

		int receive = recv(cli->sockfd, buff_out, BUFFER_SIZE, 0);

		if (receive > 0) {
			if(strlen(buff_out) > 0) {
				send_message(buff_out, cli->uid);

				str_trim_lf(buff_out, strlen(buff_out));

				printf("%s\n", buff_out);
			}
		} else if (receive == 0 || strcmp(buff_out, "exit") == 0) {
			sprintf(buff_out, "%s has left\n", cli->name);
			printf("%s", buff_out);
			send_message(buff_out, cli->uid);
			
			leave_flag = 1;
		} else {
			printf("ERROR: -1\n");
			
			leave_flag = 1;
		}

		bzero(buff_out, BUFFER_SIZE);
	}

    //Delete client from queue and yield thread
	close(cli->sockfd);
    queue_remove_client(cli->uid);
    
	free(cli);
    
	clients_connected_count--;
    pthread_detach(pthread_self());

	return NULL;
}

int main (int argc, char** argv) {
	
	if(argc != 2) {
		printf("Usage: %s <port>\n", argv[0]);
		return EXIT_FAILURE;
	}

	//localhost for simulation
	char *ip = "127.0.0.1";
	int port = atoi(argv[1]);
	
	int option = 1;
	int listenfd = 0, connfd = 0;

	struct sockaddr_in server_addr;
	struct sockaddr_in client_addr;
	
	pthread_t tid;

	//Socket settings
	//SOCK_STREAM for TCP connection
	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr(ip);
	server_addr.sin_port = htons(port);

	//Pipe singals are ignored
	signal(SIGPIPE, SIG_IGN);

	if(setsockopt(listenfd, SOL_SOCKET, (SO_REUSEPORT | SO_REUSEADDR), (char*)&option, sizeof(option)) < 0) {
		perror("ERROR: setsockopt failed");
    
		return EXIT_FAILURE;
	}

	//Binding
    if(bind(listenfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    	perror("ERROR: Socket binding failed");
    
		return EXIT_FAILURE;
    }

  	//Listening
    if (listen(listenfd, 10) < 0) {
    	perror("ERROR: Socket listening failed");
    
		return EXIT_FAILURE;
	}

	printf("=== WELCOME TO THE SERVER-MULTIPLE CLIENTS CHATROOM ===\n");

	while(1) {
		socklen_t client = sizeof(client_addr);
		connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client);

		//Max clients limit checking 
		if((clients_connected_count + 1) == MAX_CLIENTS_ACCEPTED){
			printf("Max clients reached. Rejected: ");
			
			print_client_addr(client_addr);
			printf(":%d\n", client_addr.sin_port);
			
			close(connfd);
			continue;
		}

		//Client settings
		client_t *cli = (client_t *)malloc(sizeof(client_t));
		cli->address = client_addr;
		cli->sockfd = connfd;
		cli->uid = uid++;

		//Client added to connected clients queue and thread forked
		queue_add_client(cli);
		pthread_create(&tid, NULL, &handle_client, (void*)cli);

		//Reduce CPU usage
		sleep(1);
	}

	return EXIT_SUCCESS;
}