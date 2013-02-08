/* 
Author: Andreas Voellmy
Description: This program is a variation on kqueueserver3.c and its purpose
is to test whether kqueue can support concurrent calls of kevent() on the same
kqueue, as long as the concurrent calls do not involve any of the same file
descriptors. This program is a minor variation of kqueueserver3 that causes
these concurrent (i.e. overlapping) calls to occur. It does this by the 
following change: after servicing all the requests on a socket and reaching
EAGAIN, it re-registers the socket on the next (numerically) kqueue. This
 will cause sockets to cycle through the different kqueues. More importantly, 
since each kqueue is being used by a distinct worker thread to retrieve 
events, this may cause concurrent calls on the same kqueue. 

Note that this program ensures that there can be concurrent calls of kevent() 
(either to register or retrieve events) that involve the same socket. 
 */


// (1) First make sure you update the EXPECTED_HTTP_REQUEST variable as instructed below.
// (2) Set NUM_WORKERS to the number of cores you have (or maybe more if you want?)
// (3) Set NUM_CLIENTS to be the argument given to -c of weighttp.
// (4) Compile with: gcc -O2 kqueueserver4.c -lpthread -Wall -o kqueueserver4
// (5) To run: ./kqueueserver4

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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/time.h>

// prototypes
void acceptLoop(void);
void startWorkers(void);
void startWorkerThread(int);
void *workerLoop(void *);
void receiveLoop(int, int, int, char []);
void setNonBlocking(int);
void socketCheck(void);

// constants
#define NUM_WORKERS 2
#define PORT_NUM (8080)
#define BACKLOG 600
#define MAX_EVENTS 500
#define NUM_CLIENTS 20 // 500 // comes from the -c argument of weighttp

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
  "Content-Length: 145\r\n"
  "Server: Fake\r\n"
  "Last-Modified: Mon, 09 Jul 2012 03:42:33 GMT\r\n"
  "Content-Type: text/html\r\n\r\n"
  "<html>\n<head>\n<title>Welcome to foo</title>\n</head>\n"
  "<body bgcolor=\"white\" text=\"black\">\n"
  "<center><h1>Welcome to foo</h1></center>\n</body>\n</html>\n";
size_t RESPONSE_LEN;

// global variables
int queue[NUM_WORKERS] = {[0 ... (NUM_WORKERS-1)] = -1};
int sockets[NUM_CLIENTS];
int socketAssignments[NUM_CLIENTS];
int socketRequestCounts[NUM_CLIENTS];

int main(void) {
  EXPECTED_RECV_LEN = strlen(EXPECTED_HTTP_REQUEST);
  RESPONSE_LEN = strlen(RESPONSE);
  acceptLoop();
  startWorkers();
  socketCheck();
  return 0;
}

void startWorkers(void) {
  int i;
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
  int epfd;
  int n;
  int i;
  int sock;
  struct kevent *events;
  char recvbuf[1000];
  int j;

  events = calloc (MAX_EVENTS, sizeof (struct kevent));

  if (-1==(epfd = kqueue())) {
    perror("worker kqueue");
    exit(-1);
  }
  queue[w] = epfd;
  for (j=0; j<NUM_CLIENTS; j++) {
    if (socketAssignments[j] == w) {
      struct kevent event;
      event.ident =  sockets[j];
      event.filter = EVFILT_READ ;
      event.flags = EV_ADD | EV_ONESHOT;
      kevent(epfd, &event, 1, NULL, 0, NULL);
    }
  }

  while(1) {
    n = kevent(epfd,NULL,0,events,MAX_EVENTS,NULL);
    for (i=0; i < n; i++) {
      sock = events[i].ident;
#ifdef SHOW_REQUEST
      int m;
      m = recv(sock, recvbuf, 200, 0);
      recvbuf[m]='\0';
      printf("http request: %s\n", recvbuf);
      exit(0);
#endif
      receiveLoop(sock, epfd, w, recvbuf);
    }
  }
  pthread_exit(NULL);
}

void
incSocketRequestCount(int sock) {
  int i;
  for (i=0; i < NUM_CLIENTS; i++) {
    if (sockets[i]==sock) {
      socketRequestCounts[i]++;
    }
  }
}

void receiveLoop(int sock, int epfd, int w, char recvbuf[]) {
  ssize_t m;
  int numSent;
  struct kevent event;

  while(1) {
    m = recv(sock, recvbuf, EXPECTED_RECV_LEN, 0);
    if (m==0) break;
    if (m > 0) {
      if (m == EXPECTED_RECV_LEN) {
	incSocketRequestCount(sock);
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
    } else {
      if (m==-1) {
	if (errno==EAGAIN) {  // re-arm the socket with epoll.
	  event.ident  = sock;
	  event.filter = EVFILT_READ;
	  event.flags  = EV_ADD | EV_ONESHOT;

	  /* qfd will be the kqueue that we register the socket
	     with. qfd will typically be the next worker (this 
	     works out to be the other worker when NUM_WORKER = 2), 
	     but, strictly speaking, it is possible that the next
	     worker's kqueue has not yet been initialized, so we check
	     for that case and in that case we just assign to the 
	     current kqueue.
	  */
	  int w_next = (w + 1) % NUM_WORKERS; 
	  int qfd = (queue[w_next]==-1) ? epfd : queue[w_next]; 
	  if (kevent(qfd, &event, 1, NULL, 0, NULL)) {
	    perror("rearm");
	    exit(-1);
	  }
	  printf("rearmed %d on %d\n", sock, w);
	  break;
	} else {
	  perror("recv");
	  exit(-1);
	}
      } else {
	perror("unexpected");
	exit(-1);
      }
    }
  }
}

void socketCheck() {
  int i, bytesAvailable;
  sleep(10);
  for (i = 0; i < NUM_CLIENTS; i++) {
    if (ioctl(sockets[i], FIONREAD, &bytesAvailable) < 0) {
      perror("ioctl");
      exit(-1);
    }
    if (bytesAvailable > 0) {
      printf("socket %d with index %d assigned to worker %d has %d bytes of data ready and completed %d requests\n",
	     sockets[i],
	     i,
	     socketAssignments[i],
	     bytesAvailable,
	     socketRequestCounts[i]);
    }
  }
}

void acceptLoop(void)
{
  int sd;
  struct sockaddr_in addr;
  socklen_t alen = sizeof(addr);
  short port = PORT_NUM;
  int sock_tmp;
  int current_worker = 0;
  int current_client = 0;
  int optval;
  int j;
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
  for (j=0; j<NUM_CLIENTS; j++) {
    if (-1 == (sock_tmp = accept(sd, (struct sockaddr*)&addr, &alen))) {
      printf("Error %d doing accept", errno);
      perror("accept");
      exit(-1);
    }
    sockets[current_client] = sock_tmp;
    setNonBlocking(sock_tmp);
    socketAssignments[current_client] = current_worker;
    printf("current_client: %d\n", current_client);
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
