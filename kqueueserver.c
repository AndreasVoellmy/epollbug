// (1) First make sure you update the EXPECTED_HTTP_REQUEST variable as instructed below.
// (2) Set NUM_WORKERS to the number of cores you have (or maybe more if you want?)
// (3) Compile with: gcc -O2 kqueueserver.c -lpthread -Wall -o kqueueserver
// (4) To run: ./kqueueserver


#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <stdint.h>

#include <sys/event.h>
#include <sys/time.h>

// data types
struct worker_info {
  int efd; // epoll instance
};

// prototypes
void acceptLoop(void);
void startWorkers(void);
void startWorkerThread(int);
void *workerLoop(void *);
void startSocketCheckThread(void);
void receiveLoop(int, int, char []);
void setNonBlocking(int);
void *socketCheck(void *);

// constants
#define NUM_WORKERS 2
#define PORT_NUM (8080)
#define BACKLOG 600
#define MAX_EVENTS 500
#define NUM_CLIENTS 500

// Fill this in with the http request that your
// weighttp client sends to the server. This is the 
// request that I get. You can also find out what weighttp
// is sending by first setting SHOW_REQUEST (see below) and leaving
// this as is. Then when you run the program it will print the request
// it received.
char EXPECTED_HTTP_REQUEST[] = 
  "GET / HTTP/1.1\r\nHost: 10.12.0.1:8080\r\n"
  "User-Agent: weighttp/0.3\r\nConnection: keep-alive\r\n\r\n";

// Define this and the program will print the request made 
// by the http client and then exit.
// #define SHOW_REQUEST 

int EXPECTED_RECV_LEN;

char RESPONSE[] = 
  "HTTP/1.1 200 OK\r\n"
  "Date: Tue, 09 Oct 2012 16:36:18 GMT\r\n"
  "Content-Length: 151\r\n"
  "Server: Mighttpd/2.8.1\r\n"
  "Last-Modified: Mon, 09 Jul 2012 03:42:33 GMT\r\n"
  "Content-Type: text/html\r\n\r\n"
  "<html>\n<head>\n<title>Welcome to nginx!</title>\n</head>\n"
  "<body bgcolor=\"white\" text=\"black\">\n"
  "<center><h1>Welcome to nginx!</h1></center>\n</body>\n</html>\n";
size_t RESPONSE_LEN;

// global variables
int evfd = -1;
struct worker_info workers[NUM_WORKERS];
int sockets[NUM_CLIENTS];

int main(void) {
  EXPECTED_RECV_LEN = strlen(EXPECTED_HTTP_REQUEST);
  RESPONSE_LEN = strlen(RESPONSE);
  startWorkers();
  startSocketCheckThread();
  acceptLoop();
  return 0;
}

void startWorkers(void) {
  int i;
  int efd;
  for (i=0; i < NUM_WORKERS; i++) {
    if (-1==(efd = kqueue())) {
      perror("worker kqueue");
      exit(-1);
    }
    workers[i].efd = efd;
  }

  for (i=0; i < NUM_WORKERS; i++) {
    startWorkerThread(i);
  }
}

void startWorkerThread(int w) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, workerLoop, (void *)(unsigned long) w)) {
    perror("pthread_create");
    exit(-1);
  }
  return;
}

void *workerLoop(void * arg) {
  int w = (int)(unsigned long) arg;
  int epfd = workers[w].efd;
  int n; 
  int i;
  int sock;
  struct kevent64_s *events;
  char recvbuf[1000];

  events = calloc (MAX_EVENTS, sizeof (struct kevent64_s));

  while(1) {
    n = kevent64(epfd,NULL,0,events,MAX_EVENTS,0,NULL);
    for (i=0; i < n; i++) {
      sock = events[i].ident;
#ifdef SHOW_REQUEST
      int m;
      m = recv(sock, recvbuf, 200, 0);
      recvbuf[m]='\0';
      printf("http request: %s\n", recvbuf);
      exit(0);
#endif
      receiveLoop(sock, epfd, recvbuf);
    }
  }
  pthread_exit(NULL);
}

void receiveLoop(int sock, int epfd, char recvbuf[]) {
  ssize_t m;
  int numSent;
  struct kevent64_s event;

  while(1) {
    m = recv(sock, recvbuf, EXPECTED_RECV_LEN, 0);
    if (m==0) break;
    if (m > 0) {
      if (m == EXPECTED_RECV_LEN) {
	numSent = send(sock, RESPONSE, RESPONSE_LEN, 0);
	if (numSent == -1) {
	  perror("send failed");
	  exit(-1);
	}
	if (numSent != RESPONSE_LEN) {
	  perror("partial send");
	  exit(-1);
	} 
      } else {
	perror("partial recv");
	exit(-1);
      }
    }
    if (m==-1) {
      if (errno==EAGAIN) {
	// re-arm the socket with epoll.
	event.ident = sock;
	event.filter = EVFILT_READ;
	event.flags = EV_ADD | EV_ONESHOT; 
	if (kevent64(epfd, &event, 1, NULL, 0, 0, NULL)) {
	  perror("rearm"); 
	  exit(-1);
	}
	break;
      } else {
	perror("recv");
	exit(-1);
      }
    }
  }
}

// Sleep for 10 seconds, then show the sockets which have data.
void startSocketCheckThread(void) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, socketCheck, (void *)NULL)) {
    perror("pthread_create");
    exit(-1);
  }
  return; 
}

void *socketCheck(void * arg) {
  int i, bytesAvailable; 
  sleep(10);
  for (i = 0; i < NUM_CLIENTS; i++) {
    if (ioctl(sockets[i], FIONREAD, &bytesAvailable) < 0) {
      perror("ioctl");
      exit(-1);
    }
    if (bytesAvailable > 0) {
      printf("socket %d has %d bytes of data ready\n", sockets[i], bytesAvailable);  
    }
  }
  pthread_exit(NULL);
}

void acceptLoop(void)
{
  int sd;
  struct sockaddr_in addr;
  struct kevent64_s event;
  socklen_t alen = sizeof(addr);
  short port = PORT_NUM;
  int sock_tmp;
  int current_worker = 0; 
  int current_client = 0;
  int optval;

  if (-1 == (sd = socket(PF_INET, SOCK_STREAM, 0))) {
    printf("socket: error: %d\n",errno);
    exit(-1);
  }   
  
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  
  optval = 1;
  setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);
  if (bind(sd, (struct sockaddr*)&addr, sizeof(addr))) {
    printf("bind error: %d\n",errno);
    exit(-1);
  }   
  if (listen(sd, BACKLOG)) {
    printf("listen error: %d\n",errno);
    exit(-1);
  }   
  while(1) {
    if (-1 == (sock_tmp = accept(sd, (struct sockaddr*)&addr, &alen))) {
      printf("Error %d doing accept", errno);
      perror("accept");
      exit(-1);
    }
    sockets[current_client] = sock_tmp;
    setNonBlocking(sock_tmp);
    event.ident = sock_tmp;
    event.filter = EVFILT_READ ;
    event.flags = EV_ADD | EV_ONESHOT;
    kevent64(workers[current_worker].efd, &event, 1, NULL, 0, 0, NULL);
    current_client++;    
    current_worker = (current_worker + 1) % NUM_WORKERS;
  }
}

void setNonBlocking(int fd) {
  int flags;
  if (-1 == (flags = fcntl(fd, F_GETFL, 0))) {
    perror("Getting NONBLOCKING failed.\n");
    exit(-1);
  }
  if ( fcntl(fd, F_SETFL, flags | O_NONBLOCK ) < 0 ) {
    perror("Setting NONBLOCKING failed.\n");
    exit(-1);
  }
  return;
}
