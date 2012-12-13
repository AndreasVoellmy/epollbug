
#DEFINE NUM_WORKERS 20

struct worker_info {
  int efd; // epoll instance
};

struct worker_info workers[NUM_WORKERS]

int main(void) {

  startWorkers();
  startWakeupThread();
  acceptLoop();

  return 0;
}
