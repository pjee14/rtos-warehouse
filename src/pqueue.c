#include "pqueue.h"
#include <stdlib.h>
#include <time.h>
#include <errno.h>

void pq_init(pqueue_t *q) {
    q->count = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void pq_push(pqueue_t *q, scan_msg_t *m) {
    pthread_mutex_lock(&q->lock);
    if (q->count < PQ_CAP) q->buf[q->count++] = m;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

scan_msg_t *pq_pop(pqueue_t *q) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->lock);

    int best = 0;
    for (int i = 1; i < q->count; i++)
        if (q->buf[i]->priority > q->buf[best]->priority) best = i;

    scan_msg_t *m = q->buf[best];
    q->buf[best] = q->buf[q->count - 1];
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return m;
}

scan_msg_t *pq_pop_priority(pqueue_t *q, const int *stock, int *waiting) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->lock);

    *waiting = q->count;

    int best = 0;
    for (int i = 1; i < q->count; i++) {
        scan_msg_t *a = q->buf[best];
        scan_msg_t *b = q->buf[i];
        if (b->priority > a->priority) {
            best = i;
        } else if (b->priority == a->priority) {
            int da = abs(a->amount - stock[a->type]);
            int db = abs(b->amount - stock[b->type]);
            if (db < da) best = i;
        }
    }

    scan_msg_t *m = q->buf[best];
    q->buf[best] = q->buf[q->count - 1];
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return m;
}

scan_msg_t *pq_pop_priority_timed(pqueue_t *q, const int *stock, int *waiting, int timeout_ms) {
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        while (q->count == 0) {
            int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
            if (rc == ETIMEDOUT) { pthread_mutex_unlock(&q->lock); return NULL; }
        }
    }
    if (waiting) *waiting = q->count;
    int best = 0;
    for (int i = 1; i < q->count; i++) {
        if (q->buf[i]->priority > q->buf[best]->priority) {
            best = i;
        } else if (q->buf[i]->priority == q->buf[best]->priority) {
            int di = q->buf[i]->amount    - stock[q->buf[i]->type];
            int db = q->buf[best]->amount - stock[q->buf[best]->type];
            if (di < 0) di = -di;
            if (db < 0) db = -db;
            if (di < db) best = i;
        }
    }
    scan_msg_t *m = q->buf[best];
    q->buf[best] = q->buf[q->count - 1];
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return m;
}
