#include <arpa/inet.h>
#include <bits/getopt_core.h>
#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 9999
#define DEFAULT_CONNECTIONS 100
#define DEFAULT_MESSAGE_SIZE 1024
#define DEFAULT_DURATION 30
#define SEC_NS 1000000000LL

typedef struct {
  unsigned long long messages_sent;
  unsigned long long messages_received;
  unsigned long long bytes_sent;
  unsigned long long bytes_received;
  unsigned long long errors;
} thread_state_t;

typedef struct {
  int thread_id;
  char *server_ip;
  int port;
  int num_connections;
  int message_size;
  int duration_sec;
  thread_state_t stats;
} thread_args_t;

volatile sig_atomic_t running = 1;

void sigint_handler(int sig) {
  (void)sig;
  running = 0;
}

static inline long long get_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * SEC_NS + ts.tv_nsec;
}

void set_tcp_nodelay(int fd) {
  int flag = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

int connect_to_server(const char *ip, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }

  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
  };

  if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
    close(fd);
    return -1;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }

  set_tcp_nodelay(fd);
  return fd;
}

void *worker_thread(void *arg) {
  thread_args_t *args = (thread_args_t *)arg;

  int *fds = malloc(sizeof(int) * args->num_connections);
  char *send_buf = malloc(args->message_size);
  char *recv_buf = malloc(args->message_size);

  if (!fds || !send_buf || !recv_buf) {
    fprintf(stderr, "Thread %d: Memory allocation failed\n", args->thread_id);
    return NULL;
  }

  for (int i = 0; i < args->message_size; i++) {
    send_buf[i] = 'A' + (i % 26);
  }

  printf("Thread %d: Connecting %d sockets...\n", args->thread_id,
         args->num_connections);
  for (int i = 0; i < args->num_connections; i++) {
    fds[i] = connect_to_server(args->server_ip, args->port);
    if (fds[i] < 0) {
      fprintf(stderr, "Thread %d: Failed to connect socket %d\n",
              args->thread_id, i);
      args->stats.errors++;
      fds[i] = -1;
    }
  }

  printf("Thread %d: Connected, starting ...\n", args->thread_id);
  long long start_time = get_ns();
  long long end_time = start_time + ((long long)args->duration_sec * SEC_NS);

  while (running && get_ns() < end_time) {
    for (int i = 0; i < args->num_connections; i++) {
      if (fds[i] < 0)
        continue;

      // Echo send.
      ssize_t sent = send(fds[i], send_buf, args->message_size, 0);
      if (sent < 0) {
        args->stats.errors++;
        close(fds[i]);
        fds[i] = -1;
        continue;
      }

      args->stats.messages_sent++;
      args->stats.bytes_sent += sent;

      // Echo receive.
      ssize_t total_received = 0;
      while (total_received < args->message_size) {
        ssize_t received = recv(fds[i], recv_buf + total_received,
                                args->message_size - total_received, 0);

        if (received <= 0) {
          args->stats.errors++;
          close(fds[i]);
          fds[i] = -1;
          break;
        }

        total_received += received;
      }

      if (total_received == args->message_size) {
        args->stats.messages_received++;
        args->stats.bytes_received += total_received;

        if (memcmp(send_buf, recv_buf, args->message_size) != 0) {
          fprintf(stderr, "Thread %d: Echo mismatch on socket %d\n",
                  args->thread_id, i);
          args->stats.errors++;
        }
      }
    }
  }

  for (int i = 0; i < args->num_connections; i++) {
    if (fds[i] >= 0) {
      close(fds[i]);
    }
  }

  free(fds);
  free(send_buf);
  free(recv_buf);

  printf("Thread %d: Finished\n", args->thread_id);
  return NULL;
}

void help(const char *prog) {
  printf("Usage: %s [options]\n", prog);
  printf("Options:\n");
  printf("  -s server_ip Server IP address (default: 127.0.0.1)\n");
  printf("  -p port Server port (default: %d)\n", DEFAULT_PORT);
  printf("  -c connections Number of connections per thread (default: %d)\n",
         DEFAULT_CONNECTIONS);
  printf("  -t threads Number of threads (default: 1)\n");
  printf("  -m size Message size in bytes (default: %d)\n",
         DEFAULT_MESSAGE_SIZE);
  printf("  -d duration Duration in seconds (default: %d)\n", DEFAULT_DURATION);
  printf("  -h Display this help message\n");
}

int main(int argc, char **argv) {
  char *server_ip = "127.0.0.1";
  int port = DEFAULT_PORT;
  int connections_per_thread = DEFAULT_CONNECTIONS;
  int num_threads = 1;
  int message_size = DEFAULT_MESSAGE_SIZE;
  int duration_sec = DEFAULT_DURATION;

  int opt;
  while ((opt = getopt(argc, argv, "s:p:c:t:m:d:h")) != -1) {
    switch (opt) {
    case 's':
      server_ip = optarg;
      break;
    case 'p':
      port = atoi(optarg);
      break;
    case 'c':
      connections_per_thread = atoi(optarg);
      break;
    case 't':
      num_threads = atoi(optarg);
      break;
    case 'm':
      message_size = atoi(optarg);
      break;
    case 'd':
      duration_sec = atoi(optarg);
      break;
    case 'h':
      help(argv[0]);
      exit(0);
    default:
      help(argv[0]);
      exit(1);
    }
  }

  if (message_size < 1 || message_size > 1024 * 1024) {
    fprintf(stderr, "Invalid message size: %d\n", message_size);
    exit(1);
  }

  signal(SIGINT, sigint_handler);
  signal(SIGTERM, sigint_handler);

  printf("=== Echo Server Benchmark ===\n");
  printf("[+] Server:                 %s:%d\n", server_ip, port);
  printf("[+] Threads:                %d\n", num_threads);
  printf("[+] Connections per thread: %d\n", connections_per_thread);
  printf("[+] Total connections:      %d\n",
         num_threads * connections_per_thread);
  printf("[+] Message size;           %d bytes\n", message_size);
  printf("[+] Duration:               %d seconds\n", duration_sec);
  printf("\n\n");

  pthread_t *threads = malloc(sizeof(pthread_t) * num_threads);
  thread_args_t *thread_args = malloc(sizeof(thread_args_t) * num_threads);

  long long start_time = get_ns();

  for (int i = 0; i < num_threads; i++) {
    thread_args[i].thread_id = i;
    thread_args[i].server_ip = server_ip;
    thread_args[i].port = port;
    thread_args[i].num_connections = connections_per_thread;
    thread_args[i].message_size = message_size;
    thread_args[i].duration_sec = duration_sec;
    memset(&thread_args[i].stats, 0, sizeof(thread_state_t));

    if (pthread_create(&threads[i], NULL, worker_thread, &thread_args[i]) !=
        0) {
      fprintf(stderr, "Failed to create thread %d\n", i);
      exit(1);
    }
  }

  for (int i = 0; i < num_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  long long end_time = get_ns();
  double elapsed_sec = (end_time - start_time) / 1e9;

  unsigned long long total_messages_sent = 0;
  unsigned long long total_messages_received = 0;
  unsigned long long total_bytes_sent = 0;
  unsigned long long total_bytes_received = 0;
  unsigned long long total_errors = 0;

  for (int i = 0; i < num_threads; i++) {
    total_messages_sent += thread_args[i].stats.messages_sent;
    total_messages_received += thread_args[i].stats.messages_received;
    total_bytes_sent += thread_args[i].stats.bytes_sent;
    total_bytes_received += thread_args[i].stats.bytes_received;
    total_errors += thread_args[i].stats.errors;
  }

  printf("\n=== Results ===\n");
  printf("Elapsed time: %.2f seconds\n", elapsed_sec);
  printf("\nMessages:\n");
  printf("  Sent:     %llu (%2.f msg/s)\n", total_messages_sent,
         total_messages_sent / elapsed_sec);
  printf("  Received: %llu (%.2f msg/s)\n", total_messages_received,
         total_messages_received / elapsed_sec);
  printf("  Errors:   %llu\n", total_errors);

  printf("\nThroughput (sent):\n");
  printf("  Bytes:    %llu (%.2f MB)\n)", total_bytes_sent,
         total_bytes_sent / (1024.0 * 1024.0));
  printf("  Rate:     %.2f MB/s (%.2f Mb/s)",
         (total_bytes_sent / elapsed_sec) / (1024.0 * 1024.0),
         (total_bytes_sent * 8.0 / elapsed_sec) / 1000000.0);

  printf("\nThroughput (received):\n");
  printf("  Bytes:    %llu (%.2f MB)\n)", total_bytes_received,
         total_bytes_received / (1024.0 * 1024.0));
  printf("  Rate:     %.2f MB/s (%.2f Mb/s)",
         (total_bytes_received / elapsed_sec) / (1024.0 * 1024.0),
         (total_bytes_received * 8.0 / elapsed_sec) / 1000000.0);

  printf("\nPer-Thread statistics\n");
  for (int i = 0; i < num_threads; i++) {
    printf("  Thread %d: %llu msg sent, %llu msg recv, %llu errors\n", i,
           thread_args[i].stats.messages_sent,
           thread_args[i].stats.messages_received, thread_args[i].stats.errors);
  }

  free(threads);
  free(thread_args);

  return 0;
}
