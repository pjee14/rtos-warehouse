#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <semaphore.h>
#include <signal.h>
#include "pqueue.h"
#include "device.h"
#include "mempool.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>

#define PRIO_HIGH   80
#define PRIO_MEDIUM 50
#define PRIO_LOW    20
#define INVENTORY_WORKERS 3

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }   /* handler 只設旗標 */

static const item_info_t ITEMS[ITEM_TYPE_COUNT] = {
    { "醫療物資", 3, 3  }, /* 名稱, item優先序, 門檻 */
    { "生鮮食品", 2, 3  },
    { "一般貨物", 1, 3 },
};

static pqueue_t        scan_q;
static mempool_t       msg_pool;
static int             inventory[ITEM_TYPE_COUNT] = {0};
static int             reserved_out[ITEM_TYPE_COUNT] = {0};
static pthread_mutex_t inv_lock;

/* ===== Inventory → Alert 警報通道 ===== */
#define ALERT_CAP 16
typedef struct {
    item_type_t type;
    int level;        /* 目前庫存 */
    int threshold;
    int reason;       /* 0=低於門檻, 1=庫存不足拒絕出貨 */
    int requested;    /* reason=1 時,要求出貨量 */
} alert_evt_t;

static alert_evt_t     alert_buf[ALERT_CAP];
static int             alert_count = 0;
static pthread_mutex_t alert_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  alert_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t alarm_lock = PTHREAD_MUTEX_INITIALIZER;
static int alarm_on = 0;

static void alert_post(alert_evt_t e) {
    pthread_mutex_lock(&alert_lock);
    if (alert_count < ALERT_CAP) {
        alert_buf[alert_count++] = e;
        pthread_cond_signal(&alert_not_empty);
    }
    pthread_mutex_unlock(&alert_lock);
}
static alert_evt_t alert_wait(void) {
    pthread_mutex_lock(&alert_lock);
    while (alert_count == 0)
        pthread_cond_wait(&alert_not_empty, &alert_lock);
    alert_evt_t e = alert_buf[0];
    for (int i = 1; i < alert_count; i++) alert_buf[i-1] = alert_buf[i];
    alert_count--;
    pthread_mutex_unlock(&alert_lock);
    return e;
}

/* ===== 網路寫入保護(多執行緒可能同時寫 client) ===== */
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static int  send_all(int fd, const char *data, size_t len);   /* 前置宣告 */
static void net_send(int fd, const char *msg);

static const char *action_name(action_t a) { return a == ACTION_IN ? "進貨" : "出貨"; }

static int prep_seconds(item_type_t t) {
    switch (t) {
        case ITEM_MEDICAL: return 3;   /* 醫療 3 秒 */
        case ITEM_FRESH:   return 5;   /* 生鮮 5 秒 */
        case ITEM_GENERAL: return 8;   /* 一般 8 秒 */
        default:           return 1;
    }
}

static void *inventory_task(void *arg) {
    int worker_id = (int)(intptr_t)arg;
    while (1) {
        int available[ITEM_TYPE_COUNT];
        pthread_mutex_lock(&inv_lock);
        for (int i = 0; i < ITEM_TYPE_COUNT; i++)
            available[i] = inventory[i] - reserved_out[i];
        pthread_mutex_unlock(&inv_lock);

        int waiting;
        scan_msg_t *m = pq_pop_priority(&scan_q, available, &waiting);
        int th = ITEMS[m->type].threshold;
        const char *nm = ITEMS[m->type].name;
        char reply[200];

        if (waiting > 1)
            printf("  [排程][員工%d] 佇列有 %d 筆,依規則挑出:%s(優先=%d, 數量=%d)\n",
                   worker_id, waiting, nm, m->priority, m->amount);

        /* 1) 鎖內先檢查出貨可用庫存是否足夠,足夠就保留 */
        pthread_mutex_lock(&inv_lock);
        int cur = inventory[m->type] - reserved_out[m->type];
        int rejected = (m->action == ACTION_OUT && m->amount > cur);
        if (!rejected && m->action == ACTION_OUT) {
            reserved_out[m->type] += m->amount;
        }
        pthread_mutex_unlock(&inv_lock);

        if (rejected) {
            printf("  !! [員工%d] %s 出貨 %d 遭拒:可用庫存不足(可用 %d)\n",
                   worker_id, nm, m->amount, cur);
            alert_evt_t e; e.type=m->type; e.level=cur; e.threshold=th; e.reason=1; e.requested=m->amount;
            alert_post(e);
            snprintf(reply, sizeof(reply), "[結果] %s 出貨 %d 遭拒:可用庫存不足(可用 %d)\n", nm, m->amount, cur);
            net_send(m->reply_fd, reply);
            mempool_free(&msg_pool, m);
            continue;                          /* 被拒不需備貨時間 */
        }

        /* 2) 受理 → 模擬備貨時間(鎖外,不卡住查詢) */
        int secs = prep_seconds(m->type);
        printf("  [處理中][員工%d] %s %s %d,備貨 %d 秒...\n",
               worker_id, nm, action_name(m->action), m->amount, secs);
        if (m->reply_fd >= 0) {
            snprintf(reply, sizeof(reply), "[處理中][員工%d] %s %s %d,預計 %d 秒...\n",
                     worker_id, nm, action_name(m->action), m->amount, secs);
            net_send(m->reply_fd, reply);
        }
        sleep(secs);

        /* 3) 備貨完成 → 鎖內提交庫存變動 */
        pthread_mutex_lock(&inv_lock);
        if (m->action == ACTION_IN) inventory[m->type] += m->amount;
        else {
            inventory[m->type] -= m->amount;
            reserved_out[m->type] -= m->amount;
        }
        int now = inventory[m->type];
        int avail_now = inventory[m->type] - reserved_out[m->type];
        pthread_mutex_unlock(&inv_lock);

        printf("  [員工%d] %s %s %d 完成 → 現有 %d,可用 %d\n",
               worker_id, nm, action_name(m->action), m->amount, now, avail_now);
        snprintf(reply, sizeof(reply), "[結果][員工%d] %s %s %d 完成,現有 %d,可用 %d\n",
                 worker_id, nm, action_name(m->action), m->amount, now, avail_now);
        net_send(m->reply_fd, reply);

        if (m->action == ACTION_OUT && avail_now < th) {
            alert_evt_t e; e.type=m->type; e.level=avail_now; e.threshold=th; e.reason=0; e.requested=0;
            alert_post(e);
        }
        mempool_free(&msg_pool, m);
    }
    return NULL;
}

/* Display:庫存有變化才更新七段 */
static void *display_task(void *arg) {
    int last_cat = -1;
    while (1) {
        int cat;
        pthread_mutex_lock(&inv_lock);
        cat = last_cat_count;
        pthread_mutex_unlock(&inv_lock);
        if (cat != last_cat && cat >= 0) { dev_show_number(cat); last_cat = cat; }
        usleep(100000);
    }
    return NULL;
}

static void *alert_task(void *arg) {
    while (1) {
        alert_evt_t e = alert_wait();
        if (e.reason == 1)
            printf("  !! 警報:%s 庫存不足(現有 %d,要求出貨 %d)→ 拒絕出貨\n",
                   ITEMS[e.type].name, e.level, e.requested);
        else
            printf("  !! 警報:%s 低於安全庫存(剩 %d,門檻 %d)\n",
                   ITEMS[e.type].name, e.level, e.threshold);

        pthread_mutex_lock(&alarm_lock);
        alarm_on = 1;
        pthread_mutex_unlock(&alarm_lock);
        dev_set_led(1);
        dev_set_buzzer(1);                 /* 持續響,直到按鈕解除 */
        printf("  >> 請按下按鈕解除警報\n");
    }
    return NULL;
}

static void *button_task(void *arg) {
    while (1) {
        dev_wait_button();                 /* 阻塞等按鈕(實體版=GPIO中斷) */
        pthread_mutex_lock(&alarm_lock);
        int was_on = alarm_on;
        alarm_on = 0;
        pthread_mutex_unlock(&alarm_lock);
        if (was_on) {
            dev_set_led(0);
            dev_set_buzzer(0);
            printf("  ✓ 警報已由管理員解除\n");
        }
    }
    return NULL;
}

#define SERVER_PORT 9000

static int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static void net_send(int fd, const char *msg) {
    if (fd < 0) return;                 /* reply_fd=-1(Scanner)不需回覆 */
    pthread_mutex_lock(&net_lock);
    send_all(fd, msg, strlen(msg));
    pthread_mutex_unlock(&net_lock);
}

static void inventory_snapshot_str(char *out, size_t n) {
    int snap[ITEM_TYPE_COUNT];
    int reserved[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) {
        snap[i] = inventory[i];
        reserved[i] = reserved_out[i];
    }
    pthread_mutex_unlock(&inv_lock);
    snprintf(out, n,
        "=== 庫存查詢結果 ===\n"
        "  醫療物資: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n"
        "  生鮮食品: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n"
        "  一般貨物: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n",
        snap[ITEM_MEDICAL], reserved[ITEM_MEDICAL], snap[ITEM_MEDICAL] - reserved[ITEM_MEDICAL], ITEMS[ITEM_MEDICAL].threshold,
        snap[ITEM_FRESH],   reserved[ITEM_FRESH],   snap[ITEM_FRESH] - reserved[ITEM_FRESH],     ITEMS[ITEM_FRESH].threshold,
        snap[ITEM_GENERAL], reserved[ITEM_GENERAL], snap[ITEM_GENERAL] - reserved[ITEM_GENERAL], ITEMS[ITEM_GENERAL].threshold);
}

static void *socket_task(void *arg) {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return NULL; }
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(SERVER_PORT);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return NULL; }
    if (listen(listenfd, 8) < 0) { perror("listen"); return NULL; }

    fd_set master; FD_ZERO(&master); FD_SET(listenfd, &master);
    int maxfd = listenfd;

    while (1) {
        fd_set readset = master;
        if (select(maxfd + 1, &readset, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        for (int fd = 0; fd <= maxfd; fd++) {
            if (!FD_ISSET(fd, &readset)) continue;
            if (fd == listenfd) {
                struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
                int connfd = accept(listenfd, (struct sockaddr *)&cli, &clilen);
                if (connfd < 0) continue;
                FD_SET(connfd, &master);
                if (connfd > maxfd) maxfd = connfd;
                net_send(connfd,
                    "=== 智慧倉庫連線成功 ===\n"
                    "  查詢: query\n"
                    "  進貨: in  med/food/goods 數量\n"
                    "  出貨: out med/food/goods 數量\n"
                    "  離開: quit\n");
            } else {
                char buf[128];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) { close(fd); FD_CLR(fd, &master); continue; }
                buf[n] = '\0'; buf[strcspn(buf, "\r\n")] = '\0';

                char cmd[16] = {0}, item[16] = {0}; int amt = 0;
                int k = sscanf(buf, " %15s %15s %d", cmd, item, &amt);

                if (k >= 1 && strcmp(cmd, "quit") == 0) {
                    close(fd); FD_CLR(fd, &master);
                } else if (k >= 1 && (strcmp(cmd, "in") == 0 || strcmp(cmd, "out") == 0)) {
                    item_type_t type; int ok = 1;
                    if      (strcmp(item, "med")   == 0) type = ITEM_MEDICAL;
                    else if (strcmp(item, "food")  == 0) type = ITEM_FRESH;
                    else if (strcmp(item, "goods") == 0) type = ITEM_GENERAL;
                    else ok = 0;
                    if (k != 3 || amt <= 0 || !ok) {
                        net_send(fd, "格式: in/out  med/food/goods  數量  (例: out med 3)\n");
                    } else {
                        scan_msg_t *m = mempool_alloc(&msg_pool);
                        m->type     = type;
                        m->action   = (cmd[0] == 'i') ? ACTION_IN : ACTION_OUT;
                        m->amount   = amt;
                        m->priority = ITEMS[type].priority;
                        m->reply_fd = fd;                  /* 處理完回報這個 client */
                        pq_push(&scan_q, m);               /* 依優先權排隊 */
                        net_send(fd, "已收到,依貨物優先權排入佇列處理...\n");
                    }
                } else {
                    char snap[512];
                    inventory_snapshot_str(snap, sizeof(snap));
                    net_send(fd, snap);
                }
            }
        }
    }
    close(listenfd);
    return NULL;
}

static int create_rt_task_arg(pthread_t *tid, void *(*fn)(void *), void *arg, int prio, const char *name) {
    pthread_attr_t attr; struct sched_param param;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    param.sched_priority = prio;
    pthread_attr_setschedparam(&attr, &param);
    int rc = pthread_create(tid, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "建立 %s 失敗: %s\n", name, strerror(rc));
        if (rc == EPERM) fprintf(stderr, "  → 即時優先權需要 root,請用 sudo!\n");
        return -1;
    }
    return 0;
}

static int create_rt_task(pthread_t *tid, void *(*fn)(void *), int prio, const char *name) {
    return create_rt_task_arg(tid, fn, NULL, prio, name);
}

static void init_inventory_lock(void) {
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&inv_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);
}

int main(void) {
    pthread_t inv_tid[INVENTORY_WORKERS], display, alert, sock, button;

    pq_init(&scan_q);
    mempool_init(&msg_pool);
    init_inventory_lock();
    dev_init();

    for (int i = 0; i < ITEM_TYPE_COUNT; i++) {
        inventory[i] = 5;   /* 初始庫存皆 5 */
        reserved_out[i] = 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("=== 智慧倉庫系統(Server)===\n");
    printf("初始庫存:醫療 5、生鮮 5、一般 5\n");
    printf("備貨時間:醫療 3 秒、生鮮 5 秒、一般 8 秒\n");
    printf("同時作業員工:%d 人\n", INVENTORY_WORKERS);
    printf("遠端連線:nc 127.0.0.1 9000  (query / in / out)\n");
    printf("按 Ctrl+C 結束\n\n");

    if (create_rt_task(&alert,   alert_task,     PRIO_HIGH,   "Alert")      != 0) return 1;
    if (create_rt_task(&button, button_task,    PRIO_HIGH,   "Button")     != 0) return 1;
    for (int i = 0; i < INVENTORY_WORKERS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Inventory-%d", i + 1);
        if (create_rt_task_arg(&inv_tid[i], inventory_task, (void *)(intptr_t)(i + 1),
                               PRIO_MEDIUM, name) != 0) return 1;
    }
    if (create_rt_task(&sock,    socket_task,    PRIO_MEDIUM, "Socket")    != 0) return 1;
    if (create_rt_task(&display, display_task,   PRIO_LOW,    "Display")   != 0) return 1;

    while (g_running) usleep(200000);

    printf("\n=== 系統關閉中 ===\n");
    dev_set_led(0); dev_set_buzzer(0); dev_show_number(0);
    int snap[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) snap[i] = inventory[i];
    pthread_mutex_unlock(&inv_lock);
    printf("最終庫存 | 醫療:%d  生鮮:%d  一般:%d\n", snap[ITEM_MEDICAL], snap[ITEM_FRESH], snap[ITEM_GENERAL]);
    printf("=== 系統已關閉 ===\n");
    return 0;
}

