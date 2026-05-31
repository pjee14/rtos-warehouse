#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <pthread.h>
#include <semaphore.h>
#include "message.h"

#define POOL_BLOCKS 32        /* 預先配置 32 個固定大小區塊 */

typedef struct {
    scan_msg_t      blocks[POOL_BLOCKS];      /* 固定大小區塊(啟動時一次配好) */
    int             free_list[POOL_BLOCKS];   /* 空閒區塊索引 */
    int             free_top;                 /* 空閒堆疊頂 */
    pthread_mutex_t lock;                     /* 保護 free_list */
    sem_t           avail;                    /* 計數號誌:可用區塊數 */
} mempool_t;

void        mempool_init(mempool_t *p);
scan_msg_t *mempool_alloc(mempool_t *p);      /* 取一塊;沒空塊時阻塞 */
void        mempool_free(mempool_t *p, scan_msg_t *blk);

#endif
