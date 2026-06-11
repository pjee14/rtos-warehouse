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
#include <sys/time.h>

#define PRIO_HIGH   80
#define PRIO_MEDIUM 50
#define PRIO_LOW    20
#define WORKER_COUNT   2          /* 兩位 worker,平行處理 */
#define MIN_SHIFT_SEC  15.0       /* 上班滿 15 秒才能下班 */

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int sig) { (void)sig; g_running = 0; }

static const item_info_t ITEMS[ITEM_TYPE_COUNT] = {
    { "醫療物資", 3, 3 },  /* 名稱, item優先序, 門檻 */
    { "生鮮食品", 2, 3 },
    { "一般貨物", 1, 3 },
};
static const char *ICODE[ITEM_TYPE_COUNT] = { "med", "food", "goods" };

static double now_sec(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

static pqueue_t        scan_q;
static mempool_t       msg_pool;
static int             inventory[ITEM_TYPE_COUNT] = {0};
static int             reserved_out[ITEM_TYPE_COUNT] = {0};
static int             last_cat_count = -1;
static pthread_mutex_t inv_lock;

/* ===== 上下班打卡狀態 ===== */
static pthread_mutex_t shift_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  shift_cv   = PTHREAD_COND_INITIALIZER;
static int    on_duty[WORKER_COUNT]      = {0};   /* 1=上班中 */
static double clockin_sec[WORKER_COUNT]  = {0};   /* 上班時刻 */
static int    clockout_req[WORKER_COUNT] = {0};   /* 已刷卡要求下班(待當前工作做完) */

/* ===== worker 備貨狀態(給 status/視覺化) ===== */
typedef struct {
    int         busy;
    item_type_t item;
    action_t    action;
    double      start;
    int         prep;
} wstate_t;
static wstate_t        wstate[WORKER_COUNT];
static pthread_mutex_t wstate_lock = PTHREAD_MUTEX_INITIALIZER;

/* 刷卡/打卡:reader i 或 punch 指令都呼叫這支 (i 為 0-based) */
static void worker_punch(int i) {
    if (i < 0 || i >= WORKER_COUNT) return;
    pthread_mutex_lock(&shift_lock);
    double t = now_sec();
    if (!on_duty[i]) {
        on_duty[i] = 1;
        clockin_sec[i] = t;
        clockout_req[i] = 0;
        printf("  [打卡] 員工%d 上班\n", i + 1);
        pthread_cond_broadcast(&shift_cv);
    } else {
        double worked = t - clockin_sec[i];
        if (worked < MIN_SHIFT_SEC) {
            printf("  [打卡] 員工%d 上班未滿 %.0f 秒(目前 %.1f 秒),暫不能下班\n",
                   i + 1, MIN_SHIFT_SEC, worked);
        } else {
            clockout_req[i] = 1;
            printf("  [打卡] 員工%d 下班打卡(處理完當前貨物後生效)\n", i + 1);
            pthread_cond_broadcast(&shift_cv);
        }
    }
    pthread_mutex_unlock(&shift_lock);
}

static int any_on_duty(void) {
    int c = 0;
    pthread_mutex_lock(&shift_lock);
    for (int i = 0; i < WORKER_COUNT; i++) c += on_duty[i];
    pthread_mutex_unlock(&shift_lock);
    return c;
}

/* ===== Inventory → Alert 警報通道 ===== */
#define ALERT_CAP 16
typedef struct {
    item_type_t type;
    int level;
    int threshold;
    int reason;       /* 0=低於門檻, 1=庫存不足拒絕出貨 */
    int requested;
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

/* ===== 網路寫入保護 ===== */
static pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;
static int  send_all(int fd, const char *data, size_t len);
static void net_send(int fd, const char *msg);

static const char *action_name(action_t a) { return a == ACTION_IN ? "進貨" : "出貨"; }

static int prep_seconds(item_type_t t) {
    switch (t) {
        case ITEM_MEDICAL: return 3;
        case ITEM_FRESH:   return 5;
        case ITEM_GENERAL: return 8;
        default:           return 1;
    }
}

/* Scanner:每台讀卡機一條,reader i 刷卡 = 員工 i 打卡 */
static void *scanner_task(void *arg) {
    int reader = (int)(intptr_t)arg;
    while (g_running) {
        if (dev_wait_card(reader)) {
            worker_punch(reader);
            sleep(3);            /* 冷卻,避免一次刷卡被當多次 */
        }
    }
    return NULL;
}

static void *inventory_task(void *arg) {
    int worker_id = (int)(intptr_t)arg;   /* 1-based */
    int i = worker_id - 1;
    while (g_running) {
        /* (A) 等上班打卡 */
        pthread_mutex_lock(&shift_lock);
        while (g_running && !on_duty[i])
            pthread_cond_wait(&shift_cv, &shift_lock);
        pthread_mutex_unlock(&shift_lock);
        if (!g_running) break;
        printf("  [員工%d] 開始上班,可處理進出貨\n", worker_id);

        /* (B) 上班中:處理工作直到下班生效 */
        while (g_running) {
            /* 下班只在兩筆工作之間生效:滿 15 秒且已刷下班卡才真的下班 */
            pthread_mutex_lock(&shift_lock);
            if (on_duty[i] && clockout_req[i] &&
                (now_sec() - clockin_sec[i]) >= MIN_SHIFT_SEC) {
                on_duty[i] = 0;
                clockout_req[i] = 0;
            }
            int still = on_duty[i];
            pthread_mutex_unlock(&shift_lock);
            if (!still) { printf("  [員工%d] 已下班\n", worker_id); break; }

            int available[ITEM_TYPE_COUNT];
            pthread_mutex_lock(&inv_lock);
            for (int k = 0; k < ITEM_TYPE_COUNT; k++)
                available[k] = inventory[k] - reserved_out[k];
            pthread_mutex_unlock(&inv_lock);

            int waiting;
            scan_msg_t *m = pq_pop_priority_timed(&scan_q, available, &waiting, 300);
            if (!m) continue;     /* 0.3 秒沒工作 → 回頭再檢查是否該下班 */

            int th = ITEMS[m->type].threshold;
            const char *nm = ITEMS[m->type].name;
            char reply[200];

            if (waiting > 1)
                printf("  [排程][員工%d] 佇列有 %d 筆,依規則挑出:%s(優先=%d, 數量=%d)\n",
                       worker_id, waiting, nm, m->priority, m->amount);

            /* 1) 鎖內檢查出貨可用庫存,足夠就保留 */
            pthread_mutex_lock(&inv_lock);
            int cur = inventory[m->type] - reserved_out[m->type];
            int rejected = (m->action == ACTION_OUT && m->amount > cur);
            if (!rejected && m->action == ACTION_OUT)
                reserved_out[m->type] += m->amount;
            pthread_mutex_unlock(&inv_lock);

            if (rejected) {
                printf("  !! [員工%d] %s 出貨 %d 遭拒:可用庫存不足(可用 %d)\n",
                       worker_id, nm, m->amount, cur);
                alert_evt_t e; e.type=m->type; e.level=cur; e.threshold=th; e.reason=1; e.requested=m->amount;
                alert_post(e);
                snprintf(reply, sizeof(reply), "[結果] %s 出貨 %d 遭拒:可用庫存不足(可用 %d)\n", nm, m->amount, cur);
                net_send(m->reply_fd, reply);
                mempool_free(&msg_pool, m);
                continue;
            }

            /* 2) 受理 → 備貨時間(鎖外) */
            int secs = prep_seconds(m->type);
            printf("  [處理中][員工%d] %s %s %d,備貨 %d 秒...\n",
                   worker_id, nm, action_name(m->action), m->amount, secs);
            if (m->reply_fd >= 0) {
                snprintf(reply, sizeof(reply), "[處理中][員工%d] %s %s %d,預計 %d 秒...\n",
                         worker_id, nm, action_name(m->action), m->amount, secs);
                net_send(m->reply_fd, reply);
            }

            pthread_mutex_lock(&wstate_lock);
            wstate[i].busy = 1; wstate[i].item = m->type;
            wstate[i].action = m->action; wstate[i].start = now_sec(); wstate[i].prep = secs;
            pthread_mutex_unlock(&wstate_lock);

            sleep(secs);          /* 即使中途刷了下班卡,也會把這筆做完 */

            pthread_mutex_lock(&wstate_lock);
            wstate[i].busy = 0;
            pthread_mutex_unlock(&wstate_lock);

            /* 3) 完成 → 鎖內提交 */
            pthread_mutex_lock(&inv_lock);
            if (m->action == ACTION_IN) inventory[m->type] += m->amount;
            else { inventory[m->type] -= m->amount; reserved_out[m->type] -= m->amount; }
            int now = inventory[m->type];
            int avail_now = inventory[m->type] - reserved_out[m->type];
            last_cat_count = now;
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
    }
    return NULL;
}

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
        dev_set_buzzer(1);
        printf("  >> 請按下按鈕解除警報\n");
    }
    return NULL;
}

static void *button_task(void *arg) {
    while (1) {
        dev_wait_button();
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
    if (fd < 0) return;
    pthread_mutex_lock(&net_lock);
    send_all(fd, msg, strlen(msg));
    pthread_mutex_unlock(&net_lock);
}

static void inventory_snapshot_str(char *out, size_t n) {
    int snap[ITEM_TYPE_COUNT];
    int reserved[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) { snap[i] = inventory[i]; reserved[i] = reserved_out[i]; }
    pthread_mutex_unlock(&inv_lock);
    int on = any_on_duty();
    snprintf(out, n,
        "=== 庫存查詢結果 ===\n"
        "  目前上班人數: %d\n"
        "  醫療物資: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n"
        "  生鮮食品: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n"
        "  一般貨物: 現有 %d / 保留出貨 %d / 可用 %d (門檻 %d)\n",
        on,
        snap[ITEM_MEDICAL], reserved[ITEM_MEDICAL], snap[ITEM_MEDICAL] - reserved[ITEM_MEDICAL], ITEMS[ITEM_MEDICAL].threshold,
        snap[ITEM_FRESH],   reserved[ITEM_FRESH],   snap[ITEM_FRESH] - reserved[ITEM_FRESH],     ITEMS[ITEM_FRESH].threshold,
        snap[ITEM_GENERAL], reserved[ITEM_GENERAL], snap[ITEM_GENERAL] - reserved[ITEM_GENERAL], ITEMS[ITEM_GENERAL].threshold);
}

/* status:給視覺化用
 *   LOCK <0|1>             (1=無人上班=等同鎖定)
 *   ITEM <code> <現有> <門檻> <保留出貨>
 *   WORKER <id> <off|idle|busy> <code|-> <in|out|-> <剩餘秒> <總秒>
 */
static void status_str(char *out, size_t n) {
    int snap[ITEM_TYPE_COUNT], resv[ITEM_TYPE_COUNT];
    pthread_mutex_lock(&inv_lock);
    for (int i = 0; i < ITEM_TYPE_COUNT; i++) { snap[i] = inventory[i]; resv[i] = reserved_out[i]; }
    pthread_mutex_unlock(&inv_lock);

    int duty[WORKER_COUNT];
    pthread_mutex_lock(&shift_lock);
    for (int i = 0; i < WORKER_COUNT; i++) duty[i] = on_duty[i];
    pthread_mutex_unlock(&shift_lock);

    int anyon = 0;
    for (int i = 0; i < WORKER_COUNT; i++) anyon |= duty[i];

    int off = snprintf(out, n, "LOCK %d\n", anyon ? 0 : 1);
    for (int i = 0; i < ITEM_TYPE_COUNT && off < (int)n; i++)
        off += snprintf(out + off, n - off, "ITEM %s %d %d %d\n",
                        ICODE[i], snap[i], ITEMS[i].threshold, resv[i]);

    double t = now_sec();
    pthread_mutex_lock(&wstate_lock);
    for (int i = 0; i < WORKER_COUNT && off < (int)n; i++) {
        if (duty[i] && wstate[i].busy) {
            double rem = wstate[i].prep - (t - wstate[i].start);
            if (rem < 0) rem = 0;
            off += snprintf(out + off, n - off, "WORKER %d busy %s %s %.1f %d\n",
                            i + 1, ICODE[wstate[i].item],
                            wstate[i].action == ACTION_IN ? "in" : "out", rem, wstate[i].prep);
        } else if (duty[i]) {
            off += snprintf(out + off, n - off, "WORKER %d idle - - 0 0\n", i + 1);
        } else {
            off += snprintf(out + off, n - off, "WORKER %d off - - 0 0\n", i + 1);
        }
    }
    pthread_mutex_unlock(&wstate_lock);
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
                    "  狀態: status\n"
                    "  進貨: in  med/food/goods 數量\n"
                    "  出貨: out med/food/goods 數量\n"
                    "  打卡: punch 1 或 punch 2 (模擬刷卡上/下班)\n"
                    "  離開: quit\n"
                    "  (注意:需有 worker 上班才能進出貨)\n");
            } else {
                char buf[128];
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) { close(fd); FD_CLR(fd, &master); continue; }
                buf[n] = '\0'; buf[strcspn(buf, "\r\n")] = '\0';

                char cmd[16] = {0}, item[16] = {0}; int amt = 0;
                int k = sscanf(buf, " %15s %15s %d", cmd, item, &amt);

                if (k >= 1 && strcmp(cmd, "quit") == 0) {
                    close(fd); FD_CLR(fd, &master);
                } else if (k >= 1 && strcmp(cmd, "status") == 0) {
                    char st[768];
                    status_str(st, sizeof(st));
                    net_send(fd, st);
                } else if (k >= 1 && strcmp(cmd, "punch") == 0) {
                    int w = (k >= 2) ? atoi(item) : 0;
                    if (w >= 1 && w <= WORKER_COUNT) { worker_punch(w - 1); net_send(fd, "已模擬打卡\n"); }
                    else net_send(fd, "用法: punch 1 或 punch 2\n");
                } else if (k >= 1 && (strcmp(cmd, "in") == 0 || strcmp(cmd, "out") == 0)) {
                    if (!any_on_duty()) {
                        net_send(fd, "目前無人上班,無法進出貨,請先刷卡(或 punch)上班\n");
                    } else {
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
                            m->reply_fd = fd;
                            pq_push(&scan_q, m);
                            net_send(fd, "已收到,依貨物優先權排入佇列處理...\n");
                        }
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
    pthread_t inv_tid[WORKER_COUNT], display, alert, sock, button, scanner[WORKER_COUNT];

    pq_init(&scan_q);
    mempool_init(&msg_pool);
    init_inventory_lock();
    dev_init();

    for (int i = 0; i < ITEM_TYPE_COUNT; i++) { inventory[i] = 5; reserved_out[i] = 0; }
    for (int i = 0; i < WORKER_COUNT; i++) { wstate[i].busy = 0; on_duty[i] = 0; }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int nreaders = dev_card_count();

    printf("=== 智慧倉庫系統(Server)===\n");
    printf("初始庫存:醫療 5、生鮮 5、一般 5\n");
    printf("備貨時間:醫療 3 秒、生鮮 5 秒、一般 8 秒\n");
    printf("worker 數:%d(需刷卡上班才會處理;下班需滿 %.0f 秒)\n", WORKER_COUNT, MIN_SHIFT_SEC);
    printf("讀卡機數:%d (沒接到的可用 punch 指令模擬打卡)\n", nreaders);
    printf("遠端連線:nc 127.0.0.1 9000  (query / status / in / out / punch)\n");
    printf("按 Ctrl+C 結束\n\n");

    if (create_rt_task(&alert,  alert_task,  PRIO_HIGH, "Alert")  != 0) return 1;
    if (create_rt_task(&button, button_task, PRIO_HIGH, "Button") != 0) return 1;

    /* 每台讀卡機一條 scanner;reader i 對應 worker i(這裡只有 PN532=reader0=員工1) */
    for (int i = 0; i < nreaders && i < WORKER_COUNT; i++) {
        char name[32]; snprintf(name, sizeof(name), "Scanner-%d", i);
        if (create_rt_task_arg(&scanner[i], scanner_task, (void *)(intptr_t)i, PRIO_MEDIUM, name) != 0) return 1;
    }

    for (int i = 0; i < WORKER_COUNT; i++) {
        char name[32]; snprintf(name, sizeof(name), "Inventory-%d", i + 1);
        if (create_rt_task_arg(&inv_tid[i], inventory_task, (void *)(intptr_t)(i + 1),
                               PRIO_MEDIUM, name) != 0) return 1;
    }
    if (create_rt_task(&sock,    socket_task,  PRIO_MEDIUM, "Socket")  != 0) return 1;
    if (create_rt_task(&display, display_task, PRIO_LOW,    "Display") != 0) return 1;

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
