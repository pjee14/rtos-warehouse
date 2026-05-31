#include <stdio.h>
#include "mempool.h"

void mempool_init(mempool_t *p) {
    for (int i = 0; i < POOL_BLOCKS; i++) p->free_list[i] = i;
    p->free_top = POOL_BLOCKS;                  /* 全部可用 */
    pthread_mutex_init(&p->lock, NULL);
    sem_init(&p->avail, 0, POOL_BLOCKS);        /* 初始 = 區塊總數 */
    printf("[記憶體池] 就緒:%d 個固定區塊(每塊 %zu bytes)\n",
           POOL_BLOCKS, sizeof(scan_msg_t));
}

scan_msg_t *mempool_alloc(mempool_t *p) {
    sem_wait(&p->avail);                        /* 沒空塊 → 阻塞(背壓) */
    pthread_mutex_lock(&p->lock);
    int idx = p->free_list[--p->free_top];      /* O(1) 取一塊 */
    pthread_mutex_unlock(&p->lock);
    return &p->blocks[idx];
}

void mempool_free(mempool_t *p, scan_msg_t *blk) {
    int idx = (int)(blk - p->blocks);           /* 由位址反推索引 */
    pthread_mutex_lock(&p->lock);
    p->free_list[p->free_top++] = idx;          /* O(1) 歸還 */
    pthread_mutex_unlock(&p->lock);
    sem_post(&p->avail);
}
