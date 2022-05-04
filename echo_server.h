#ifndef ECHO_SERVER_H
#define ECHO_SERVER_H

#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <net/sock.h>

#define MODULE_NAME "kecho"

struct echo_server_param {
    struct socket *listen_sock;
};

struct echo_service {
    bool is_stopped;
    struct list_head worker;
};

struct kecho {
    struct socket *sock;
    struct list_head list;
    struct work_struct kecho_work;
};

enum {
    TRACE_kzalloc_err = 1,  // kzalloc 失敗的次數
    TRACE_get_err,          // get request 失敗的次數
    TRACE_send_err,         // send request 失敗的次數
    TRACE_recvmsg,          // recvmsg 的次數
    TRACE_sendmsg,          // sendmsg 的次數
    TRACE_shutdown,         // shutdown 的次數
    TRACE_accept_err,       // accept 失敗的次數
    TRACE_work_err          // 建立 work 失敗的次數
};

struct runtime_state {
    atomic_t kzalloc_err, get_err;
    atomic_t send_err, recvmsg;
    atomic_t sendmsg, shutdown;
    atomic_t accept_err, work_err;
};

#define TRACE(ops)                      \
    do {                                \
        if (TRACE_##ops)                \
            atomic_add(1, &states.ops); \
    } while (0)

extern int echo_server_daemon(void *);

#endif
