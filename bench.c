#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define TARGET_HOST "127.0.0.1"
#define TARGET_PORT 12345
#define BENCH_COUNT 10
#define BENCHMARK_RESULT_FILE "bench.txt"

/* length of unique message (TODO below) should shorter than this */
#define MAX_MSG_LEN 32

/*
 * Too much concurrent connection would be treated as sort of DDoS attack
 * (mainly caused by configs (kernel: "tcp_max_syn_backlog" and
 * "somaxconn". Application (kecho): "backlog"). Nevertheless, default
 * maximum number of fd per-process is 1024. If you insist to proceed
 * the benchmarking with "MAX_THREAD" larger than these limitation,
 * perform following modifications:
 *
 * (1)
 * Use following commands to adjust kernel attributes:
 *     a. "$ sudo sysctl net.core.somaxconn={depends_on_MAX_THREAD}"
 *     b. "$ sudo sysctl net.ipv4.tcp_max_syn_backlog={ditto}"
 * Note that "$ sudo sysctl net.core.somaxconn" can get current value.
 * "somaxconn" is max amount of established connection, whereas
 * "tcp_max_syn_backlog" is max amount of connection at first step
 * of TCP 3-way handshake (SYN).
 *
 * (2)
 * Use command "$ ulimit -n {ditto}" to enlarge limitation of fd
 * per-process. Note that this modification only effects on process
 * which executes the command and its child processes.
 *
 * (3)
 * Specify "backlog" with value as large as "net.ipv4.tcp_max_syn_backlog".
 *
 * Remember to reset the modifications after benchmarking to keep
 * stability of running machine
 */
#define MAX_THREAD 1000

#define GENRAND64(X) (((X) &0x7F7F7F7F7F7F7F7F) | 0x2020202020202020)
#define GENRAND8(X) (((X) &0x7F) | 0x20)

#define DETECT_NULL(X) (((X) -0x0101010101010101) & ~(X) &0x8080808080808080)
#define DETECT_CHAR(X, MASK) (DETECT_NULL((X) ^ (MASK)))

static pthread_t pt[MAX_THREAD];

/* block all workers before they are all ready to benchmarking kecho */
static bool ready;

static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_wait = PTHREAD_COND_INITIALIZER;

static long time_res[MAX_THREAD] = {0};
static int idx = 0; /* for indexing "time_res" */
static FILE *bench_fd;

static void GenRandString(char *str)
{
    int size = (rand() & (MAX_MSG_LEN - 1)) + 1;
    uint64_t *lptr = (uint64_t *) str;

    while (size >= 8) {
        uint64_t rand64 = (uint64_t) rand() << 32 | rand();
        *lptr = GENRAND64(rand64);
        // 如果偵測到 DEL
        if (DETECT_CHAR(*lptr, 0x7F7F7F7F7F7F7F7F))
            continue;
        lptr++;
        size -= 8;
    }

    char *cptr = (char *) lptr;

    while (size) {
        *cptr = GENRAND8(rand());
        // 如果產生 DEL
        if (*cptr == 0x7F)
            continue;
        cptr++;
        size--;
    }
    *cptr = '\0';
}

static inline long time_diff_us(struct timeval *start, struct timeval *end)
{
    return ((end->tv_sec - start->tv_sec) * 1000000) +
           (end->tv_usec - start->tv_usec);
}

static void *bench_worker(__attribute__((unused)))
{
    int sock_fd;
    char msg_dum[MAX_MSG_LEN + 1];
    char dummy[MAX_MSG_LEN + 1];
    struct timeval start, end;

    // 產生隨機字串
    GenRandString(msg_dum);
    /* wait until all workers created */
    pthread_mutex_lock(&worker_lock);
    while (!ready)
        // 等待 worker_wait 被 broadcast 或是 signal
        if (pthread_cond_wait(&worker_wait, &worker_lock)) {
            puts("pthread_cond_wait failed");
            exit(-1);
        }
    pthread_mutex_unlock(&worker_lock);

    // 建立 socket descriptor
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        perror("socket");
        exit(-1);
    }

    struct sockaddr_in info = {
        .sin_family = PF_INET,
        .sin_addr.s_addr = inet_addr(TARGET_HOST),
        .sin_port = htons(TARGET_PORT),
    };

    // 建立連線
    if (connect(sock_fd, (struct sockaddr *) &info, sizeof(info)) == -1) {
        perror("connect");
        exit(-1);
    }

    gettimeofday(&start, NULL);
    // 送資料給 server
    send(sock_fd, msg_dum, strlen(msg_dum), 0);
    // 從 server 接收資料
    recv(sock_fd, dummy, MAX_MSG_LEN, 0);
    gettimeofday(&end, NULL);

    shutdown(sock_fd, SHUT_RDWR);
    close(sock_fd);

    // 比較接受回來的資料和原本的資料是否相同
    if (strncmp(msg_dum, dummy, strlen(msg_dum))) {
        puts("echo message validation failed");
        exit(-1);
    }

    // 計算經過時間
    pthread_mutex_lock(&res_lock);
    time_res[idx++] += time_diff_us(&start, &end);
    pthread_mutex_unlock(&res_lock);

    pthread_exit(NULL);
}

static void create_worker(int thread_qty)
{
    for (int i = 0; i < thread_qty; i++) {
        if (pthread_create(&pt[i], NULL, bench_worker, NULL)) {
            puts("thread creation failed");
            exit(-1);
        }
    }
}

static void bench(void)
{
    for (int i = 0; i < BENCH_COUNT; i++) {
        ready = false;
        // 建立 client
        create_worker(MAX_THREAD);

        pthread_mutex_lock(&worker_lock);

        ready = true;

        /* all workers are ready, let's start bombing kecho */
        pthread_cond_broadcast(&worker_wait);

        pthread_mutex_unlock(&worker_lock);

        /* waiting for all workers to finish the measurement */
        for (int x = 0; x < MAX_THREAD; x++)
            pthread_join(pt[x], NULL);

        idx = 0;
    }
    // 計算平均時間
    for (int i = 0; i < MAX_THREAD; i++)
        fprintf(bench_fd, "%d %ld\n", i, time_res[i] /= BENCH_COUNT);
}

int main(void)
{
    srand(time(NULL));
    bench_fd = fopen(BENCHMARK_RESULT_FILE, "w");
    if (!bench_fd) {
        perror("fopen");
        return -1;
    }

    bench();

    fclose(bench_fd);

    return 0;
}
