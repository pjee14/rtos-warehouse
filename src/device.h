#ifndef DEVICE_H
#define DEVICE_H

/* 裝置抽象層:現在是虛擬(印到 terminal),
   之後接樹莓派時換成 GPIO 版本實作,上層呼叫完全不用改。 */
void dev_init(void);
void dev_show_number(int value);   /* 七段顯示器:顯示數字 */
void dev_set_led(int on);          /* 警報 LED:1=亮 0=滅 */
void dev_set_buzzer(int on);       /* 蜂鳴器:1=響 0=停 */

#endif
