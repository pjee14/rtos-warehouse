#ifndef PQUEUE_H
#define PQUEUE_H
#include <pthread.h>
#include "message.h"
#define PQ_CAP 64

typedef struct {
    scan_msg_t     *buf[PQ_CAP];
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} pqueue_t;
void        pq_init(pqueue_t *q);
void        pq_push(pqueue_t *q, scan_msg_t *m);   /* 生產者:放入並喚醒消費者 */
/* 同 pq_pop_priority,但最多等 timeout_ms 毫秒;逾時回傳 NULL */
scan_msg_t *pq_pop_priority_timed(pqueue_t *q, const int *stock, int *waiting, int timeout_ms);
/* 消費者:取出最佳的一筆(先比商品優先權,同品項再比數量最接近庫存者);空則阻塞。
   stock 傳入目前庫存陣列供比較;waiting 回傳此刻佇列待處理筆數 */
#endif
