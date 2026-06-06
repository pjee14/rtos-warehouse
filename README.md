# 智慧倉庫管理系統 (RTOS-based Smart Warehouse)

基於嵌入式 Linux 的即時智慧倉庫系統:用 pthread 實作多任務、SCHED_FIFO 即時優先權排程、
訊息佇列 / Mutex / 計數號誌三種 IPC、固定區塊記憶體池,以及並發 TCP socket server。

> 目前為**筆電模擬版**:七段顯示器 / LED / 蜂鳴器以 terminal 文字模擬,Scanner 自動隨機模擬進出貨。
> 樹莓派實體 GPIO 為後續工作(只需替換裝置層,上層程式不動)。

## 系統架構

5 條 task(pthread)以即時優先權並行運作,資料流如下:

    [Scanner]  自動隨機進出貨,從記憶體池取訊息,丟進優先序佇列
        |
        v   (優先序訊息佇列 scan_q,依貨物優先序排序)
    [Inventory]  取訊息 → 更新庫存表(mutex+優先權繼承) → 判斷門檻
        |
        |-- 低於門檻 --(計數號誌 alert_sem)--> [Alert] 亮 LED / 鳴蜂鳴器
        |
      庫存表 inventory[]
        ^                       ^
        | (讀,上鎖)             | (讀,上鎖)
    [Display] 顯示到七段     [Socket] <==TCP / select== 遠端 client 查詢

## 環境需求

- Linux(Ubuntu / Debian / WSL / 虛擬機),**不支援原生 Windows 或 macOS**
- gcc、make、python3-tk(可視化用)
- 執行需 root 權限(SCHED_FIFO 即時優先權)

## 編譯與執行

    make
    sudo ./warehouse

系統會自動隨機模擬進出貨。按 Ctrl+C 可關閉(印出最終庫存後安全離開)。

## 遠端查詢測試

另開一個終端機:

    nc 127.0.0.1 9000

連上後輸入任意字查即時庫存,輸入 quit 離開。可同時開多個 client。

## 即時可視化

圖形化前端會連到 socket(port 9000),把倉庫畫成立體紙箱堆疊(醫療紅十字、生鮮橘子、一般素面)並即時更新,庫存不足時警報器亮起。

    sudo apt install python3-tk      # 第一次需安裝
    python3 tools/warehouse_view.py  # C 系統執行中時另開

## 系統行為

- 三種貨物:醫療物資 / 生鮮食品 / 一般貨物,**安全門檻皆為 3,初始庫存皆為 5**。
- 進貨累加、出貨扣減。
- 出貨後低於門檻 → 觸發警報。
- 出貨量超過現有庫存 → **拒絕出貨並警報**(庫存不變)。
- 貨物優先序(醫療 > 生鮮 > 一般)決定佇列中先被處理的順序。

## 五個 Task

| Task | 優先權 | 職責 |
|------|--------|------|
| Scanner | High (80) | 自動隨機模擬進出貨,封裝訊息丟進佇列 |
| Alert | High (80) | 收到庫存不足通知 → 控制 LED / 蜂鳴器 |
| Inventory | Medium (50) | 取訊息、更新庫存、判斷門檻、拒絕超量出貨 |
| Socket | Medium (50) | TCP server,接受遠端 client 查詢 |
| Display | Low (20) | 顯示庫存總數到七段顯示器 |

## 三種 IPC

| 機制 | 程式變數 | 用途 |
|------|---------|------|
| 訊息佇列 | scan_q | Scanner → Inventory 非同步傳資料,依貨物優先序排序 |
| Mutex(優先權繼承) | inv_lock | 保護庫存表防 race condition;PRIO_INHERIT 防優先權反轉 |
| 計數號誌 | alert_sem | Inventory 通知 Alert 警報事件;記憶體池也用號誌管理空閒區塊 |

## 檔案結構

| 檔案 | 說明 |
|------|------|
| src/main.c | 5 個 task、優先權設定、警報通道、socket server、signal handler、main 流程 |
| src/message.h | 訊息結構、貨物種類/動作列舉、貨物屬性(priority、threshold) |
| src/pqueue.h / .c | 優先序訊息佇列(mutex + condition variable,空則阻塞) |
| src/mempool.h / .c | 固定區塊記憶體池(counting semaphore + mutex,O(1) 配置) |
| src/device.h | 裝置抽象介面(七段 / LED / 蜂鳴器) |
| src/device_virtual.c | 裝置介面的虛擬實作;Pi 上替換為 device_gpio.c |
| tools/warehouse_view.py | Python 即時可視化前端(連 socket,畫倉庫紙箱與警報器) |
| Makefile | 編譯設定 |

## 詳細文件

架構說明、簡報對應、Demo 腳本,詳見 docs/ 資料夾。

## 後續工作(樹莓派)

1. 撰寫 device_gpio.c,實作 device.h 同樣四個函式但操作真實 GPIO,Makefile 替換重編。
2. 按鈕用 kernel driver 的 request_irq 接中斷 + workqueue 做下半部,取代 Scanner 的亂數模擬。
