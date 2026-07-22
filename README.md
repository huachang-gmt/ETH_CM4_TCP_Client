# STM32H755ZI-Q Ethernet + LwIP 網路建立與 CM4 TCP Client

## 更新說明

```text
[2026-07-22]
1. 在原來的基礎上，新增加 data_client.c 與 data_client.h。 用來模擬 EtherCAT 來源資料給 tcp_client 發送出去。
2. 使用 32 個 buffer，每個 buffer 具有 400 byte 大小。
3. 寫入資料利用 write_index 從 buf[0] 寫到 buf[31] 然後覆蓋舊的資料繼續寫入 buf[0] ...如此反覆。
4. 每 1ms 產生一組資料。
5. 利用 read_index 表示 tcp_client 讀取資料，讀完一筆，read_index++，當 write_index==read_index，表示資料都讀完，而且送出。
6. 使用 32 個 buffer的目的是 保全完整的來源資料，如果 讀出 TCP 有些延遲，會有緩衝，可以防止資料遺失。
7. 目前已知的問題是，tcp_client 每次會丟出兩個 400 byte 資料封包，這兩個資料封包 sequence number 會是相同的。

    400 byte payload
    = 4 byte sequence + 308 byte pattern + 88 byte zero
```

## 專案簡介

本專案使用 STM32H755ZI-Q NUCLEO 開發板建立 Ethernet 網路功能，採用：

* STM32H755ZIT6
* Cortex-M4
* LAN8742A PHY
* RMII（Reduced Media Independent Interface）介面
* STM32 HAL Ethernet Driver
* LwIP TCP/IP Stack

最終目標：

* 建立穩定 Ethernet 通訊
* Ping 測試成功
* 建立 TCP Client 與 TCP Server 建立連線後，每次發送 資料 400 byte， 當 TCP server 回覆 ACK 就繼續傳送下一次 400 byte。

### 關鍵檔案：

* CM7\Core\Src\main.c
* CM4\Core\Src\main.c
* CM4\Core\Src\tcp_client.c
* CM4\Core\Inc\tcp_client.h
* CM4\Core\Src\data_client.c
* CM4\Core\Inc\data_client.h

## 在電腦端的 TCP Server 程式 ：  tcp_server.py

```python
import socket
import os
import threading
import sys
import time
import queue
from datetime import datetime

# 設定監聽位置
HOST = '0.0.0.0'
PORT = 8888
SAVE_DIR = r'D:\RaspberryPi\Python_TCP_Server\LogData'

# 每累積 4MB 就交給寫檔執行緒
WRITE_BLOCK_SIZE = 4 * 1024 * 1024

# Queue 最大允許數量
MAX_QUEUE_SIZE = 100

is_running = True

# 修改1：
# 使用 Queue 取代 list + lock
# Queue 本身就是 thread-safe
buffer_queue = queue.Queue(maxsize=MAX_QUEUE_SIZE)

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)

# ==========================================================
# 磁碟寫入執行緒
# ==========================================================
def disk_writer_thread():

    global is_running

    print("Disk Writer Thread 啟動")

    while is_running or not buffer_queue.empty():

        try:
            # 修改2：
            # 等待最多 1 秒
            data_to_write = buffer_queue.get(timeout=1)

        except queue.Empty:
            continue

        try:
            # 修改3：
            # 加入微秒避免檔名重複
            filename = os.path.join(
                SAVE_DIR,
                datetime.now().strftime('%Y%m%d-%H%M%S-%f.bin')
            )

            with open(filename, 'wb') as f:
                f.write(data_to_write)

            print(
                f"[寫入完成] "
                f"{os.path.basename(filename)} "
                f"{len(data_to_write)/1024/1024:.2f} MB "
                f"Queue={buffer_queue.qsize()}"
            )

        except Exception as e:
            print("寫檔錯誤:", e)

        finally:
            buffer_queue.task_done()

    print("Disk Writer Thread 結束")


# ==========================================================
# 鍵盤輸入 q 離開
# ==========================================================
def listen_for_quit():

    global is_running

    while True:

        cmd = sys.stdin.readline().strip().lower()

        if cmd == 'q':
            print("收到結束指令...")
            is_running = False
            break

# ==========================================================
# TCP Server
# ==========================================================
def start_server():

    global is_running

    threading.Thread(
        target=disk_writer_thread,
        daemon=True
    ).start()

    server_socket = socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM
    )

    server_socket.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_REUSEADDR,
        1
    )

    server_socket.setsockopt(
        socket.IPPROTO_TCP,
        socket.TCP_NODELAY,
        1
    )

    # 修改4：
    # 增加 Linux/Windows TCP Receive Buffer
    server_socket.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_RCVBUF,
        4 * 1024 * 1024
    )

    server_socket.bind((HOST, PORT))

    server_socket.listen(5)

    server_socket.settimeout(1)

    print("")
    print("===================================")
    print(f"TCP Server 啟動")
    print(f"Port : {PORT}")
    print(f"Write Block Size : {WRITE_BLOCK_SIZE/1024/1024:.1f} MB")
    print("===================================")

    while is_running:

        try:

            conn, addr = server_socket.accept()

            print("")
            print(f"Client Connected : {addr}")

            # 修改5：
            # 防止 recv 永遠卡住
            conn.settimeout(1)

            # 每個連線自己的 Buffer
            current_buffer = bytearray()

            total_received = 0

            start_time = time.time()

            while is_running:

                try:

                    data = conn.recv(65536)

                    if not data:
                        print("Client Disconnect")
                        break

                    current_buffer.extend(data)

                    total_received += len(data)

                    # 修改6：
                    # 每 4MB 就丟給 writer thread
                    if len(current_buffer) >= WRITE_BLOCK_SIZE:

                        try:

                            buffer_queue.put(
                                bytes(current_buffer),
                                timeout=5
                            )

                            print(
                                f"[Buffer Flush] "
                                f"{len(current_buffer)/1024/1024:.2f} MB "
                                f"Queue={buffer_queue.qsize()}"
                            )

                            current_buffer = bytearray()

                        except queue.Full:

                            print(
                                "警告：Disk Writer 跟不上，Queue 已滿"
                            )

                    # 每 10 秒顯示統計資訊
                    if time.time() - start_time >= 10:

                        speed = total_received / 1024 / 1024 / 10

                        print(
                            f"RX Speed = {speed:.2f} MB/s "
                            f"Queue={buffer_queue.qsize()}"
                        )

                        total_received = 0
                        start_time = time.time()

                except socket.timeout:
                    continue

                except Exception as e:
                    print("recv error:", e)
                    break

            # 修改7：
            # 關閉前把剩餘資料存檔
            if len(current_buffer) > 0:

                try:
                    buffer_queue.put(
                        bytes(current_buffer),
                        timeout=5
                    )

                    print(
                        f"[Final Flush] "
                        f"{len(current_buffer)/1024/1024:.2f} MB"
                    )

                except queue.Full:
                    print("Final Flush 失敗，Queue 已滿")

            conn.close()

        except socket.timeout:
            continue

        except Exception as e:
            print("accept error:", e)

    print("等待剩餘資料寫入磁碟...")

    buffer_queue.join()

    server_socket.close()

    print("Server 結束")


# ==========================================================
# Main
# ==========================================================
if __name__ == "__main__":

    threading.Thread(
        target=listen_for_quit,
        daemon=True
    ).start()

    start_server()

    os._exit(0)
```

```text
STM32H755 (Client)
        │
        │ TCP Data (400 Bytes)
        ▼
Windows TCP Driver
        │
        │ ① 收到封包
        │
        ├────────────► 立刻送 ACK
        │
        ▼
Socket Receive Buffer (SO_RCVBUF)
        │
        ▼
Python recv()
        │
        ▼
current_buffer (16MB)
        │
        ▼
Queue
        │
        ▼
Disk Writer Thread
        │
        ▼
SSD/HDD
```

