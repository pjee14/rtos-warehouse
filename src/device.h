#ifndef DEVICE_H
#define DEVICE_H

/* 裝置抽象層:現在是虛擬(印到 terminal),
   之後接樹莓派時換成 GPIO 版本實作,上層呼叫完全不用改。 */
void dev_init(void);
void dev_show_number(int value);   /* 七段顯示器:顯示數字 */
void dev_set_led(int on);          /* 警報 LED:1=亮 0=滅 */
void dev_set_buzzer(int on);       /* 蜂鳴器:1=響 0=停 */
void dev_wait_button(void);   /* 阻塞直到按鈕被按下 */
void dev_show_number2(int n);   /* 第二顆七段:顯示最近異動分類的數量 */
int  dev_wait_card(void);      /* 阻塞直到刷到授權卡,回傳 1 */
int  dev_starts_locked(void);  /* 系統初始是否鎖定 */
#endif
