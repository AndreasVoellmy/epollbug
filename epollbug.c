#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

/* FreeBSD should #include <netinet/in.h> */
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>


#define NUM_WORKERS 20
#define PORT_NUM (8080)
#define BACKLOG 600

struct sock_var {
	int sk;
} __attribute__ ((aligned (64))); /* one cacheline */

struct sock_var socks[MAX_SOCK_NUM];


struct worker_info {
  int efd; // epoll instance
};

struct worker_info workers[NUM_WORKERS]

void acceptLoop( )
{
	int sd;
	struct sockaddr_in addr;
	int alen = sizeof(addr);
	short port = PORT_NUM;
	int sock_tmp;

        sd = socket(PF_INET, SOCK_STREAM, 0); 
        if (sd == -1) {
                printf("socket: error: %d\n",errno);
                return -1; 
        }   

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        int optval = 1;
        setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

        if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
                printf("bind error: %d\n",errno);
                return -1; 
        }   

        if (listen(sd, BACKLOG) == -1) {

                printf("listen error: %d\n",errno);
                return -1; 
        }   

	while(1) {
		sock_tmp = accept(sd, (struct sockaddr*)&addr, &alen);
		if (sock_tmp == -1) {
			printf("Error %d doing accept", errno);
			return -1;
		}
		int flags = fcntl(sock_tmp, F_GETFL, 0);
		if (flags < 0)
			perror("Getting NONBLOCKING failed.\n");
		if ( fcntl(sock_tmp, F_SETFL, flags | O_NONBLOCK ) < 0 )
			perror("Setting NONBLOCKING failed.\n");
	}

}




int main(void) {

  startWorkers();
  startWakeupThread();
  acceptLoop();

  return 0;
}
