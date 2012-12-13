#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

/* FreeBSD should #include <netinet/in.h> */
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <stdint.h>
#include <sys/epoll.h>

#define NUM_WORKERS 20
#define PORT_NUM (8080)
#define BACKLOG 600

struct worker_info {
  int efd; // epoll instance
};

void startWakeupThread(void);
void * wakeupThreadLoop(void *);
void acceptLoop(void);
void startWorkers(void);
void startWorkerThread(int);

int evfd = -1;
struct worker_info workers[NUM_WORKERS];

int main(void) {

  int res;
  void *thread_pointer;

  startWorkers();
  startWakeupThread();
  acceptLoop();

  return 0;
}

void startWorkers(void) {
  int i;
  int efd;
  for (i=0; i < NUM_WORKERS; i++) {
    efd = epoll_create1(0);
    if (efd==-1) {
      perror("worker epoll_create1");
      exit(-1);
    }
    workers[i].efd = efd;
  }

  for (i=0; i < NUM_WORKERS; i++) {
    startWorkerThread(i);
  }
}

void *workerLoop(void * arg) {
  int w = (int)(unsigned long) arg;
  printf("hello from worker %d\n", w);
  pthread_exit(NULL);
}

void startWorkerThread(int w) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, workerLoop, (void *)(unsigned long) w)) {
    perror("pthread_create");
    exit(-1);
  }
  return;
}

void acceptLoop(void)
{
	int sd;
	struct sockaddr_in addr;
	int alen = sizeof(addr);
	short port = PORT_NUM;
	int sock_tmp;
	int current_worker = 0; 
	struct epoll_event event;

        sd = socket(PF_INET, SOCK_STREAM, 0); 
        if (sd == -1) {
                printf("socket: error: %d\n",errno);
		exit(-1);
        }   

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        int optval = 1;
        setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

        if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
                printf("bind error: %d\n",errno);
		exit(-1);
        }   

        if (listen(sd, BACKLOG) == -1) {

                printf("listen error: %d\n",errno);
		exit(-1);
        }   

	while(1) {
	  sock_tmp = accept(sd, (struct sockaddr*)&addr, &alen);
	  if (sock_tmp == -1) {
	    printf("Error %d doing accept", errno);
	    exit(-1);
	  }
	  int flags = fcntl(sock_tmp, F_GETFL, 0);
	  if (flags < 0)
	    perror("Getting NONBLOCKING failed.\n");
	  if ( fcntl(sock_tmp, F_SETFL, flags | O_NONBLOCK ) < 0 )
	    perror("Setting NONBLOCKING failed.\n");

	  event.data.fd = sock_tmp;
	  event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	  epoll_ctl(workers[current_worker].efd, EPOLL_CTL_ADD, sock_tmp, &event);

	  current_worker++;
	}

}

void startWakeupThread(void) {
  pthread_t wait_thread;
  if (pthread_create(&wait_thread, NULL, wakeupThreadLoop, NULL) != 0) {
    perror("Thread creat failed.");
    exit(-1);
  }
}

void * wakeupThreadLoop(void * null) {

  int epfd;
  struct epoll_event event;
  struct epoll_event *events;
  int n;

  evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (evfd == -1) {
    perror("eventfd failed");
    exit(-1);
  }

  epfd = epoll_create1(0);
  events = calloc (1, sizeof event);
  event.data.fd = evfd;
  event.events = EPOLLIN;
  if (epoll_ctl (epfd, EPOLL_CTL_ADD, evfd, &event) == -1) {
    perror("epoll_ctl");
    exit(-1);
  }
  while(1) {
    n = epoll_wait(epfd, events, 1, -1);
  }
  return NULL;
}

