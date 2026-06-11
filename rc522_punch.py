#!/usr/bin/env python3
# RC522(MFRC522)讀卡 → 對 warehouse server 送 "punch 2"(員工2 上/下班打卡)
# 跑在 Pi 上(RC522 接在這台 Pi 的 SPI0)
# 安裝:sudo pip3 install mfrc522 --break-system-packages
# 執行:python3 rc522_punch.py            (server 在本機)
#       python3 rc522_punch.py 127.0.0.1  (指定 IP)
import time, socket, sys
from mfrc522 import SimpleMFRC522
import RPi.GPIO as GPIO

HOST   = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT   = 9000
WORKER = 2          # 這台讀卡機代表員工2
COOLDOWN = 3.0      # 一次刷卡後冷卻秒數,避免被當多次

def send_punch():
    try:
        s = socket.create_connection((HOST, PORT), timeout=3)
        s.recv(1024)                       # 吃掉歡迎訊息
        s.sendall(("punch %d\n" % WORKER).encode())
        time.sleep(0.2)
        try: print("  server:", s.recv(256).decode("utf-8", "ignore").strip())
        except Exception: pass
        s.close()
        return True
    except Exception as e:
        print("  送出失敗:", e)
        return False

def main():
    reader = SimpleMFRC522()
    print("RC522 就緒,等待刷卡(員工2 打卡)… server=%s:%d" % (HOST, PORT))
    try:
        while True:
            uid, _text = reader.read()     # 阻塞,直到讀到卡
            print("讀到卡 UID=%s → punch %d" % (uid, WORKER))
            send_punch()
            time.sleep(COOLDOWN)
    except KeyboardInterrupt:
        pass
    finally:
        GPIO.cleanup()
        print("\nRC522 讀卡程式結束")

if __name__ == "__main__":
    main()
