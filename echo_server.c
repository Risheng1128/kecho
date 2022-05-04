#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>
#include <linux/types.h>

#include "echo_server.h"

#define BUF_SIZE 4096

struct echo_service daemon = {.is_stopped = false};
struct runtime_state states = {0};
extern struct workqueue_struct *kecho_wq;

static int get_request(struct socket *sock, unsigned char *buf, size_t size)
{
    struct msghdr msg;
    struct kvec vec;
    int length;

    /* kvec setting */
    vec.iov_len = size;
    vec.iov_base = buf;

    /* msghdr setting */
    msg.msg_name = 0;
    msg.msg_namelen = 0;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    /* get msg */
    length = kernel_recvmsg(sock, &msg, &vec, size, size, msg.msg_flags);
    TRACE(recvmsg);
    return length;
}

static int send_request(struct socket *sock, unsigned char *buf, size_t size)
{
    int length;
    struct kvec vec;
    struct msghdr msg;

    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;

    vec.iov_base = buf;
    vec.iov_len = strlen(buf);

    length = kernel_sendmsg(sock, &msg, &vec, 1, size);
    TRACE(sendmsg);

    return length;
}

static void echo_server_worker(struct work_struct *work)
{
    struct kecho *worker = container_of(work, struct kecho, kecho_work);
    unsigned char *buf;

    // 取得 buffer 空間
rekzalloc:
    buf = kzalloc(BUF_SIZE, GFP_KERNEL);
    if (!buf) {
        TRACE(kzalloc_err);
        goto rekzalloc;
    }

    // 當程式還沒有要中斷前，執行無限迴圈
    while (!daemon.is_stopped) {
        // 取得資料
        int res = get_request(worker->sock, buf, BUF_SIZE - 1);
        if (res <= 0) {
            if (res)
                TRACE(get_err);
            break;
        }
        // 回傳資料
        res = send_request(worker->sock, buf, res);
        if (res < 0) {
            TRACE(send_err);
            break;
        }
        // 重置 buffer
        memset(buf, 0, res);
    }

    kernel_sock_shutdown(worker->sock, SHUT_RDWR);
    TRACE(shutdown);
    kfree(buf);
}

static struct work_struct *create_work(struct socket *sk)
{
    struct kecho *work;

    // 分配 kecho 結構大小的空間
    // GFP_KERNEL: 正常配置記憶體
    if (!(work = kmalloc(sizeof(struct kecho), GFP_KERNEL)))
        return NULL;

    work->sock = sk;

    // 初始化已經建立的 work ，並運行函式 echo_server_worker
    INIT_WORK(&work->kecho_work, echo_server_worker);

    list_add(&work->list, &daemon.worker);

    return &work->kecho_work;
}

/* it would be better if we do this dynamically */
static void free_work(void)
{
    struct kecho *l, *tar;
    /* cppcheck-suppress uninitvar */

    list_for_each_entry_safe (tar, l, &daemon.worker, list) {
        kernel_sock_shutdown(tar->sock, SHUT_RDWR);
        flush_work(&tar->kecho_work);
        sock_release(tar->sock);
        kfree(tar);
    }
}

static void do_analysis(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#define TRACE_PRINT(ops) \
    printk(MODULE_NAME ": %s : %d\n", #ops, atomic_read(&states.ops));
    TRACE_PRINT(recvmsg);
    TRACE_PRINT(sendmsg);
    TRACE_PRINT(shutdown);
    TRACE_PRINT(kzalloc_err);
    TRACE_PRINT(get_err);
    TRACE_PRINT(send_err);
    TRACE_PRINT(accept_err);
    TRACE_PRINT(work_err);
}

int echo_server_daemon(void *arg)
{
    struct echo_server_param *param = arg;
    struct socket *sock;
    struct work_struct *work;

    // 登記要接收的 Signal
    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    INIT_LIST_HEAD(&daemon.worker);

    // 判斷執行緒是否該被中止
    while (!kthread_should_stop()) {
        /* using blocking I/O */
        int error = kernel_accept(param->listen_sock, &sock, 0);
        if (error < 0) {
            // 檢查當前執行緒是否有 signal 處理
            if (signal_pending(current))
                break;
            TRACE(accept_err);
            continue;
        }

        if (unlikely(!(work = create_work(sock)))) {
            TRACE(work_err);
            kernel_sock_shutdown(sock, SHUT_RDWR);
            sock_release(sock);
            continue;
        }

        /* start server worker */
        queue_work(kecho_wq, work);
    }

    printk(MODULE_NAME ": daemon shutdown in progress...\n");

    daemon.is_stopped = true;
    do_analysis();
    free_work();
    return 0;
}
