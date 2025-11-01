/*
**
** Benchmark suite for echo server suing `epoll` and `io_uring` with `multishot`.
**
**
**
*/

#include <arpa/inet.h>
#include <asm-generic/errno.h>
#include <asm-generic/socket.h>
#include <assert.h>
#include <bits/time.h>
#include <errno.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PORT 9999
#define MAX_CONN 1000
#define BUFFER_SIZE 4096
#define MAX_EVENTS 128
#define SEC_NS 1000000000LL

/*
**
** Metrics recorded per benchmark.
**
*/
typedef struct {
  unsigned long long total_bytes;
  unsigned long long total_messages;
  unsigned long long connections_accepted;
  unsigned long long connections_closed;
  struct timespec start_time;
  struct timespec last_report_time;
} metrics_t;

metrics_t metrics = {0};
volatile sig_atomic_t running = 1;

/*
**
** Modes available for the server (epoll/uring/uring + multishot).
**
*/
typedef enum {
  MODE_EPOLL,
  MODE_URING,
  MODE_URING_MULTISHOT,
} server_mode_t;

/*
**
** Operations dispatched for io_uring (application defined).
**
*/
typedef enum {
  OP_ACCEPT,
  OP_READ,
  OP_WRITE,
} op_type_t;

/*
**
** Queue request messages with optional buffer ids for multishot.
**
*/
typedef struct {
  op_type_t type;
  int fd;
  char *buffer;
  size_t len;
  int buffer_id;
} request_t;

/*
**
** Returns a timestamp timespec as nanoseconds.
**
*/
static inline long long get_ns(struct timespec *ts) {
  return (long long)ts->tv_sec * SEC_NS + ts->tv_nsec;
}

/*
**
** Signal handler for shutdown.
**
*/
void sigint_handler(int sig) {
  (void)sig;
  running = 0;
}

/*
**
** Print metrics to stdout.
**
*/
void print_metrics(int force) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  long long elapsed_ns = get_ns(&now) - get_ns(&metrics.last_report_time);

  if (!force && elapsed_ns < SEC_NS) {
    return;
  }

  long long total_elapsed_ns = get_ns(&now) - get_ns(&metrics.start_time);
  double total_elapsed_sec = total_elapsed_ns / 1e9;
  // double interval_sec = elapsed_ns / 1e9;

  double total_throughput_mbps =
      (metrics.total_bytes * 8.0) / (total_elapsed_sec * 1000000.0);
  double total_msg_rate = metrics.total_messages / total_elapsed_sec;

  printf("\r[%.1fs] Connections: %llu active, %llu total | "
         "Messages: %llu (%.0f msg/s) | "
         "Throughput: %.2f Mb/s (%.2f MB/s) | "
         "Total: %.2f MB",
         total_elapsed_sec,
         metrics.connections_accepted - metrics.connections_closed,
         metrics.connections_accepted, metrics.total_messages, total_msg_rate,
         total_throughput_mbps, total_throughput_mbps / 8.0,
         metrics.total_bytes / (1024.0 * 1024.0));

  fflush(stdout);

  metrics.last_report_time = now;
}

/*
**
** Set socket to non blocking.
**
*/
int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
**
** Set socket option to TPC_NO_DELAY.
**
*/
void set_tcp_nodelay(int fd) {
  int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

/*
**
** Create a listener on the port.
**
*/
int create_listening_socket(int port) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket: couldn't create listener file descriptor");
    return -1;
  }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = INADDR_ANY,
  };

  // Bind to listener address.
  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    return -1;
  }

  // Listen.
  if (listen(listen_fd, 512) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }

  return listen_fd;
}

/*
**
** epoll based server.
**
*/
typedef struct {
  int fd;
  char buffer[BUFFER_SIZE];
  size_t bytes_read;
} epoll_conn_t;

void run_epoll_server(int port) {
  int listen_fd = create_listening_socket(port);
  if (listen_fd < 0) {
    exit(1);
  }

  set_nonblocking(listen_fd);

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    exit(1);
  }

  struct epoll_event ev = {
      .events = EPOLLIN,
      .data.fd = listen_fd,
  };

  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

  struct epoll_event events[MAX_EVENTS];
  epoll_conn_t *connections[MAX_CONN] = {0};

  printf("EPOLL server listening on port %d\n", port);

  while (running) {
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);

    for (int i = 0; i < nfds; i++) {
      int fd = events[i].data.fd;
      if (fd == listen_fd) {
        // Accept new connections.
        while (1) {
          struct sockaddr_in client_addr;
          socklen_t addr_len = sizeof(client_addr);

          int client_fd =
              accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);

          if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
              perror("accept");
            }
            break;
          }

          set_nonblocking(client_fd);
          set_tcp_nodelay(client_fd);

          epoll_conn_t *conn = malloc(sizeof(epoll_conn_t));
          conn->fd = client_fd;
          conn->bytes_read = 0;
          connections[client_fd] = conn;

          struct epoll_event ev = {
              .events = EPOLLIN | EPOLLET,
              .data.fd = client_fd,
          };
          epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

          metrics.connections_accepted++;
        }
      } else {
        // Handle client I/O.
        epoll_conn_t *conn = connections[fd];
        if (!conn)
          continue;

        if (events[i].events & EPOLLIN) {
          while (1) {
            ssize_t n = recv(fd, conn->buffer + conn->bytes_read,
                             BUFFER_SIZE - conn->bytes_read, 0);

            if (n > 0) {
              conn->bytes_read += n;
              metrics.total_bytes += n;

              // Echo.
              ssize_t sent = send(fd, conn->buffer, conn->bytes_read, 0);
              if (sent > 0) {
                metrics.total_messages++;
                conn->bytes_read = 0;
              }
            } else if (n == 0) {
              // connection closed, cleanup.
              epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
              close(fd);
              free(conn);
              connections[fd] = NULL;
              metrics.connections_closed++;
              break;
            } else {
              if (errno != EAGAIN && errno != EWOULDBLOCK) {
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                free(conn);
                connections[fd] = NULL;
                metrics.connections_closed++;
              }

              break;
            }
          }
        }
      }
    }

    print_metrics(0);
  }

  printf("\n");
  print_metrics(1);

  // Clean up.
  for (int i = 0; i < MAX_CONN; i++) {
    if (connections[i]) {
      close(connections[i]->fd);
      free(connections[i]);
    }
  }

  close(epoll_fd);
  close(listen_fd);
}

/*
**
** io_uring (single shot).
**
*/
void run_uring_server(int port) {
  int listen_fd = create_listening_socket(port);
  if (listen_fd < 0) {
    exit(1);
  }

  struct io_uring ring;
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));

  if (io_uring_queue_init_params(256, &ring, &params) < 0) {
    perror("io_uring_queue_init_params");
    exit(1);
  }

  printf("IO_URING server listening on port %d\n", port);

  // Submit initial accept.
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  request_t *req = malloc(sizeof(request_t));
  req->type = OP_ACCEPT;
  req->fd = listen_fd;
  req->buffer = NULL;

  io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);

  while (running) {
    // Completion queue.
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe_timeout(
        &ring, &cqe,
        &(struct __kernel_timespec){.tv_sec = 0, .tv_nsec = 100000000});

    if (ret == -ETIME) {
      print_metrics(0);
      continue;
    }

    if (ret < 0) {
      fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
      break;
    }

    request_t *req = io_uring_cqe_get_data(cqe);
    int res = cqe->res;

    if (req->type == OP_ACCEPT) {
      if (res >= 0) {
        // Mark connection as accepted
        int client_fd = res;
        set_tcp_nodelay(client_fd);
        metrics.connections_accepted++;

        // Submit read for the new connection.
        sqe = io_uring_get_sqe(&ring);

        request_t *read_req = malloc(sizeof(request_t));
        read_req->type = OP_READ;
        read_req->fd = client_fd;
        read_req->buffer = malloc(BUFFER_SIZE);
        read_req->len = BUFFER_SIZE;

        io_uring_prep_recv(sqe, client_fd, read_req->buffer, BUFFER_SIZE, 0);
        io_uring_sqe_set_data(sqe, read_req);

        // Submit another accept for the next connection.
        sqe = io_uring_get_sqe(&ring);

        request_t *accept_req = malloc(sizeof(request_t));
        accept_req->type = OP_ACCEPT;
        accept_req->fd = listen_fd;
        accept_req->buffer = NULL;

        io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
        io_uring_sqe_set_data(sqe, accept_req);

        io_uring_submit(&ring);
      }
    } else if (req->type == OP_READ) {
      if (res > 0) {
        metrics.total_bytes += res;
        metrics.total_messages++;

        // Echo.
        sqe = io_uring_get_sqe(&ring);
        request_t *write_req = malloc(sizeof(request_t));
        write_req->type = OP_WRITE;
        write_req->fd = req->fd;
        write_req->buffer = req->buffer;
        write_req->len = res;

        io_uring_prep_send(sqe, req->fd, write_req->buffer, res, 0);
        io_uring_sqe_set_data(sqe, write_req);
        io_uring_submit(&ring);

        free(req);
      } else {
        // Connection closed or errored out, cleanup.
        close(req->fd);
        free(req->buffer);
        free(req);
        metrics.connections_closed++;
      }
    } else if (req->type == OP_WRITE) {
      // After write completion, submit another read.
      if (res > 0) {
        sqe = io_uring_get_sqe(&ring);
        request_t *read_req = malloc(sizeof(request_t));
        read_req->type = OP_READ;
        read_req->fd = req->fd;
        read_req->buffer = req->buffer;
        read_req->len = BUFFER_SIZE;

        io_uring_prep_recv(sqe, req->fd, read_req->buffer, BUFFER_SIZE, 0);
        io_uring_sqe_set_data(sqe, read_req);
        io_uring_submit(&ring);
      } else {
        close(req->fd);
        free(req->buffer);
        metrics.connections_closed++;
      }

      free(req);
    }

    io_uring_cqe_seen(&ring, cqe);
    print_metrics(0);
  }

  printf("\n");
  print_metrics(1);

  io_uring_queue_exit(&ring);
  close(listen_fd);
}

/*
**
** io_uring (multishot).
**
*/

#define BUFFER_RING_SIZE 256
#define BUFFER_GROUP_ID 1

/*
**
** Buffer ring management API.
**
*/
typedef struct {
  void *buf_base;
  struct io_uring_buf_ring *br;
  size_t buf_size;
  int buf_count;
} buffer_group_t;

/*
**
** Creates and registers buffer ring, alternative to `provide_buffers`.
**
*/
static buffer_group_t *create_buffer_ring(struct io_uring *ring, int bgid,
                                          int buf_count, size_t buf_size) {
  buffer_group_t *bg = calloc(1, sizeof(buffer_group_t));
  if (!bg) {
    return NULL;
  }

  bg->buf_size = buf_size;
  bg->buf_count = buf_count;

  // Allocate contiguous buffer memory
  size_t total_size = buf_size * buf_count;
  if (posix_memalign(&bg->buf_base, 4096, total_size)) {
    free(bg);
    return NULL;
  }
  memset(bg->buf_base, 0, total_size);

  // Allocate buffer ring structure
  size_t ring_size = sizeof(struct io_uring_buf) * buf_count;
  void *ring_mem;
  if (posix_memalign(&ring_mem, 4096, ring_size)) {
    free(bg->buf_base);
    free(bg);
    return NULL;
  }
  memset(ring_mem, 0, ring_size);

  // Register buffer ring with io_uring
  struct io_uring_buf_reg reg = {
      .ring_addr = (unsigned long)ring_mem,
      .ring_entries = buf_count,
      .bgid = bgid,
  };

  int ret = io_uring_register_buf_ring(ring, &reg, 0);
  if (ret) {
    fprintf(stderr, "Failed to register buffer ring: %s\n", strerror(-ret));
    free(ring_mem);
    free(bg->buf_base);
    free(bg);
    return NULL;
  }

  bg->br = (struct io_uring_buf_ring *)ring_mem;

  // Add all buffers to the ring
  for (int i = 0; i < buf_count; i++) {
    void *buf_addr = (char *)bg->buf_base + (i * buf_size);
    io_uring_buf_ring_add(bg->br, buf_addr, buf_size, i,
                          io_uring_buf_ring_mask(buf_count), i);
  }
  io_uring_buf_ring_advance(bg->br, buf_count);

  return bg;
}

static void *get_buffer(buffer_group_t *bg, int buf_id) {
  return (char *)bg->buf_base + (buf_id * bg->buf_size);
}

static void return_buffer(buffer_group_t *bg, int buf_id) {
  void *buf_addr = get_buffer(bg, buf_id);
  io_uring_buf_ring_add(bg->br, buf_addr, bg->buf_size, buf_id,
                        io_uring_buf_ring_mask(bg->buf_count), 0);
  io_uring_buf_ring_advance(bg->br, 1);
}

static void free_buffer_ring(struct io_uring *ring, buffer_group_t *bg,
                             int bgid) {
  if (!bg)
    return;
  io_uring_unregister_buf_ring(ring, bgid);
  if (bg->br)
    free(bg->br);
  if (bg->buf_base)
    free(bg->buf_base);
  free(bg);
}

void run_uring_multishot_server(int port) {
  int listen_fd = create_listening_socket(port);
  if (listen_fd < 0) {
    exit(1);
  }

  struct io_uring ring;
  struct io_uring_params params;
  memset(&params, 0, sizeof(params));

  if (io_uring_queue_init_params(256, &ring, &params) < 0) {
    perror("io_uring_queue_init_params");
    exit(1);
  }

  // FIX #1: Use buffer rings instead of provide_buffers.
  //
  // Buffer rings are more efficient and the recommended approach for multishot.
  buffer_group_t *bg =
      create_buffer_ring(&ring, BUFFER_GROUP_ID, BUFFER_RING_SIZE, BUFFER_SIZE);
  if (!bg) {
    fprintf(stderr, "Failed to create buffer ring\n");
    exit(1);
  }

  printf("io_uring multishot server listening on port %d\n", port);

  // Submit multishot accept
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  request_t *req = malloc(sizeof(request_t));
  req->type = OP_ACCEPT;
  req->fd = listen_fd;

  io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
  io_uring_sqe_set_data(sqe, req);
  io_uring_submit(&ring);

  while (running) {
    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe_timeout(
        &ring, &cqe,
        &(struct __kernel_timespec){.tv_sec = 0, .tv_nsec = 100000000});

    if (ret == -ETIME) {
      print_metrics(0);
      continue;
    }

    if (ret < 0) {
      fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-ret));
      break;
    }

    request_t *req = io_uring_cqe_get_data(cqe);
    int res = cqe->res;

    if (!req) {
      io_uring_cqe_seen(&ring, cqe);
      print_metrics(0);
      continue;
    }

    // FIX #2: Handle errors properly before processing
    if (res < 0) {
      if (res == -ENOBUFS) {
        fprintf(stderr, "Buffer pool exhausted!\n");
      }

      // Clean up on error
      if (req->type == OP_WRITE && req->buffer) {
        free(req->buffer);
      }
      if (req->type != OP_ACCEPT) {
        free(req);
      }
      io_uring_cqe_seen(&ring, cqe);
      continue;
    }

    if (req->type == OP_ACCEPT) {
      int client_fd = res;
      set_tcp_nodelay(client_fd);
      metrics.connections_accepted++;

      // Init multishot recv for this connection
      sqe = io_uring_get_sqe(&ring);
      request_t *recv_req = malloc(sizeof(request_t));
      recv_req->type = OP_READ;
      recv_req->fd = client_fd;
      recv_req->buffer = NULL;

      io_uring_prep_recv_multishot(sqe, client_fd, NULL, 0, 0);
      sqe->flags |= IOSQE_BUFFER_SELECT;
      sqe->buf_group = BUFFER_GROUP_ID;

      io_uring_sqe_set_data(sqe, recv_req);
      io_uring_submit(&ring);

      // FIX #3: Only re-arm accept if multishot stopped
      // Your original code had the logic inverted
      if (!(cqe->flags & IORING_CQE_F_MORE)) {
        sqe = io_uring_get_sqe(&ring);
        io_uring_prep_multishot_accept(sqe, listen_fd, NULL, NULL, 0);
        io_uring_sqe_set_data(sqe, req);
        io_uring_submit(&ring);
      }

    } else if (req->type == OP_READ) {
      // Extract buffer ID from CQE flags
      int buffer_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
      char *data = get_buffer(bg, buffer_id);

      metrics.total_bytes += res;
      metrics.total_messages++;

      // FIX #4: Use async send instead of blocking send()
      // Allocate a copy of the data for async send
      sqe = io_uring_get_sqe(&ring);
      request_t *write_req = malloc(sizeof(request_t));
      write_req->type = OP_WRITE;
      write_req->fd = req->fd;
      write_req->buffer = malloc(res);
      write_req->len = res;
      memcpy(write_req->buffer, data, res);

      io_uring_prep_send(sqe, req->fd, write_req->buffer, res, 0);
      io_uring_sqe_set_data(sqe, write_req);
      io_uring_submit(&ring);

      // KEY FIX #5: Return buffer immediately after copying
      // Don't wait for send to complete
      return_buffer(bg, buffer_id);

      // Check if multishot recv continues
      if (!(cqe->flags & IORING_CQE_F_MORE)) {
        close(req->fd);
        free(req);
        metrics.connections_closed++;
      }

    } else if (req->type == OP_WRITE) {
      // Send completed, free the copied buffer
      if (req->buffer) {
        free(req->buffer);
      }
      free(req);
    }

    io_uring_cqe_seen(&ring, cqe);
    print_metrics(0);
  }

  printf("\n");
  print_metrics(1);

  free_buffer_ring(&ring, bg, BUFFER_GROUP_ID);
  io_uring_queue_exit(&ring);
  close(listen_fd);
}

void help(const char *prog) {
  printf("Usage: %s [-m mode] [-p port]\n", prog);
  printf("  -m mode: epoll, uring, multishot (default: epoll)\n");
  printf("  -p port: port number (default :%d)\n", PORT);
}

int main(int argc, char **argv) {
  server_mode_t mode = MODE_EPOLL;
  int port = PORT;

  // Parse arguments.
  int opt;
  while ((opt = getopt(argc, argv, "m:p:h")) != -1) {
    switch (opt) {
    case 'm':
      if (strcmp(optarg, "epoll") == 0) {
        mode = MODE_EPOLL;
      } else if (strcmp(optarg, "uring") == 0) {
        mode = MODE_URING;
      } else if (strcmp(optarg, "multishot") == 0) {
        mode = MODE_URING_MULTISHOT;
      } else {
        fprintf(stderr, "Invalid mode; %s\n", optarg);
        help(argv[0]);
        exit(1);
      }
      break;
    case 'p':
      port = atoi(optarg);
      break;
    case 'h':
      help(argv[0]);
      exit(0);
    default:
      help(argv[0]);
      exit(1);
    }
  }

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  clock_gettime(CLOCK_MONOTONIC, &metrics.start_time);
  metrics.last_report_time = metrics.start_time;

  switch (mode) {
  case MODE_EPOLL:
    run_epoll_server(port);
    break;
  case MODE_URING:
    run_uring_server(port);
    break;
  case MODE_URING_MULTISHOT:
    run_uring_multishot_server(port);
    break;
  }

  return 0;
}
