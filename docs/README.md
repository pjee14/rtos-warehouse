# 智慧倉庫管理系統 (RTOS-based Smart Warehouse)

基於嵌入式 Linux 的即時智慧倉庫系統,使用 pthread 多任務、SCHED_FIFO 優先權排程、
訊息佇列 / Mutex / 計數號誌三種 IPC、記憶體池與並發 TCP socket server。

## 環境需求
- Linux (Ubuntu / Debian / WSL / 虛擬機),不支援原生 Windows 或 macOS
- gcc、make
- 執行需 root 權限 (SCHED_FIFO 即時優先權)

## 編譯與執行
    make
    sudo ./warehouse

## 遠端查詢測試
另開一個終端機:
    nc 127.0.0.1 9000
連上後輸入任意字查庫存,輸入 quit 離開。

## 架構與 Demo 說明
詳見 docs/ 資料夾中的架構說明與 Demo 腳本文件。
