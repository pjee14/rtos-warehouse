#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include <semaphore.h>
#include "pqueue.h"
#include "device.h"
#include "mempool.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>

#define PRIO_HIGH   80
#define PRIO_MEDIUM 50
#define PRIO_LOW    20

static volatile sig_atomic_t g_running = 1;

/* SIGINT(Ctrl+C)處理函式:只設旗標,不在 handler 裡做複雜事 */
static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static const item_info_t ITEMS[ITEM_TYPE_COUNT] = {
    { "醫療物資", 3, 3  },
    { "生鮮食品", 2, 5  },
    { "一般貨物", 1, 10 },
};

static pqueue_t        scan_q;                       /* Scanner → Inventory */
static mempool_t msg_pool;
static int             inventory[ITEM_TYPE_COUNT] = {0};
static pthread_mutex_t inv_lock;                     /* 保護庫存表(優先權繼承) */

/* ===== Inventory → Alert 的警報通道(計數號誌 + 小緩衝) ===== */
#define ALERT_CAP 16
typedef struct { item_type_t type; int level; int threshold; } alert_evt_t;
static alert_evt_t     alert_buf[ALERT_CAP];
static int             alert_count = 0;
static pthread_mutex_t alert_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           alert_sem;                    /* 未處理警報數 */

static void alert_post(alert_evt_t e) {
    pthread_mutex_lock(&alert_lock);
    if (alert_count < ALERT_CAP) alert_buf[alert_count++] = e;
    pthread_mutex_unlock(&alert_lock);
    sem_post(&alert_sem);                            /* 通知 Alert:有警報 */
}
static alert_evt_t alert_wait(void) {
    sem_wait(&alert_sem);                            /* 沒警報 → BLOCKED */
    pthread_mutex_lock(&alert_lock);
    alert_evt_t e = alert_buf[0];
    for (int i = 1; i < alert_count; i++) alert_buf[i-1] = alert_buf[i];
    alert_count--;
    pthread_mutex_unlock(&alert_lock);
    return e;
}

static const char *action_name(action_t a) { return a == ACTION_IN ? "進貨" : "出貨"; }

static void print_sched_info(const char *name) {
    int policy; struct sched_param param;
    pthread_getschedparam(pthread_self(), &policy, &param);
    const char *pname = (policy == SCHED_FIFO) ? "SCHED_FIFO" :
                        (policy == SCHED_RR)   ? "SCHED_RR"   : "SCHED_OTHER";
    printf("[%-9s] 啟動 — 排程=%s, 優先權=%d\n", name, pname, param.sched_priority);
}

static void *scanner_task(void *arg) {
    print_sched_info("Scanner");
    srand(2024);
    while (1) {
        scan_msg_t *m = mempool_alloc(&msg_pool);
        m->type     = rand() % ITEM_TYPE_COUNT;
        m->action   = rand() % 2;
        m->amount   = 1 + rand() % 5;
        m->priority = ITEMS[m->type].priority;
        printf("[Scanner]   掃描: %s %s x%d\n",
               ITEMS[m->type].name, action_name(m->action), m->amount);
        pq_push(&scan_q, m);
        sleep(2);
    }
    return NULL;
}

static void *inventory_task(void *arg) {
    print_sched_info("Inventory");
    while (1) {
        scan_msg_t *m = pq_pop(&scan_q);

        pthread_mutex_lock(&inv_lock);
        if (m->action == ACTION_IN) inventory[m->type] += m->amount;
        else { inventory[m->type] -= m->amount; if (inventory[m->type] < 0) inventory[m->type] = 0; }
        int now = inventory[m->type];
        pthread_mutex_unlock(&inv_lock);

        printf("        [Inventory] %s %s x%d → 現有 %d\n",
               ITEMS[m->type].name, action_name(m->action), m->amount, now);

        if (m->action == ACTION_OUT && now < ITEMS[m->type].threshold) {
            alert_evt_t e = { m->type, now, ITEMS[m->type].threshold };
            alert_post(e);
        }

        mempool_free(&msg_pool, m);        /* 用完歸還記憶體池 */
    }
    return NULL;
}

static void *display_task(void *arg) {
    print_sched_info("Display");
    while (1) {
        int snap[ITEM_TYPE_COUNT], total = 0;
        pthread_mutex_lock(&inv_lock);
        for (int i = 0; i < ITEM_TYPE_COUNT; i++) { snap[i] = inventory[i]; total += snap[i]; }
        pthread_mutex_unlock(&inv_lock);
        printf("[Display]   庫存 | 醫療:%d  生鮮:%d  一般:%d (總計 %d)\n",
               snap[ITEM_MEDICAL], snap[ITEM_FRESH], snap[ITEM_GENERAL], total);
        dev_show_number(total);          /* 在七段顯示器上顯示總量 */
        sleep(4);
    }
    return NULL;
}

static void *alert_task(void *arg) {
    print_sched_info("Alert");
    while (1) {
        alert_evt_t e = alert_wait();
        printf("  *** [Alert] 警報!%s 僅剩 %d(門檻 %d)\n",
               ITEMS[e.type].name, e.level, e.threshold);
        dev_set_led(1); dev_set_buzzer(1);   /* 亮燈鳴笛 */
        sleep(1);                            /* 嗶一聲 */
        dev_set_led(0); dev_set_buzzer(0);
    }
    return NULL;
}

#define SERVER_PORT 9000

/* 確保整段資料都送出(socket 可能一次只送部分) */
static int send_all(int fd, const char *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* 取得庫存快照,格式化成回覆字串(給 Socket 用) */
static void inventory_snapshot_str(char *out, size_t n) {
    int snap[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);                 /* 讀共享表也要鎖 */
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) snap[i] = inventory[i];
    pthread_mutex_unlock(&inv_lock);
    snprintf(out, n,
        "=== 庫存查詢結果 ===\n"
        "  醫療物資: %d (門檻 %d)\n"
        "  生鮮食品: %d (門檻 %d)\n"
        "  一般貨物: %d (門檻 %d)\n",
        snap[ITEM_MEDICAL],  ITEMS[ITEM_MEDICAL].threshold,
        snap[ITEM_FRESH],    ITEMS[ITEM_FRESH].threshold,
        snap[ITEM_GENERAL],  ITEMS[ITEM_GENERAL].threshold);
}

static void *socket_task(void *arg) {
    print_sched_info("Socket");

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
    printf("[Socket]    Server 啟動,監聽 port %d (支援多 client)\n", SERVER_PORT);

    fd_set master;                       /* 主集合:所有要監看的 fd */
    FD_ZERO(&master);
    FD_SET(listenfd, &master);
    int maxfd = listenfd;

    while (1) {
        fd_set readset = master;         /* select 會改動集合,所以每次複製一份 */
        if (select(maxfd + 1, &readset, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }

        for (int fd = 0; fd <= maxfd; fd++) {
            if (!FD_ISSET(fd, &readset)) continue;

            if (fd == listenfd) {                          /* 有新連線 */
                struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
                int connfd = accept(listenfd, (struct sockaddr *)&cli, &clilen);
                if (connfd < 0) continue;
                FD_SET(connfd, &master);
                if (connfd > maxfd) maxfd = connfd;
                printf("[Socket]    client 連入: %s (fd=%d)\n", inet_ntoa(cli.sin_addr), connfd);
                const char *w = "歡迎連線智慧倉庫,輸入任意字查庫存,輸入 quit 離開\n";
                send_all(connfd, w, strlen(w));
            } else {                                       /* 既有 client 有資料 */
                char buf[128];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) {                              /* 斷線 */
                    printf("[Socket]    client 離線 (fd=%d)\n", fd);
                    close(fd); FD_CLR(fd, &master);
                } else {
                    buf[n] = '\0';
                    buf[strcspn(buf, "\r\n")] = '\0';
                    if (strncmp(buf, "quit", 4) == 0) {
                        printf("[Socket]    client 主動離線 (fd=%d)\n", fd);
                        close(fd); FD_CLR(fd, &master);
                    } else {
                        char reply[512];
                        inventory_snapshot_str(reply, sizeof(reply));
                        send_all(fd, reply, strlen(reply));
                    }
                }
            }
        }
    }
    close(listenfd);
    return NULL;
}

static int create_rt_task(pthread_t *tid, void *(*fn)(void *), int prio, const char *name) {
    pthread_attr_t attr; struct sched_param param;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    param.sched_priority = prio;
    pthread_attr_setschedparam(&attr, &param);
    int rc = pthread_create(tid, &attr, fn, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "建立 %s 失敗: %s\n", name, strerror(rc));
        if (rc == EPERM) fprintf(stderr, "  → 即時優先權需要 root,請用 sudo!\n");
        return -1;
    }
    return 0;
}

static void init_inventory_lock(void) {
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setprotocol(&mattr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&inv_lock, &mattr);
    pthread_mutexattr_destroy(&mattr);
}

int main(void) {
    pthread_t scanner, inv_tid, display, alert, sock;
    printf("=== 智慧倉庫系統啟動 ===\n");

    pq_init(&scan_q);
    mempool_init(&msg_pool);
    init_inventory_lock();
    sem_init(&alert_sem, 0, 0);                      /* 初始 0 個警報 */
    dev_init();

    if (create_rt_task(&alert,     alert_task,     PRIO_HIGH,   "Alert")     != 0) return 1;
    if (create_rt_task(&scanner,   scanner_task,   PRIO_HIGH,   "Scanner")   != 0) return 1;
    if (create_rt_task(&inv_tid, inventory_task, PRIO_MEDIUM, "Inventory") != 0) return 1;
    if (create_rt_task(&sock,      socket_task,    PRIO_MEDIUM, "Socket")    != 0) return 1;
    if (create_rt_task(&display,   display_task,   PRIO_LOW,    "Display")   != 0) return 1;

    /* 安裝 SIGINT 處理函式 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    printf("=== 系統運作中,按 Ctrl+C 可優雅關閉 ===\n");

    /* 主執行緒在此等待關閉訊號 */
    while (g_running) sleep(1);

    /* ---- 優雅關閉程序 ---- */
    printf("\n=== 收到關閉訊號,正在安全關閉系統... ===\n");
    dev_set_led(0);
    dev_set_buzzer(0);
    dev_show_number(0);

    int snap[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) snap[i] = inventory[i];
    pthread_mutex_unlock(&inv_lock);
    printf("最終庫存 | 醫療:%d  生鮮:%d  一般:%d\n",
           snap[ITEM_MEDICAL], snap[ITEM_FRESH], snap[ITEM_GENERAL]);

    printf("=== 系統已安全關閉 ===\n");
    return 0;
}
