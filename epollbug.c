// compile with
// gcc -O2 epollbug.c -lpthread -Wall

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
#include <sys/eventfd.h>
#include <stdint.h>
#include <sys/epoll.h>

// data types
struct worker_info {
  int efd; // epoll instance
};

// prototypes
void startWakeupThread(void);
void *wakeupThreadLoop(void *);
void acceptLoop(int);
void startWorkers(int);
void startWorkerThread(int);
void *workerLoop(void *);
void startSocketCheckThread(void);
void receiveLoop(int, int, char []);
void setNonBlocking(int);
void *socketCheck(void *);

// constants
#define MAX_NUM_WORKERS 60
#define PORT_NUM (8080)
#define BACKLOG 600
#define MAX_EVENTS 500
#define NUM_CLIENTS 500

// Define this and the program will print the request made
// by the http client and then exit.
// #define SHOW_REQUEST

// This makes the bug more likely to happen, but it can happen without this.
// #define READ_EVENT_FD

// This removes all features for debugging, and converts this program to an 
// simple, yet fast C-based HTTP server.
#define SHOW_PEAK_PERFORMANCE
#ifdef SHOW_PEAK_PERFORMANCE
  #undef READ_EVENT_FD
  #undef SHOW_REQUEST
#endif

// Fill this in with the http request that your
// weighttp client sends to the server. This is the
// request that I get.
char EXPECTED_HTTP_REQUEST[] =
  "GET / HTTP/1.1\r\nHost: 10.12.0.1:8080\r\n"
  "User-Agent: weighttp/0.3\r\nConnection: keep-alive\r\n\r\n";
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

#if !(defined SHOW_PEAK_PERFORMANCE)
int evfd = -1;
#endif

struct worker_info workers[MAX_NUM_WORKERS];
int sockets[NUM_CLIENTS];

int main(int argc, char *argv[]) {
  EXPECTED_RECV_LEN = strlen(EXPECTED_HTTP_REQUEST);
  RESPONSE_LEN = strlen(RESPONSE);
  
  if (argc != 2) {
    printf( "usage: %s #workers\n", argv[0] );
    return -1;
  }
  int numWorkers = atoi(argv[1]);
  if (numWorkers >= MAX_NUM_WORKERS) {
    printf("error: number of workers must be less than %d\n", MAX_NUM_WORKERS);
    return -1;
  }

  startWorkers(numWorkers);
#if !(defined SHOW_PEAK_PERFORMANCE)
  startWakeupThread();
  startSocketCheckThread();
#endif
  acceptLoop(numWorkers);
  return 0;
}

void startWorkers(int numWorkers) {
  int i;
  int efd;
  for (i=0; i < numWorkers; i++) {
    if (-1==(efd = epoll_create1(0))) {
      perror("worker epoll_create1");
      exit(-1);
    }
    workers[i].efd = efd;
  }

  for (i=0; i < numWorkers; i++) {
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
  struct epoll_event *events;
  char recvbuf[1000];

  events = calloc (MAX_EVENTS, sizeof (struct epoll_event));

  while(1) {
    n = epoll_wait(epfd, events, MAX_EVENTS, -1);
    for (i=0; i < n; i++) {
      sock = events[i].data.fd;
#ifdef SHOW_REQUEST
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
  struct epoll_event event;

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
#if !(defined SHOW_PEAK_PERFORMANCE)
	if (eventfd_write(evfd, 1)) {
	  perror("eventfd_write");
	  exit(-1);
	}
#endif
      } else {
	perror("partial recv");
	exit(-1);
      }
    }
    if (m==-1) {
      if (errno==EAGAIN) {
	// re-arm the socket with epoll.
	event.data.fd = sock;
	event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	if (epoll_ctl(epfd, EPOLL_CTL_MOD, sock, &event)) {
	  perror("rearm epoll_ctl");
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

#if !(defined SHOW_PEAK_PERFORMANCE)
void startWakeupThread(void) {
  pthread_t wait_thread;
  if (pthread_create(&wait_thread, NULL, wakeupThreadLoop, NULL) != 0) {
    perror("Thread create failed.");
    exit(-1);
  }
}

void * wakeupThreadLoop(void * null) {

  evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (evfd == -1) {
    perror("eventfd failed");
    exit(-1);
  }
#ifdef READ_EVENT_FD
  int epfd;
  struct epoll_event event;
  struct epoll_event *events;
  uint64_t val;
  int n;

  epfd = epoll_create1(0);
  events = calloc (1, sizeof event);
  event.data.fd = evfd;
  event.events = EPOLLIN;

  if (epoll_ctl (epfd, EPOLL_CTL_ADD, evfd, &event)) {
    perror("epoll_ctl");
    exit(-1);
  }
  while(1) {
    n = epoll_wait(epfd, events, 1, -1);
    if (n>0) {
      if (eventfd_read(evfd, &val)) {
	perror("eventfd_read");
	exit(-1);
      }
    }
  }
#else
  sleep(20);
#endif
  pthread_exit(NULL);
}
#endif

#if !(defined SHOW_PEAK_PERFORMANCE)
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
#endif

void acceptLoop(int numWorkers)
{
  int sd;
  struct sockaddr_in addr;
  struct epoll_event event;
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
      exit(-1);
    }
    sockets[current_client] = sock_tmp;
    setNonBlocking(sock_tmp);
    event.data.fd = sock_tmp;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(workers[current_worker].efd, EPOLL_CTL_ADD, sock_tmp, &event);
    current_client++;
    current_worker = (current_worker + 1) % numWorkers;
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
