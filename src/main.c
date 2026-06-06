#include <stdio.h>
#include <stdlib.h>
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
static sem_t           alert_sem;

static void alert_post(alert_evt_t e) {
    pthread_mutex_lock(&alert_lock);
    if (alert_count < ALERT_CAP) alert_buf[alert_count++] = e;
    pthread_mutex_unlock(&alert_lock);
    sem_post(&alert_sem);
}
static alert_evt_t alert_wait(void) {
    sem_wait(&alert_sem);
    pthread_mutex_lock(&alert_lock);
    alert_evt_t e = alert_buf[0];
    for (int i = 1; i < alert_count; i++) alert_buf[i-1] = alert_buf[i];
    alert_count--;
    pthread_mutex_unlock(&alert_lock);
    return e;
}

static const char *action_name(action_t a) { return a == ACTION_IN ? "進貨" : "出貨"; }

static void *scanner_task(void *arg) {
    srand(time(NULL));
    while (g_running) {
        scan_msg_t *m = mempool_alloc(&msg_pool);
        m->type     = rand() % ITEM_TYPE_COUNT;
        m->action   = rand() % 2;
        m->amount   = 1 + rand() % 5;
        m->priority = ITEMS[m->type].priority;
        printf("[Scanner] 掃描: %s %s x%d\n",
               ITEMS[m->type].name, action_name(m->action), m->amount);
        pq_push(&scan_q, m);
        sleep(2);                 /* 每 2 秒模擬一筆 */
    }
    return NULL;
}

static void *inventory_task(void *arg) {
    while (1) {
        scan_msg_t *m = pq_pop(&scan_q);
        int th = ITEMS[m->type].threshold;

        pthread_mutex_lock(&inv_lock);
        int cur = inventory[m->type];
        int rejected = 0, now = cur;
        if (m->action == ACTION_IN) {
            inventory[m->type] += m->amount;
            now = inventory[m->type];
        } else {                                  /* 出貨 */
            if (m->amount > cur) {
                rejected = 1;                     /* 庫存不足,不扣帳 */
            } else {
                inventory[m->type] -= m->amount;
                now = inventory[m->type];
            }
        }
        pthread_mutex_unlock(&inv_lock);

        if (rejected) {
            alert_evt_t e;
            e.type = m->type; e.level = cur; e.threshold = th;
            e.reason = 1; e.requested = m->amount;
            alert_post(e);                        /* 交給 Alert 顯示「庫存不足」 */
        } else {
            printf("  %s %s %d → 現有 %d\n",
                   ITEMS[m->type].name, action_name(m->action), m->amount, now);
            if (m->action == ACTION_OUT && now < th) {
                alert_evt_t e;
                e.type = m->type; e.level = now; e.threshold = th;
                e.reason = 0; e.requested = 0;
                alert_post(e);
            }
        }
        mempool_free(&msg_pool, m);
    }
    return NULL;
}

/* Display:庫存有變化才更新七段 */
static void *display_task(void *arg) {
    int last;
    pthread_mutex_lock(&inv_lock);
    last = 0; for (int i = 0; i < ITEM_TYPE_COUNT; i++) last += inventory[i];
    pthread_mutex_unlock(&inv_lock);
    while (1) {
        int total = 0;
        pthread_mutex_lock(&inv_lock);
        for (int i = 0; i < ITEM_TYPE_COUNT; i++) total += inventory[i];
        pthread_mutex_unlock(&inv_lock);
        if (total != last) { dev_show_number(total); last = total; }
        usleep(100000);
    }
    return NULL;
}

/* Alert:庫存不足警報 */
static void *alert_task(void *arg) {
    while (1) {
        alert_evt_t e = alert_wait();
        dev_set_led(1); dev_set_buzzer(1);        /* 實體版閃燈鳴笛;虛擬版靜默 */
        if (e.reason == 1)
            printf("  !! 警報:%s 庫存不足(現有 %d,要求出貨 %d)→ 拒絕出貨\n",
                   ITEMS[e.type].name, e.level, e.requested);
        else
            printf("  !! 警報:%s 低於安全庫存(剩 %d,門檻 %d)\n",
                   ITEMS[e.type].name, e.level, e.threshold);
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

static void inventory_snapshot_str(char *out, size_t n) {
    int snap[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
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
                const char *w = "歡迎連線智慧倉庫,輸入任意字查庫存,quit 離開\n";
                send_all(connfd, w, strlen(w));
            } else {
                char buf[128];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) { close(fd); FD_CLR(fd, &master); }
                else {
                    buf[n] = '\0'; buf[strcspn(buf, "\r\n")] = '\0';
                    if (strncmp(buf, "quit", 4) == 0) { close(fd); FD_CLR(fd, &master); }
                    else {
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

    pq_init(&scan_q);
    mempool_init(&msg_pool);
    init_inventory_lock();
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) inventory[i] = 5;   /* 初始庫存皆 5 */
    sem_init(&alert_sem, 0, 0);
    dev_init();

    /* 安裝 SIGINT 處理函式(Ctrl+C) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    printf("=== 智慧倉庫系統 ===\n");
    printf("Task 優先權(SCHED_FIFO): Alert/Scanner=80  Inventory/Socket=50  Display=20\n");
    printf("系統自動模擬進出貨中... 按 Ctrl+C 結束\n");
    printf("遠端查詢: 另開終端機 nc 127.0.0.1 9000\n\n");

    if (create_rt_task(&alert,   alert_task,     PRIO_HIGH,   "Alert")     != 0) return 1;
    if (create_rt_task(&scanner, scanner_task,   PRIO_HIGH,   "Scanner")   != 0) return 1;
    if (create_rt_task(&inv_tid, inventory_task, PRIO_MEDIUM, "Inventory") != 0) return 1;
    if (create_rt_task(&sock,    socket_task,    PRIO_MEDIUM, "Socket")    != 0) return 1;
    if (create_rt_task(&display, display_task,   PRIO_LOW,    "Display")   != 0) return 1;

    /* 等待結束:q(Scanner 設旗標)或 Ctrl+C(SIGINT handler 設旗標) */
    while (g_running) usleep(200000);

    /* ---- 結束程序 ---- */
    printf("\n=== 系統關閉中 ===\n");
    dev_set_led(0);
    dev_set_buzzer(0);
    dev_show_number(0);

    int snap[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) snap[i] = inventory[i];
    pthread_mutex_unlock(&inv_lock);
    printf("最終庫存 | 醫療:%d  生鮮:%d  一般:%d\n",
           snap[ITEM_MEDICAL], snap[ITEM_FRESH], snap[ITEM_GENERAL]);
    printf("=== 系統已關閉 ===\n");
    return 0;
}
