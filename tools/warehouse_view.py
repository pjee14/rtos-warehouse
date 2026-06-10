#!/usr/bin/env python3
# 智慧倉庫即時可視化:立體紙箱(漸層光影)+ 橘子/紅十字 + 警報器
import socket, re, tkinter as tk
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = 9000
ZONES = [("醫療用品", "med"), ("生鮮食品", "food"), ("一般貨物", "goods")]
NAME_MAP = {"med": "醫療物資", "food": "生鮮食品", "goods": "一般貨物"}
RED = "#d9352c"
W, H = 720, 500
FLOOR = H - 100

class WarehouseView:
    def __init__(self, root):
        self.root = root
        root.title("智慧倉庫即時監控")
        self.canvas = tk.Canvas(root, width=W, height=H, bg="#20202f", highlightthickness=0)
        self.canvas.pack()
        self.sock = None
        self.connect()
        self.poll()

    # ---- 連線 / 查詢 / 解析 ----
    def connect(self):
        try:
            self.sock = socket.create_connection((HOST, PORT), timeout=2)
            self.sock.settimeout(2); self.sock.recv(1024)
        except Exception:
            self.sock = None
    def query(self):
        if not self.sock:
            self.connect()
            if not self.sock: return None
        try:
            self.sock.sendall(b"query\n")
            return self.sock.recv(1024).decode("utf-8", "ignore")
        except Exception:
            self.sock = None; return None
    def parse(self, text):
        inv = {}
        for line in text.splitlines():
            m = re.search(
                r"(醫療物資|生鮮食品|一般貨物):\s*現有\s*(\d+).*?\(門檻\s*(\d+)\)",
                line)
            if m:
                inv[m.group(1)] = (int(m.group(2)), int(m.group(3)))
        return inv

    # ---- 漸層工具 ----
    @staticmethod
    def _lerp(c1, c2, t):
        a = (int(c1[1:3],16), int(c1[3:5],16), int(c1[5:7],16))
        b = (int(c2[1:3],16), int(c2[3:5],16), int(c2[5:7],16))
        return "#%02x%02x%02x" % tuple(int(a[i] + (b[i]-a[i]) * t) for i in range(3))
    def vgrad(self, x0, y0, x1, y1, top, bot, n=16):
        step = (y1 - y0) / n
        for i in range(n):
            col = self._lerp(top, bot, i / (n - 1) if n > 1 else 0)
            self.canvas.create_rectangle(x0, y0 + i*step, x1, y0 + (i+1)*step + 1, fill=col, outline=col)

    # ---- 立體紙箱 ----
    def carton(self, cx, base_y, kind, low, top_lid):
        c = self.canvas
        bw, bh, d = 84, 30, 16
        x0, x1 = cx - bw/2, cx + bw/2
        yt, yb = base_y - bh, base_y
        if low:
            ftop, fbot, sidec, lidc, edge = "#b06a52", "#8a4c39", "#763f31", "#c08376", "#5f3225"
        else:
            ftop, fbot, sidec, lidc, edge = "#dab074", "#ad8245", "#946d3b", "#ecc998", "#6f5230"
        c.create_polygon(x1,yt, x1+d,yt-d, x1+d,yb-d, x1,yb, fill=sidec, outline=edge, width=1)   # 右側面
        if top_lid:
            c.create_polygon(x0,yt, x0+d,yt-d, x1+d,yt-d, x1,yt, fill=lidc, outline=edge, width=1)  # 上蓋
            c.create_line(cx, yt, cx+d/2, yt-d/2, fill=edge)                                        # 蓋縫
        self.vgrad(x0+1, yt+1, x1-1, yb-1, ftop, fbot, 10)                                         # 正面漸層
        c.create_rectangle(x0, yt, x1, yb, outline=edge, width=1)
        c.create_line(x0+2, yt+2, x1-2, yt+2, fill=self._lerp(ftop, "#ffffff", 0.35))              # 上緣高光
        c.create_line(cx, yt+5, cx, yb-2, fill=self._lerp(fbot, "#000000", 0.12))                  # 封箱縫
        cy = (yt + yb) / 2
        if kind == "med":                                                                          # 醫療:白底紅十字
            c.create_oval(cx-11, cy-11, cx+11, cy+11, fill="#f4f4f4", outline="#cfcfcf")
            c.create_rectangle(cx-3, cy-7, cx+3, cy+7, fill=RED, outline="")
            c.create_rectangle(cx-7, cy-3, cx+7, cy+3, fill=RED, outline="")
        elif kind == "food":                                                                       # 生鮮:橘子
            c.create_oval(cx-10, cy-9, cx+10, cy+11, fill="#e07d12", outline="#b5640c")            # 果身
            c.create_oval(cx-9, cy-8, cx+3, cy+2, fill="#f3a23a", outline="")                       # 受光面
            c.create_oval(cx-7, cy-7, cx-2, cy-2, fill="#ffce82", outline="")                       # 高光
            c.create_oval(cx-2, cy-11, cx+2, cy-7, fill="#9c560a", outline="")                      # 頂部凹點
            c.create_oval(cx, cy-14, cx+8, cy-9, fill="#3f9b3a", outline="#2c7029")                 # 葉子

    # ---- 警報器 ----
    def alarm(self, cx, cy, on):
        c = self.canvas
        if on:
            c.create_oval(cx-24, cy-16, cx+24, cy+18, outline="#7a2a12")                            # 暈光環
            for dx1,dy1,dx2,dy2 in [(-14,1,-23,-2),(-8,-6,-13,-13),(0,-8,0,-16),(8,-6,13,-13),(14,1,23,-2)]:
                c.create_line(cx+dx1, cy+dy1, cx+dx2, cy+dy2, fill="#ffd23f", width=2)
        c.create_rectangle(cx-12, cy+9, cx+12, cy+16, fill="#2e2e38", outline="#15151c")            # 底座
        c.create_arc(cx-13, cy-5, cx+13, cy+21, start=0, extent=180, style=tk.PIESLICE,
                     fill=(RED if on else "#6a6a72"), outline="#1a1a1a", width=1)                    # 燈罩
        if on:
            c.create_oval(cx-7, cy-1, cx-1, cy+5, fill="#ff9a8a", outline="")                        # 燈罩高光
        c.create_text(cx, cy+27, text="警報器", fill="white", font=("Sans", 9))

    def draw(self, inv):
        c = self.canvas; c.delete("all")
        self.vgrad(0, 0, W, FLOOR, "#32324c", "#232334", 22)      # 後牆漸層
        self.vgrad(0, FLOOR, W, H, "#4c4c66", "#37374a", 10)                       # 地板漸層
        c.create_line(0, FLOOR, W, FLOOR, fill="#5e5e7e")
        c.create_text(W/2, 26, text="智慧倉庫即時監控", fill="white", font=("Sans", 20, "bold"))

        total = sum(inv.get(NAME_MAP[k], (0,3))[0] for _, k in ZONES)
        c.create_text(W/2, 50, text=f"全倉總數:{total}", fill="#f4cf3a", font=("Sans", 13, "bold"))

        any_low = any(inv.get(NAME_MAP[k], (0,3))[0] < inv.get(NAME_MAP[k], (0,3))[1] for _, k in ZONES)
        self.alarm(58, 40, any_low)
        self.alarm(W-58, 40, any_low)
        if any_low:
            c.create_rectangle(0, 74, W, 100, fill=RED, outline="")
            c.create_text(W/2, 87, text="警報:有貨物庫存低於安全量!", fill="white", font=("Sans", 13, "bold"))

        xs = [W*0.24, W*0.5, W*0.76]
        for (label, kind), cx in zip(ZONES, xs):
            qty, th = inv.get(NAME_MAP[kind], (0, 3))
            low = qty < th
            c.create_oval(cx-52, FLOOR-7, cx+52, FLOOR+9, fill="#1c1c28", outline="")   # 地面陰影
            vis = min(qty, 7)
            for i in range(vis):
                self.carton(cx, FLOOR - 8 - i*30, kind, low, i == vis-1)
            if qty > 7:
                c.create_text(cx, FLOOR - 8 - 7*30 - 16,text=f"+{qty-7}",
                              fill="white", font=("Sans", 10, "bold"))
            c.create_text(cx, FLOOR+26, text=label, fill="white", font=("Sans", 13, "bold"))
            c.create_text(cx, FLOOR+48, text=f"庫存 {qty}  (門檻 {th})",
                          fill=(RED if low else "#dddddd"), font=("Sans", 11))

    def no_conn(self):
        c = self.canvas; c.delete("all")
        c.create_text(W/2, H/2, justify="center", fill="white", font=("Sans", 14),
                      text="未連線到倉庫系統\n請先在終端機執行  sudo ./warehouse")

    def poll(self):
        text = self.query()
        self.draw(self.parse(text)) if text else self.no_conn()
        self.root.after(1000, self.poll)

if __name__ == "__main__":
    root = tk.Tk()
    WarehouseView(root)
    root.mainloop()
