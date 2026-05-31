#ifndef MESSAGE_H
#define MESSAGE_H

/* 貨物種類 */
typedef enum {
    ITEM_MEDICAL = 0,   /* 醫療物資 */
    ITEM_FRESH,         /* 生鮮食品 */
    ITEM_GENERAL,       /* 一般貨物 */
    ITEM_TYPE_COUNT
} item_type_t;

/* 動作 */
typedef enum {
    ACTION_IN = 0,      /* 進貨 */
    ACTION_OUT          /* 出貨 */
} action_t;

/* Scanner → Inventory 傳遞的訊息 */
typedef struct {
    item_type_t type;       /* 哪一種貨物 */
    action_t    action;     /* 進貨 / 出貨 */
    int         amount;     /* 數量 */
    int         priority;   /* 貨物優先序:數字越大越優先 */
} scan_msg_t;

/* 每種貨物的靜態屬性 */
typedef struct {
    const char *name;
    int priority;     /* 越大越優先 */
    int threshold;    /* 低於此值要警報(3b 之後用) */
} item_info_t;

#endif
