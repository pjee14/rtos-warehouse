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

void       pq_init(pqueue_t *q);
void       pq_push(pqueue_t *q, scan_msg_t *m);   /* 生產者:放入並喚醒消費者 */
scan_msg_t *pq_pop(pqueue_t *q);                  /* 消費者:取出最高優先;空則阻塞 */

#endif
