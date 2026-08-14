# STM32H755ZI-Q Ethernet + LwIP 網路建立與 CM4 TCP Client

## 更新說明

## [2026-08-14]

> 搭配的 TCP Server 為  tcp_server.py。 執行方式 在 Terminal 下執行 ： python3 tcp_server.py 程式貼在本次紀錄後面。

### 關鍵檔案：

* CM4\Core\Src\data_client.c
* CM4\Core\Inc\data_client.h
* CM4\Core\Src\tcp_client.c
* CM4\Core\Src\main.c

---

# TCP 即時性量測實驗

## 1. 實驗目的

本階段並非單純驗證 TCP 是否能夠成功傳輸資料，而是進一步量測：

> **STM32H7 使用 TCP/IP 傳送週期性資料時，從資料送出到 PC Server 收到資料的實際時間行為。**

本實驗特別關注：

* 1 ms 週期資料傳送是否能維持穩定週期
* TCP 傳輸過程是否會產生 jitter
* 是否會出現連續資料快速送出的情況
* 是否會出現較長時間沒有資料送出的情況
* TCP ACK 與 Application ACK 的時間關係
* STM32H7 端是否能透過 GPIO 將傳輸時間反映到示波器
* 實際波形是否符合嚴格 deterministic real-time 的要求

本專案對「即時性」的要求並不是單純追求高傳輸速度，而是：

> **每一個 1 ms 時間點都必須準時執行，不能因為系統負載、TCP/IP stack、網路狀態、ACK、buffer 或排程而產生不可接受的延遲與 jitter。**

因此，本次實驗的重點是 **deterministic timing**，而不是 throughput。

---

# 2. TCP Client 實驗架構

STM32H7 作為 TCP Client，PC 作為 TCP Server。

資料流程：

```text
STM32H7
   │
   │ 204-byte packet
   ▼
lwIP TCP/IP Stack
   │
   │ Ethernet
   ▼
PC Python TCP Server
   │
   │ Application ACK
   ▼
STM32H7
```

本階段使用：

```text
STM32H7
    ↓
lwIP TCP Client
    ↓
Ethernet
    ↓
PC Python TCP Server
```

PC Server 收到完整的 204-byte packet 後，另外回傳 4-byte Application ACK：

```text
AC 4B XX XX
```

其中：

```text
Byte 0 = 0xAC
Byte 1 = 0x4B
Byte 2 = ACK counter LSB
Byte 3 = ACK counter MSB
```

必須特別區分：

```text
TCP ACK
```

與：

```text
Application ACK
```

兩者不是同一件事情。

TCP ACK 是 TCP protocol 本身的確認機制；Application ACK 則是 Python Server 額外建立的應用層確認，用來協助本次時間量測。

---

# 3. STM32H7 TCP Client 修改內容

本階段主要修改：

```text
CM4/Core/Src/tcp_client.c
```

目的不是重新設計 TCP Client，而是在原有架構上加入**時間量測與除錯功能**。

---

## 3.1 使用 `data_client_get_packet()`

TCP Client 從 Ring Buffer 取得待傳送資料：

```c
current_packet = data_client_get_packet();
```

確認取得有效 packet 後，再交給 lwIP：

```c
tcp_write(
    tpcb,
    current_packet->data,
    current_packet->length,
    TCP_WRITE_FLAG_COPY);
```

本次實驗的 packet size 為：

```text
204 bytes
```

因此實際 TCP payload：

```text
204 bytes / packet
```

---

# 4. `data_client_release_packet()` 的時機

本次修改中特別確認一個非常重要的 TCP Buffer ownership 問題。

不能因為：

```c
tcp_write() == ERR_OK
```

就立即：

```c
data_client_release_packet();
```

因為：

> `tcp_write()` 成功只代表資料已經交給 lwIP TCP stack，並不代表 PC Server 已經收到資料。

因此本次設計改為：

```text
data_client_get_packet()
        │
        ▼
    tcp_write()
        │
        ▼
   等待 TCP ACK
        │
        ▼
    tcp_sent()
        │
        ▼
data_client_release_packet()
```

在：

```c
tcp_client_sent()
```

中：

```c
if(current_packet != NULL)
{
    data_client_release_packet();

    current_packet = NULL;
}
```

如此可以避免 Ring Buffer 中的資料在 TCP stack 尚未完成使用前就被釋放或覆蓋。

---

# 5. TCP ACK Credit

加入：

```c
static volatile uint32_t tx_credit = 0;
```

每次 TCP ACK 回來：

```c
tx_credit += len;
```

而送出 204-byte packet 時：

```c
tx_credit -= current_packet->length;
```

初始值設定：

```c
tx_credit = DATA_PACKET_SIZE;
```

因此第一個 packet 可以送出。

之後必須等 TCP ACK 產生新的 credit，才能繼續送下一個 packet。

形成：

```text
Send packet
     ↓
tx_credit -= 204
     ↓
等待 TCP ACK
     ↓
tcp_sent()
     ↓
tx_credit += 204
     ↓
Send next packet
```

這個設計也讓本次實驗可以觀察：

> TCP transmission 與 TCP ACK timing 對資料傳送週期的影響。

---

# 6. Application ACK 接收

加入：

```c
tcp_client_recv()
```

用來接收 Python Server 回傳的 Application ACK。

程式確認：

```c
ack[0] == 0xAC
ack[1] == 0x4B
```

後取得 ACK counter。

並記錄：

```c
app_ack_count
last_app_ack
```

這讓 STM32 可以確認：

```text
Python Server
      ↓
收到 packet
      ↓
送出 Application ACK
      ↓
STM32 收到 ACK
```

---

# 7. 使用 PC6 作為示波器量測腳位

為了避免單純依靠 software log 判斷時間，本次實驗加入實體 GPIO timing signal。

使用：

```text
NUCLEO-H755ZI-Q
CN7 Pin 1
```

對應：

```text
PC6
```

PC6 被設定為：

```c
GPIO_MODE_OUTPUT_PP
```

並使用：

```c
GPIO_NOPULL
GPIO_SPEED_FREQ_VERY_HIGH
```

初始化完成後預設：

```text
PC6 = LOW
```

---

# 8. PC6 波形定義

PC6 的定義為：

```text
HIGH
    ↓
STM32 已將 204-byte packet 交給 lwIP TCP stack
```

而：

```text
LOW
    ↓
STM32 收到 Python Server 回傳的 Application ACK
```

因此示波器觀察的是：

```text
PC6 HIGH
    │
    │  TCP packet transmission / network round trip
    │
PC6 LOW
```

概念上：

```text
                 TCP / Ethernet / PC
                 ───────────────────>

STM32                         PC
  │                            │
  │── 204-byte packet ────────>│
  │                            │
  │<── Application ACK ────────│
  │                            │
  ▼
 PC6 LOW
```

---

# 9. GPIO Timing Start

當：

```c
tcp_write() == ERR_OK
```

之後：

```c
HAL_GPIO_WritePin(
    GPIOC,
    GPIO_PIN_6,
    GPIO_PIN_SET);
```

因此：

```text
PC6 = HIGH
```

表示這一個 packet 已經交給 lwIP TCP stack。

接著：

```c
tcp_output(tpcb);
```

要求 lwIP 儘快輸出 TCP data。

---

# 10. GPIO Timing End

當 STM32 收到 Python Server 的 Application ACK：

```c
if((ack[0] == 0xAC) &&
   (ack[1] == 0x4B))
```

執行：

```c
HAL_GPIO_WritePin(
    GPIOC,
    GPIO_PIN_6,
    GPIO_PIN_RESET);
```

因此：

```text
PC6 = LOW
```

示波器上的 HIGH 寬度可以用來觀察：

```text
packet 交給 TCP stack
        ↓
Ethernet
        ↓
PC Server 收到
        ↓
Python Application ACK
        ↓
Ethernet
        ↓
STM32 收到 ACK
```

這是一個非常重要的實驗結果。

**它不是單純的「TCP packet 在網路上傳輸需要多久」**，而是整個實驗鏈路所觀察到的 application-level round-trip timing。

---

# 11. 連線異常時 PC6 強制 LOW

在：

```c
tcp_client_err()
```

以及：

```c
p == NULL
```

的情況下，也會將：

```text
PC6 = LOW
```

這樣可以避免 TCP 連線異常時，GPIO 永久停留在 HIGH。

因此：

```text
正常：

HIGH → Application ACK → LOW


異常斷線：

HIGH → connection error → LOW
```

這只是為了讓示波器的狀態具有明確意義，並不是額外的資料傳輸機制。

---

# 12. PC Python Server 的修改

PC 端建立 Python TCP Server，監聽：

```text
Port = 8888
```

Server 除了接收資料，也增加了監控資訊。

Terminal 不再逐 packet 大量列印：

```text
[TCP] recv() = 204 bytes
[PACKET] #00000001
[PACKET] #00000002
[PACKET] #00000003
...
```

因為這種輸出速度過快，反而不利於觀察。

改成固定格式的監控畫面，定期更新：

```text
STM32 TCP CLIENT MONITOR
```

顯示：

* Connection Status
* Client IP
* Client Port
* Connected Time
* Packets
* Bytes
* RX Rate
* Data Rate
* Average Rate
* Last Packet
* Invalid Packet
* Application ACK
* ACK Error
* recv() Calls
* Disk Queue
* Last Error

因此可以在不讓 Terminal 大量滾動的情況下，觀察 STM32 Client 的整體傳輸狀態。

---

# 13. 實驗觀察結果

實際測試時，PC Server 可以穩定收到 STM32H7 傳送的 204-byte packet。

例如：

```text
Packets         : 6,486
Bytes           : 1.26 MB
RX Rate         : 1,590.6 packets/s
Data Rate       : 316.88 KB/s
Invalid Packet  : 0

Application ACK
ACK Sent        : 6,486
ACK Error       : 0
```

這證明：

```text
STM32H7
   ↓
lwIP
   ↓
Ethernet
   ↓
PC Python Server
```

資料傳輸本身是成功且穩定的。

但是：

> **高 throughput 並不等於 deterministic real-time。**

這也是本次實驗最重要的結論。

---

# 14. 示波器觀察到的問題

示波器並沒有呈現理想中的：

```text
HIGH ┌────┐       ┌────┐       ┌────┐
     │    │       │    │       │    │
LOW ─┘    └───────┘    └───────┘    └──
       1ms             1ms
```

而可能出現：

### 情況一：連續 HIGH

```text
HIGH ┌─────────────────────────────┐
     │                             │
LOW ─┘                             └────
       packet1 packet2 packet3
```

代表多個資料傳送事件可能在非常接近的時間內發生。

---

### 情況二：HIGH / LOW 間隔不規律

```text
HIGH ┌──────┐       ┌────────┐
     │      │       │        │
LOW ─┘      └───────┘        └────────
```

代表資料傳送與 ACK 回來的時間不是固定 deterministic interval。

---

# 15. 實驗最重要的結論

本次實驗證明：

> TCP/IP 可以具有很好的資料傳輸能力，但「傳得快」與「精準即時」是兩個不同的問題。

TCP/IP stack、Ethernet MAC、PHY、交換器、PC OS、Python application、TCP congestion/flow control、buffering、ACK timing 等因素，都可能使資料實際到達時間產生變化。

因此，即使平均：

```text
1,000 packets/s
```

也不能直接推論：

```text
每一個 packet 都會精準地在每 1 ms 時刻送出。
```

更不能保證：

```text
Jitter = 0
```

---

# 16. 本專案的技術決策

本專案對即時資料的要求是：

> **Deterministic 1 ms timing，而不是單純高 throughput。**

因此經過 STM32H7 + lwIP + Ethernet + PC Python Server 的實際示波器量測後，決定：

```text
TCP/IP
  ↓
不作為本專案嚴格 1 ms deterministic real-time
data path
```

本階段因此停止 TCP 即時傳輸方向的進一步最佳化。

後續改採：

```text
STM32H7
   │
   │ SPI
   ▼
Target Device
```

作為即時資料傳輸方向。

---

# 17. 本次實驗的價值

雖然最後沒有採用 TCP 作為即時資料傳輸方式，但這次實驗仍然非常重要。

因為它實際驗證了：

```text
Software log
     ↓
TCP Client
     ↓
lwIP
     ↓
Ethernet
     ↓
PC Server
     ↓
Application ACK
     ↓
GPIO
     ↓
Oscilloscope
```

並且把原本抽象的「TCP 是否即時」問題，轉換成了可以實際量測的 electrical timing waveform。

因此本次實驗的核心成果不是：

> 「TCP 傳輸速度不夠快」

而是：

> **TCP/IP 的 throughput 可以很好，但對本專案要求的 deterministic 1 ms real-time timing，無法提供所需要的時間確定性。**

這也是本次實驗最重要的工程結論。



---
### 使用示波器量測波形後產生的疑惑
```text
觀看示波器的 波形，有幾個問題 ： 
1. 雖然高電位的時間在 1ms 以內，但會出現連續高電位，也就是不只 1ms 的波形，這個感覺很像是補送資料。而且這樣的波形還蠻常出現的。為什麼會這樣，這樣的波形出現讓人覺得使用 TCP/IP 傳送資料沒辦法做到即時性 real-time。
2. 有高電位也有低電位，為什麼會有低電位時間，而且這個時間還滿久的，解釋一下。
3. 如果要達到即時性，若是與 SPI 比較，顯然 TCP Client 網路傳送很不可靠，還不如使用 SPI。你認為我的同事說法正確嗎？
4. 針對 必要的 即時性資料或是控制，使用網路傳輸是不可靠，也不該用。另外，嵌入式系統，也就是有 OS 作業系統的設備，因為 OS的安排，也不適合用來做即時性工作，例如要很準時每 1 ms 精準的執行資料傳送工作。所以，樹莓派不如MCU。這些也都是我的疑惑，我說的OS，不是RTOS之類的。
```
### 回答

你看到的現象，其實非常有價值。**它不是單純「TCP 不可靠」的證據，反而剛好把 TCP、Ethernet、Linux/一般 OS 與 MCU 即時性的差異暴露出來了。**

先給你最重要的結論：

> **你現在示波器看到的波形，不能直接解讀成「TCP 補送資料」或「TCP 不可靠」。**
>
> TCP 本身是可靠傳輸協定；你現在看到的連續 HIGH、長 LOW，主要反映的是 **STM32 lwIP 的排程/ACK 機制、TCP buffering、網路 stack 延遲，以及你目前 GPIO 測量點的定義**。

而且，我要特別修正我們目前實驗的一個觀念：

### 你現在量到的不是「1 ms 資料週期」

你現在：

```c
tcp_write(...);

HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);

tcp_output(tpcb);
```

然後等 Python Application ACK：

```text
Python 收到 204 bytes
        ↓
Python send(4-byte ACK)
        ↓
STM32 tcp_client_recv()
        ↓
PC6 LOW
```

所以 PC6 的 HIGH 時間實際上量的是：

> **從 STM32 呼叫 tcp_write() 後，到 Python Server 收到資料、處理 ACK，再回到 STM32 lwIP receive callback 的整個路徑。**

它不是：

> 「Ethernet 線上 204 bytes 的傳輸時間」。

這一點非常重要。

---

# 1. 為什麼會出現連續 HIGH？

你同事說「看起來像補送資料」，**觀察是對的，但原因不一定是補送**。

假設理想情況：

```text
PC6

HIGH ┌──────┐       ┌──────┐       ┌──────┐
     │      │       │      │       │      │
LOW ─┘      └───────┘      └───────┘      └──

      packet1        packet2        packet3
```

但是 TCP 實際上可能是：

```text
HIGH ┌────────┐ ┌────────┐       ┌───────┐
     │        │ │        │       │       │
LOW ─┘        └─┘        └───────┘       └──
```

甚至：

```text
HIGH ┌─────────────────────────────┐
     │                             │
LOW ─┘                             └──────
       packet1 packet2 packet3
```

這並不表示 TCP 把 packet1 「補送」了。

---

## 真正的原因之一：你的 GPIO HIGH 太早

這是目前實驗中非常重要的一點。

你把 HIGH 放在：

```c
err = tcp_write(...);

if(err == ERR_OK)
{
    tx_credit -= current_packet->length;

    HAL_GPIO_WritePin(
        GPIOC,
        GPIO_PIN_6,
        GPIO_PIN_SET);

    tcp_output(tpcb);
}
```

`tcp_write()` 成功只代表：

> **資料已經成功交給 lwIP TCP stack。**

它不代表：

> Ethernet PHY 已經送出。

甚至不代表：

> TCP segment 已經離開 STM32。

所以：

```text
tcp_write()
   ↓
PC6 HIGH
   ↓
tcp_output()
   ↓
lwIP
   ↓
TCP/IP
   ↓
Ethernet MAC
   ↓
PHY
   ↓
網路線
   ↓
Switch
   ↓
PC
```

因此 HIGH 已經開始了，但真正的 Ethernet transmission 可能還沒開始。

---

# 2. 更重要：為什麼 HIGH 會連在一起？

這和 TCP buffering 有很大的關係。

TCP 並不是：

```text
write 204
↓
送出
↓
ACK
↓
write 204
↓
送出
```

這麼簡單。

TCP 的概念比較接近：

```text
Application
     ↓
    TCP
     ↓
TCP Send Buffer
     ↓
TCP Segmentation
     ↓
IP
     ↓
Ethernet
```

所以你的：

```c
tcp_write()
```

只是把資料放入 TCP transmission queue。

而：

```c
tcp_output()
```

要求 lwIP 儘快把 queue 裡的資料送出去。

因此可能發生：

```text
packet #1
    ↓
tcp_write
    ↓
HIGH

packet #1 transmission

ACK

packet #2
    ↓
tcp_write
    ↓
HIGH

packet #2 transmission
```

如果 ACK 回來得非常快，PC6 的 LOW 可能非常短。

示波器看起來就會變成：

```text
      HIGH HIGH HIGH HIGH
       ┌──┐┌──┐┌──┐┌──┐
───────┘  └┘  └┘  └┘  └────
```

在示波器的時間尺度或觸發方式下，它甚至可能看起來像一個很長的 HIGH。

---

# 3. 但還有另一個非常重要的問題

你的 Application ACK 是：

```text
STM32
  ↓
204 bytes
  ↓
Python
  ↓
4-byte ACK
  ↓
STM32
```

而 Python 的 ACK 是 application-level ACK。

所以你現在實驗實際上是：

```text
STM32
  │
  │ 204 bytes
  ▼
Python Server
  │
  │ 4-byte ACK
  ▼
STM32
```

這個 HIGH/LOW 波形反映的是：

> **整個 round-trip latency**

不是單向傳輸 latency。

---

# 4. 那為什麼會有很長的 LOW？

這個現象也很正常，而且非常值得理解。

你的程式：

```c
while(tx_credit >= DATA_PACKET_SIZE)
```

而初始：

```c
tx_credit = DATA_PACKET_SIZE;
```

所以：

```text
送出 packet
↓
tx_credit = 0
↓
停止送
↓
等待 tcp_sent()
↓
tx_credit += 204
↓
再次送 packet
```

也就是：

```text
packet #1
    ↓
等待 TCP ACK
    ↓
packet #2
    ↓
等待 TCP ACK
    ↓
packet #3
```

你現在其實刻意建立了一個：

> **Stop-and-Wait TCP transmission model**

---

# 5. 所以 LOW 很長代表什麼？

例如：

```text
HIGH
│
│ 送出 packet
│
LOW
│
│ 等待 ACK
│
HIGH
```

LOW 可能包含：

```text
STM32 TCP processing
       +
Ethernet transmission
       +
Switch forwarding
       +
PC TCP processing
       +
Python socket processing
       +
Python application processing
       +
ACK transmission
       +
STM32 Ethernet RX
       +
lwIP RX processing
       +
tcp_client_recv()
```

所以 LOW 不代表：

> 「TCP 沒有能力傳資料。」

而是：

> **你目前的程式刻意等 ACK 才允許下一個 packet。**

這點非常重要。

---

# 6. 所以你說「TCP/IP 不適合 real-time」對不對？

### 部分正確，但需要非常精確地說。

如果說：

> 「TCP 不可靠，所以不能 real-time。」

❌ **這句不正確。**

TCP 是可靠傳輸協定。

它具有：

* sequence number
* acknowledgement
* retransmission
* flow control
* congestion control
* ordered delivery

所以 TCP 的設計目標本來就是：

> **可靠地把資料送到對方。**

---

但如果說：

> 「TCP/IP 在一般 Ethernet + 一般 OS 上，很難提供嚴格 deterministic 的 1 ms real-time guarantee。」

✅ **這句是正確的。**

這是完全不同的概念。

---

# 7. Real-time 最重要的不是「快」

這是你現在這個實驗最值得理解的地方。

很多人會把：

```text
高速 = real-time
```

但其實不是。

Real-time 更重要的是：

> **Deadline guarantee**

例如：

```text
每 1 ms 必須完成一次控制
```

不是：

```text
平均 1 ms
```

而是：

```text
1.000 ms
1.001 ms
0.999 ms
1.000 ms
...
```

而且最重要的是：

> **不能偶爾突然變成 5 ms、10 ms、20 ms。**

---

# 8. 舉一個很簡單的例子

假設兩種系統：

### 系統 A

```text
99.999% = 100 μs
0.001%  = 50 ms
```

### 系統 B

```text
100% = 900 μs ~ 950 μs
```

如果是控制系統：

**B 反而比較適合 real-time。**

因為它 deterministic。

---

# 9. 這就是 TCP 最大的問題之一

不是：

> TCP 很慢。

而是：

> **TCP latency 不具有嚴格 deterministic guarantee。**

TCP 可能因為：

```text
buffer
ACK
retransmission
flow control
queue
OS scheduling
interrupt latency
network congestion
switch buffering
```

而產生 latency variation，也就是：

> **jitter**

---

# 10. 那 SPI 和 TCP 比較呢？

你說：

> 「如果要 real-time，SPI 比 TCP 好。」

### 在某些 MCU 本地控制場景，這個方向是正確的。

SPI 是：

```text
MCU
 │
 │ clock
 │
 ▼
Peripheral
```

Master 可以直接控制：

```text
SCLK
CS
MOSI
MISO
```

例如：

```text
SPI clock = 20 MHz
```

時序非常直接。

沒有：

```text
TCP
IP
ARP
Ethernet
Switch
socket
OS
ACK
retransmission
```

這一整套協定堆疊。

因此如果你要：

> MCU ↔ ADC / DAC / FPGA / Sensor
> 每 1 ms 固定取得資料

SPI 非常適合。

---

# 11. 但不能因此說「SPI 一定比 Ethernet 快」

這也要修正。

例如：

```text
1 Gbps Ethernet
```

比：

```text
SPI 1 MHz
```

快很多。

所以：

> **吞吐量**

和：

> **deterministic latency**

是兩個不同問題。

---

# 12. 真正適合工業 Real-time Ethernet 的東西是什麼？

這就會回到你之前研究的：

> **EtherCAT**

這也是為什麼 EtherCAT 存在。

EtherCAT 仍然使用 Ethernet physical layer，但它不是：

```text
普通 TCP/IP
```

它走的是：

```text
EtherCAT frame
```

而不是：

```text
TCP
 ↓
IP
 ↓
Ethernet
```

EtherCAT 的設計目標就是：

> **high-speed deterministic industrial communication**

所以：

```text
Ethernet
```

不等於：

```text
TCP/IP
```

也不等於：

```text
real-time Ethernet
```

這三個概念一定要分開。

---

# 13. 這也是為什麼你之前學 EtherCAT 非常重要

你之前研究訊成科技 EtherCAT + STM32H755，其實剛好碰到這個核心概念。

可以把它簡化成：

```text
普通 TCP/IP

Application
    ↓
TCP
    ↓
IP
    ↓
Ethernet
    ↓
PHY
```

它強調：

> reliability / interoperability / general networking

---

而 EtherCAT：

```text
Application / Control
        ↓
    EtherCAT
        ↓
    Ethernet PHY
```

它強調：

> deterministic cyclic communication

---

# 14. 接下來談你第四個問題：Raspberry Pi vs MCU

你說：

> 「有 OS 的設備不適合做很準時的 1 ms 工作。」

### 這個說法「大方向正確」，但不能說得太絕對。

如果你指的是：

> 一般 Raspberry Pi + Linux

那麼：

**不適合拿來做嚴格 deterministic 1 ms hard real-time control。**

這個判斷是合理的。

---

# 15. 為什麼 Linux 不適合嚴格 1 ms？

因為 Linux 是通用作業系統。

CPU 同時可能處理：

```text
你的 application
network
USB
filesystem
SD card
memory management
kernel
interrupt
drivers
other processes
```

例如你寫：

```c
while(1)
{
    wait_until_1ms();
    send_data();
}
```

你以為：

```text
1 ms
1 ms
1 ms
1 ms
```

實際可能：

```text
1.00 ms
1.01 ms
0.99 ms
1.02 ms
1.00 ms
3.72 ms   ← OS / interrupt / scheduling
1.00 ms
```

---

# 16. 但 Linux 不是「完全不能 real-time」

這點也非常重要。

Linux 可以透過：

* PREEMPT_RT
* CPU isolation
* real-time scheduling
* IRQ affinity
* memory locking
* dedicated CPU
* real-time network techniques

把 latency 做得非常好。

但是：

> **一般 Linux ≠ hard real-time system**

這才是正確說法。

---

# 17. MCU 為什麼通常更適合？

像 STM32H7：

```text
CPU
 │
 ├── Timer
 ├── DMA
 ├── SPI
 ├── Ethernet
 ├── ADC
 └── GPIO
```

你可以使用：

```text
Hardware Timer
      ↓
interrupt
      ↓
DMA
      ↓
SPI/Ethernet
```

例如：

```text
Timer = 1 kHz
```

硬體 timer 每：

```text
1 ms
```

產生事件。

這件事情不需要 Linux scheduler 幫你決定。

所以 deterministic 能力通常更好。

---

# 18. 但 MCU 也不是自動變成 real-time

這一點也很重要。

如果 MCU 裡面寫：

```c
while(1)
{
    HAL_Delay(1);
}
```

然後：

```text
printf()
SD card
Ethernet
大量 interrupt
blocking function
```

一樣可能產生 jitter。

所以：

> **MCU 提供比較好的 deterministic 基礎，但程式架構仍然決定 real-time performance。**

---

# 19. 最後回到現在這個實驗

我反而認為現在做的實驗非常成功。

因為你已經親眼看到：

```text
STM32
   ↓
TCP/IP
   ↓
Ethernet
   ↓
Python
   ↓
Application ACK
   ↓
TCP/IP
   ↓
STM32
```

不是：

```text
1 ms
1 ms
1 ms
1 ms
```

這種完全 deterministic 的世界。

你們看到了：

```text
HIGH
LOW
HIGH
LOW
HIGH────────
LOW
HIGH
LOW────
```

這就是實際 network stack 的行為。

---

# 20. 但我要提醒你一個非常重要的實驗結論

**目前不要把 PC6 波形直接拿來證明「TCP 傳輸時間」。**

因為目前 PC6：

```c
tcp_write()
↓
PC6 HIGH
↓
tcp_output()
```

而 LOW：

```c
Python ACK
↓
tcp_client_recv()
↓
PC6 LOW
```

因此你量的是：

> **STM32 application → TCP stack → network → Python application → network → STM32 lwIP receive callback**

也就是一種：

### Application-level round-trip latency

不是純 TCP packet transmission time。

---

# 21. 這也解釋了你們看到的「連續 HIGH」

你同事看到：

> 「怎麼會連續 HIGH？是不是補送？」

我的判斷是：

**不能這樣下結論。**

目前更合理的解釋是：

```text
tcp_write()
   ↓
PC6 HIGH
   ↓
TCP buffering / transmission
   ↓
ACK
   ↓
PC6 LOW
```

而 ACK 回來的時間和下一次送出的時間可能非常接近，因此 HIGH/LOW pulse 會受到：

* lwIP processing
* Ethernet MAC
* PHY
* switch
* Windows network stack
* Python socket
* Python scheduling
* application processing

影響。

---

# 22. 我建議我們下一階段做一個非常重要的改進

不要再讓一個 GPIO 同時承擔「開始」和「結束」的模糊意義。

我們可以做成：

```text
PC6 = TX START
PC7 = APPLICATION ACK
```

例如：

```text
PC6
     ┌────┐
─────┘    └────────
     TX

PC7
             ┌────┐
─────────────┘    └──
             ACK
```

這樣示波器就能直接看到：

```text
        <---- Round Trip ---->

PC6  ┌──┐
─────┘  └──────────────────────

PC7                 ┌──┐
────────────────────┘  └────────
```

更進一步，我們甚至可以做：

```text
CH1 = TX start
CH2 = tcp_sent()
CH3 = Application ACK
```

這會非常有價值。

因為你們就能把三件事情完全分開：

### A

```text
tcp_write()
```

### B

```text
tcp_sent()
```

代表：

> TCP ACK 到達 STM32

### C

```text
tcp_client_recv()
```

代表：

> Python application ACK 到達 STM32

然後你們就可以第一次真正看到：

```text
TX
│
├──────── TCP ACK latency ────────┤
│
└──────── Application ACK latency ───────────────┘
```

這會比目前 PC6 單線波形**有意義非常多**。

---

# 23. 最後幫你把四個問題下結論

| 你同事/你的疑問                            | 判斷                                                      |
| ----------------------------------- | ------------------------------------------------------- |
| 連續 HIGH = TCP 補送？                   | ❌ 不能這樣判斷                                                |
| TCP 是不可靠的？                          | ❌ TCP 本身是可靠傳輸                                           |
| TCP/IP 能保證嚴格 1 ms？                  | ❌ 一般 TCP/IP 無法提供 deterministic hard-real-time guarantee |
| SPI 適合 MCU 即時控制？                    | ✅ 通常非常適合                                                |
| SPI 一定比 Ethernet 快？                 | ❌ 不一定                                                   |
| 普通 Ethernet = Real-time Ethernet？   | ❌ 不是                                                    |
| EtherCAT 適合 deterministic control？  | ✅ 是                                                     |
| 一般 Linux 適合 hard 1 ms control？      | ❌ 通常不適合                                                 |
| Linux 完全不能做 real-time？              | ❌ 太絕對，RT Linux 等技術可以改善                                  |
| MCU 一定是 real-time？                  | ❌ 還要看硬體與程式設計                                            |
| Raspberry Pi 適合資料記錄/網路服務？           | ✅ 非常適合                                                  |
| STM32 適合 deterministic I/O/control？ | ✅ 通常更適合                                                 |

---

### 最重要的一句話

你們這次實驗真正證明的不是：

> **「TCP 很慢，所以不能用。」**

而是：

> **「一般 TCP/IP 是可靠的資料傳輸機制，但它的 latency 與 jitter 並沒有提供嚴格 deterministic 的 hard-real-time 保證；因此它和 SPI、EtherCAT、即時控制匯流排的設計目標不同。」**

這個結論是正確的。

而且你們現在已經進入一個很有價值的階段：**不要再猜波形代表什麼，我們下一步可以直接用 2～3 個 GPIO channel，把 `tcp_write → tcp_sent → Application ACK` 三個時間點在示波器上分離出來。**這樣你和同事看到的每一個 HIGH、LOW、jitter，都可以有明確的技術意義。



### 問題
```text
我要強調的是精準的即時性，傳輸快反而是其次問題，我認為的即時性是不管如何，1ms 時間到就必須準時執行，不能有任何延誤或是 jitter，所以，使用網路傳輸做不到，這樣的說法是否正確？其次，波形：
HIGH ┌────────┐ ┌────────┐       ┌───────┐
     │        │ │        │       │       │
LOW ─┘        └─┘        └───────┘       └──
和
HIGH ┌─────────────────────────────┐
     │                             │
LOW ─┘                             └──────
       packet1 packet2 packet3
就是最大問題所在，期望的是很穩定的 1ms 高電位規律間隔產出，我列出的兩個有問題的波形是不是可以透過修改程式排除呢？
還有一個問題，embeded linux system 真的無法做到 即時性，即使使用timer 中斷，也不可靠，一定要用標準的 RTOS，像是 FreeRTOS之類才行，這種說法正確嗎？
```


### 回答
對，你這次把「即時性」定義得非常清楚了。我也要把前一個回答再收斂得更精確一些。

你所說的即時性是：

> **每 1 ms 到達 deadline 時，工作必須在規定時間內完成，而且 latency/jitter 必須受到嚴格上限控制；不是「平均約 1 ms」，也不是「大部分時間 1 ms」。**

如果是這個定義，那我們討論的就是 **hard real-time / deterministic real-time**。

---

# 一、先回答你的核心問題：普通 TCP/IP 能不能保證你要求的「精準 1 ms」？

### 一般的 Ethernet + TCP/IP：不能提供這種保證。

更精確地說：

> **不能保證「每 1 ms 絕對準時、零 jitter、任何情況都不延誤」。**

原因不是 TCP「傳輸速度不夠」，而是它的整個系統缺乏你要求的 **deterministic upper bound**。

你的資料路徑是：

```text
STM32 Timer / Application
        ↓
       lwIP
        ↓
       TCP
        ↓
       IP
        ↓
    Ethernet MAC
        ↓
       PHY
        ↓
    Ethernet network
        ↓
    PC Ethernet
        ↓
    Windows network stack
        ↓
       Python
```

任何一層都可能產生變動。

所以你要求：

```text
0 ms      1 ms      2 ms      3 ms      4 ms
│─────────│─────────│─────────│─────────│
    ↑         ↑         ↑         ↑
   必須      必須      必須      必須
   準時      準時      準時      準時
```

這種 **hard deadline**，普通 TCP/IP 不會給你這個 guarantee。

---

# 二、但是「使用網路傳輸做不到」這句話要稍微修正

如果你說：

> **「任何網路傳輸都做不到 hard real-time。」**

❌ 不夠精確。

如果你說：

> **「一般 TCP/IP 網路通訊不適合拿來提供我要求的、具有嚴格 deterministic deadline 的 hard real-time 傳輸保證。」**

✅ 這是正確的。

因為有：

* EtherCAT
* PROFINET IRT
* TSN
* EtherNet/IP 某些 real-time mechanisms
* 其他 deterministic industrial Ethernet 技術

它們就是利用 Ethernet 的 physical/network infrastructure，設計 deterministic communication。

所以問題不是：

> **Ethernet 天生不能 real-time。**

而是：

> **普通 TCP/IP stack 的設計目標不是 hard real-time deterministic communication。**

---

# 三、你現在看到的第一種波形，能不能靠修改程式消除？

你說的是：

```text
HIGH ┌────────┐ ┌────────┐       ┌───────┐
     │        │ │        │       │       │
LOW ─┘        └─┘        └───────┘       └──
```

### 可以「改善」，但不能靠程式保證完全消失。

而且這裡有一個非常重要的原因：

你目前的 GPIO HIGH 是：

```c
tcp_write(...);

HAL_GPIO_WritePin(
    GPIOC,
    GPIO_PIN_6,
    GPIO_PIN_SET);

tcp_output(tpcb);
```

所以 PC6 的 HIGH 並不是：

> 「Ethernet 線上開始傳輸的精確時間」。

它代表：

> 「application 已經把資料交給 lwIP TCP stack」。

因此你看到的：

```text
HIGH
LOW
HIGH
LOW
```

本身並沒有問題。

---

# 四、但你的第二種波形確實是我們應該深入研究的

你說：

```text
HIGH ┌─────────────────────────────┐
     │                             │
LOW ─┘                             └──────
       packet1 packet2 packet3
```

這代表你的測量訊號在很長一段時間維持 HIGH。

這對你「每一個 packet 都應該有清楚的 1 ms 週期」的目標而言，確實不是理想波形。

但是我要很慎重地說：

> **不能直接看到這個波形就判定 TCP 正在補送 packet1、packet2、packet3。**

我們需要先知道：

### PC6 到底代表什麼。

目前它代表：

```text
tcp_write() 成功
       ↓
PC6 HIGH
       ↓
Python Application ACK
       ↓
PC6 LOW
```

所以如果 packet #1 的 Application ACK 很慢：

```text
packet #1
    ↓
HIGH────────────────────
                         ↓
                    Application ACK
                         ↓
                        LOW
```

那麼下一個 packet 才會開始。

---

# 五、其實你現在的程式刻意限制了 throughput

這一點非常重要。

你現在：

```c
tx_credit = DATA_PACKET_SIZE;
```

然後：

```c
while(tx_credit >= DATA_PACKET_SIZE)
```

送一包：

```c
tx_credit -= current_packet->length;
```

變成：

```text
tx_credit = 0
```

所以：

```text
packet #1
   ↓
tx_credit = 0
   ↓
停止
   ↓
等待 tcp_sent()
   ↓
tx_credit = 204
   ↓
packet #2
```

你其實是：

> **一個 packet → 等 TCP ACK → 下一個 packet**

這是一種 stop-and-wait。

因此你現在的程式本身就會產生：

```text
HIGH
    ↓
等待
    ↓
LOW
    ↓
下一包
```

所以我們現在看到的波形，**不能拿來證明「TCP 天生只能這樣」**。

因為我們的 STM32 client 還沒有做真正的 pipeline。

---

# 六、那能不能修改成穩定的 1 ms？

這裡必須分成兩個問題。

## A. STM32「產生資料」每 1 ms

### 可以做到非常穩定。

例如：

```text
Hardware Timer
      ↓
每 1 ms
      ↓
產生資料
      ↓
Ring Buffer
```

STM32 timer 是硬體計時器，不需要 Linux scheduler。

這部分可以做到非常 deterministic。

---

## B. TCP「每 1 ms 保證把資料送到 PC」

### 不能保證。

即使你把程式寫得非常漂亮：

```text
Timer
 ↓
DMA
 ↓
lwIP
 ↓
Ethernet
```

你仍然不能說：

> 「任何情況下，PC 一定在 deadline 前收到。」

因為 network path 本身不是 hard deterministic。

---

# 七、所以你真正應該區分兩個 deadline

這個觀念非常重要。

### Deadline 1

```text
STM32
每 1 ms
↓
產生資料
```

這可以做到非常精準。

### Deadline 2

```text
STM32
每 1 ms
↓
TCP
↓
PC
```

這不能由普通 TCP/IP 保證。

所以如果你的系統是：

> EtherCAT control data 必須每 1 ms 準時執行

通常做法不是：

```text
MCU
 ↓
TCP/IP
 ↓
Raspberry Pi
 ↓
Control
```

而是：

```text
        ┌───────────────┐
        │ MCU / EtherCAT│
        │ real-time     │
        └───────┬───────┘
                │
          deterministic
                │
        ┌───────▼───────┐
        │ Control       │
        └───────────────┘

                │
                │ 非即時資料
                ▼

             TCP/IP
                ↓
        Raspberry Pi
                ↓
       Logging / Storage
```

這個架構非常合理。

---

# 八、這也回答你第二個問題：Embedded Linux 能不能 real-time？

你問：

> embedded Linux 真的無法做到即時性，即使使用 timer interrupt 也不可靠，一定要標準 RTOS，例如 FreeRTOS？

### 答案是：

**「一般 Embedded Linux」不能直接保證 hard real-time。**

但是：

> **「Linux 絕對不能做到 real-time」是錯的。**

而：

> **「一定要 FreeRTOS 才能 real-time」也是錯的。**

---

# 九、Linux Timer Interrupt 為什麼還是不夠？

假設 Raspberry Pi：

```text
Hardware Timer
      ↓
Interrupt
      ↓
Linux kernel
      ↓
scheduler
      ↓
your process
```

Timer interrupt 發生，不代表：

> 你的 application 立刻執行。

中間還有：

```text
interrupt handling
       ↓
kernel
       ↓
scheduler
       ↓
task/process
       ↓
your code
```

因此：

```text
Timer event
     ↓
     ├── 100 μs
     ├── 110 μs
     ├── 105 μs
     └── 3 ms   ← 某種系統事件造成延遲
```

一般 Linux 的問題不是：

> Timer 不準。

而是：

> **Timer event 到 application code 真正執行之間的 latency 不具備你要求的 hard deterministic guarantee。**

---

# 十、那 RTOS 就一定完美嗎？

也不能這麼說。

例如 FreeRTOS：

```text
Hardware Timer
       ↓
Interrupt
       ↓
High priority task
       ↓
application
```

它非常適合 deterministic embedded control。

但是：

> FreeRTOS 也不是「任何程式寫法都保證 1 ms」。

如果你：

```c
taskENTER_CRITICAL();
```

鎖太久；

或者：

```text
高優先權 interrupt
```

執行太久；

或者：

```text
blocking driver
```

設計不好；

仍然會產生 jitter。

所以正確說法是：

> **RTOS 提供比一般 Linux 更適合建立 deterministic real-time system 的執行模型，但是否達到 hard real-time，仍然取決於硬體、scheduler、interrupt、driver、程式架構與最壞情況分析。**

---

# 十一、那 Linux 有沒有辦法改善？

有。

例如 Linux 可以使用：

### PREEMPT_RT

讓 Linux kernel 更具有 real-time 特性。

再配合：

```text
SCHED_FIFO
CPU isolation
IRQ affinity
memory locking
real-time priority
dedicated CPU
```

可以把 jitter 大幅降低。

但是這和：

```text
一般 Raspberry Pi
+
一般 Ubuntu
+
一般 application
```

是完全不同的系統。

---

# 十二、所以 MCU、RTOS、Linux 可以這樣理解

| 系統                        | 1 ms deterministic control |
| ------------------------- | -------------------------- |
| STM32 bare-metal          | ⭐⭐⭐⭐⭐                      |
| STM32 + FreeRTOS          | ⭐⭐⭐⭐⭐                      |
| STM32 + Ethernet TCP/IP   | ⭐⭐⭐                        |
| Raspberry Pi + 一般 Linux   | ⭐⭐                         |
| Raspberry Pi + PREEMPT_RT | ⭐⭐⭐⭐                       |
| EtherCAT real-time system | ⭐⭐⭐⭐⭐                      |
| 一般 TCP/IP                 | ⭐⭐                         |

這個表不是在說「誰快」，而是在說：

> **誰比較容易提供 deterministic timing guarantee。**

---

# 十三、回到你現在的示波器，我建議不要急著修改 TCP client

因為你現在已經發現一個非常重要的問題：

> **我們目前的測量點其實不能充分回答「1 ms 是否 deterministic」。**

所以如果我們現在直接改程式讓波形看起來漂亮：

```text
HIGH ┌─┐    ┌─┐    ┌─┐    ┌─┐
     │ │    │ │    │ │    │ │
LOW ─┘ └────┘ └────┘ └────┘ └──
       1ms    1ms    1ms
```

這反而可能是「作弊」。

因為我們可能只是把 GPIO 做成漂亮的 timing signal，**而不是證明 TCP 有 deterministic timing。**

---

# 十四、我反而建議下一步做一個更嚴謹的實驗

我們保留現在的 TCP 架構，但是把 GPIO 定義清楚。

例如：

```text
PC6 = STM32 發送開始
PC7 = tcp_sent()
PC8 = Application ACK
```

示波器：

```text
PC6  ┌─┐      ┌─┐       ┌─┐
─────┘ └──────┘ └───────┘ └────

PC7      ┌─┐      ┌─┐       ┌─┐
─────────┘ └──────┘ └───────┘ └──

PC8         ┌─┐      ┌─┐       ┌─┐
────────────┘ └──────┘ └───────┘ └──
```

我們就可以知道：

```text
A = tcp_write → tcp_sent
B = tcp_write → Application ACK
C = packet-to-packet interval
```

然後讓示波器做：

> **最小值 / 最大值 / 平均值 / jitter**

---

# 十五、這才是真正能回答你的實驗

例如最後得到：

```text
Packet interval

Min       0.91 ms
Average   1.00 ms
Max       3.42 ms
Jitter    2.42 ms
```

那就非常有說服力。

因為我們可以說：

> **平均傳輸週期接近 1 ms，但存在 3.42 ms 的 worst-case latency，因此無法宣稱 hard real-time 1 ms deterministic guarantee。**

這比單純說：

> 「示波器看起來不漂亮」

科學很多。

---

# 最後給你一個非常重要的觀念

你現在的思考方向其實已經從：

> **「TCP 傳得夠不夠快？」**

進步到：

> **「TCP 能不能保證 deadline？」**

這才是 Real-Time 真正的核心。

所以你的直覺：

> **「我不在乎平均速度，我在乎 1 ms deadline 絕對不能 miss。」**

是正確的。

而在這個定義下：

### 普通 TCP/IP

**不能提供你要求的 hard-real-time guarantee。**

### STM32 Timer / DMA / bare-metal / RTOS

**可以建立 deterministic real-time system。**

### 一般 Embedded Linux

**不能直接提供 hard-real-time guarantee。**

### PREEMPT_RT Linux

**可以把 Linux 改造成 real-time-capable system，但仍需做 worst-case latency 驗證。**

### EtherCAT 等 deterministic industrial Ethernet

**就是為了解決「需要 Ethernet，又需要 deterministic real-time」這個問題而存在。**

而你現在示波器看到的兩種異常波形，**不要急著「修掉」**。它們很可能正是我們要拿來量測 jitter、latency 和 scheduling behaviour 的證據。下一步應該先把 **GPIO 的三個時間點重新定義清楚，再量 packet interval 的 worst-case**；這樣才能真正回答「TCP/IP 能不能滿足 1 ms hard real-time」這個問題。

---
python3 tcp_server.py 

> 最後一版
```python
import socket
import os
import threading
import sys
import time
import queue
from datetime import datetime


# ==========================================================
# TCP Server Configuration
# ==========================================================

HOST = '0.0.0.0'
PORT = 8888

SAVE_DIR = r'D:\RaspberryPi\Python_TCP_Server\LogData'

# 每累積 64 MB 就交給 Disk Writer
WRITE_BLOCK_SIZE = 64 * 1024 * 1024

# Disk Writer Queue 最大數量
MAX_QUEUE_SIZE = 100

# STM32 每一筆真正資料
EXPECTED_PACKET_SIZE = 204

# Terminal Monitor 更新週期
MONITOR_INTERVAL = 1.0


# ==========================================================
# Global Control
# ==========================================================

is_running = True


# ==========================================================
# Disk Writer Queue
# ==========================================================

buffer_queue = queue.Queue(maxsize=MAX_QUEUE_SIZE)


# ==========================================================
# Monitor Statistics
#
# 注意：
# 這些變數只用來顯示，不參與 TCP 傳輸流程。
# ==========================================================

stats_lock = threading.Lock()

client_connected = False

client_ip = ""
client_port = 0

client_connect_time = 0.0

total_packets = 0
total_received_bytes = 0

last_packet_number = 0
last_packet_data = b""

invalid_packet_count = 0

ack_sent_count = 0
ack_error_count = 0

last_ack_number = 0

last_recv_size = 0

last_error_message = ""

monitor_start_time = time.time()

# 本次 1 秒統計區間
interval_packets = 0
interval_bytes = 0

# TCP recv() 次數
recv_call_count = 0


# ==========================================================
# Create Save Directory
# ==========================================================

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)


# ==========================================================
# Utility
# ==========================================================

def format_bytes(value):

    if value < 1024:
        return f"{value} B"

    if value < 1024 * 1024:
        return f"{value / 1024:.2f} KB"

    if value < 1024 * 1024 * 1024:
        return f"{value / 1024 / 1024:.2f} MB"

    return f"{value / 1024 / 1024 / 1024:.2f} GB"


def format_duration(seconds):

    if seconds < 0:
        seconds = 0

    seconds = int(seconds)

    hours = seconds // 3600
    minutes = (seconds % 3600) // 60
    secs = seconds % 60

    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def format_hex(data):

    if not data:
        return "-"

    return " ".join(f"{byte:02x}" for byte in data)


# ==========================================================
# Terminal Monitor
#
# 不再每個 packet print。
#
# 每秒重新繪製固定畫面。
# ==========================================================

def monitor_thread():

    global is_running

    previous_packets = 0
    previous_bytes = 0
    previous_time = time.time()

    while is_running:

        time.sleep(MONITOR_INTERVAL)

        now = time.time()

        with stats_lock:

            connected = client_connected

            ip = client_ip
            port = client_port
            connect_time = client_connect_time

            packets = total_packets
            received_bytes = total_received_bytes

            last_packet = last_packet_number
            last_data = last_packet_data

            invalid_packets = invalid_packet_count

            ack_sent = ack_sent_count
            ack_errors = ack_error_count
            last_ack = last_ack_number

            recv_size = last_recv_size

            error_message = last_error_message

            recv_calls = recv_call_count

        elapsed = now - previous_time

        if elapsed > 0:

            instant_packets = (
                packets - previous_packets
            ) / elapsed

            instant_bytes = (
                received_bytes - previous_bytes
            ) / elapsed

        else:

            instant_packets = 0
            instant_bytes = 0

        previous_packets = packets
        previous_bytes = received_bytes
        previous_time = now

        total_elapsed = now - monitor_start_time

        if total_elapsed > 0:

            average_packets = packets / total_elapsed
            average_bytes = received_bytes / total_elapsed

        else:

            average_packets = 0
            average_bytes = 0


        # ==================================================
        # Connection Information
        # ==================================================

        if connected:

            status = "CONNECTED"

            connection_duration = format_duration(
                now - connect_time
            )

        else:

            status = "WAITING"

            connection_duration = "-"


        # ==================================================
        # Clear / redraw terminal
        #
        # ANSI escape sequence:
        #   ESC[2J = clear screen
        #   ESC[H  = cursor home
        #
        # Windows Terminal / modern Windows CMD 支援。
        # ==================================================

        print("\033[2J\033[H", end="")


        print("=" * 64)
        print("              STM32 TCP CLIENT MONITOR")
        print("=" * 64)

        print()

        print("Connection")
        print("-" * 64)

        print(f"Status          : {status}")
        print(f"Client IP       : {ip if connected else '-'}")
        print(f"Client Port     : {port if connected else '-'}")
        print(f"Connected Time  : {connection_duration}")

        print()

        print("TCP RX")
        print("-" * 64)

        print(f"Packets         : {packets:,}")
        print(f"Bytes           : {format_bytes(received_bytes)}")

        print(
            f"RX Rate         : "
            f"{instant_packets:,.1f} packets/s"
        )

        print(
            f"Data Rate       : "
            f"{format_bytes(instant_bytes)}/s"
        )

        print(
            f"Average Rate    : "
            f"{average_packets:,.1f} packets/s"
        )

        print()

        print("Packet")
        print("-" * 64)

        print(
            f"Expected Size   : "
            f"{EXPECTED_PACKET_SIZE} bytes"
        )

        print(
            f"Last recv()     : "
            f"{recv_size} bytes"
        )

        print(
            f"Last Packet     : "
            f"#{last_packet:08d}"
        )

        print(
            f"First 16 Bytes  : "
            f"{format_hex(last_data[:16])}"
        )

        print(
            f"Invalid Packet  : "
            f"{invalid_packets}"
        )

        print()

        print("Application ACK")
        print("-" * 64)

        print("ACK Enabled     : YES")

        print(
            f"ACK Sent        : "
            f"{ack_sent:,}"
        )

        print(
            f"Last ACK        : "
            f"#{last_ack:08d}"
        )

        print(
            f"ACK Error       : "
            f"{ack_errors}"
        )

        print()

        print("TCP Debug")
        print("-" * 64)

        print(
            f"recv() Calls    : "
            f"{recv_calls:,}"
        )

        print()

        print("Disk Writer")
        print("-" * 64)

        print(
            f"Queue           : "
            f"{buffer_queue.qsize()} / "
            f"{MAX_QUEUE_SIZE}"
        )

        print(
            f"Write Block     : "
            f"{WRITE_BLOCK_SIZE / 1024 / 1024:.1f} MB"
        )

        print()

        print("Status")
        print("-" * 64)

        if error_message:

            print(
                f"Last Error      : "
                f"{error_message}"
            )

        else:

            print("Last Error      : NONE")

        print()

        print("-" * 64)
        print("Press [Q] to stop server")
        print("-" * 64)

    print()


# ==========================================================
# Disk Writer Thread
# ==========================================================

def disk_writer_thread():

    global is_running

    print("[DISK] Disk Writer Thread 啟動")

    while is_running or not buffer_queue.empty():

        try:

            data_to_write = buffer_queue.get(
                timeout=1
            )

        except queue.Empty:

            continue

        try:

            filename = os.path.join(
                SAVE_DIR,
                datetime.now().strftime(
                    '%Y%m%d-%H%M%S-%f.bin'
                )
            )

            with open(filename, 'wb') as f:

                f.write(data_to_write)

            # Disk Writer 不再每次寫檔就 print。
            #
            # 避免 Terminal 被 Disk Writer 訊息洗掉。
            #
            # 狀態由 Monitor 顯示。

        except Exception as e:

            with stats_lock:

                last_error_message = (
                    f"Disk Writer Error: {e}"
                )

        finally:

            buffer_queue.task_done()

    print("[DISK] Disk Writer Thread 結束")


# ==========================================================
# Keyboard Listener
# ==========================================================

def listen_for_quit():

    global is_running

    while is_running:

        try:

            cmd = sys.stdin.readline()

        except Exception:

            break

        if not cmd:

            continue

        cmd = cmd.strip().lower()

        if cmd == 'q':

            print()
            print("[SERVER] 收到 Q，準備結束...")

            is_running = False

            break


# ==========================================================
# Reset Client Statistics
# ==========================================================

def reset_client_statistics():

    global client_connected
    global client_ip
    global client_port
    global client_connect_time

    global total_packets
    global total_received_bytes

    global last_packet_number
    global last_packet_data

    global invalid_packet_count

    global ack_sent_count
    global ack_error_count
    global last_ack_number

    global last_recv_size

    global last_error_message

    global monitor_start_time

    global recv_call_count

    with stats_lock:

        client_connected = False

        client_ip = ""
        client_port = 0

        client_connect_time = 0

        total_packets = 0
        total_received_bytes = 0

        last_packet_number = 0
        last_packet_data = b""

        invalid_packet_count = 0

        ack_sent_count = 0
        ack_error_count = 0

        last_ack_number = 0

        last_recv_size = 0

        last_error_message = ""

        recv_call_count = 0

        monitor_start_time = time.time()


# ==========================================================
# TCP Server
# ==========================================================

def start_server():

    global is_running

    global client_connected
    global client_ip
    global client_port
    global client_connect_time

    global total_packets
    global total_received_bytes

    global last_packet_number
    global last_packet_data

    global invalid_packet_count

    global ack_sent_count
    global ack_error_count
    global last_ack_number

    global last_recv_size
    global last_error_message

    global interval_packets
    global interval_bytes

    global recv_call_count


    # ======================================================
    # Start Disk Writer
    # ======================================================

    threading.Thread(
        target=disk_writer_thread,
        daemon=True
    ).start()


    # ======================================================
    # Start Monitor
    # ======================================================

    threading.Thread(
        target=monitor_thread,
        daemon=True
    ).start()


    # ======================================================
    # Create Server Socket
    # ======================================================

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

    server_socket.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_RCVBUF,
        4 * 1024 * 1024
    )


    server_socket.bind(
        (HOST, PORT)
    )

    server_socket.listen(5)

    server_socket.settimeout(1)


    # ======================================================
    # Startup Information
    # ======================================================

    print()
    print("=" * 64)
    print("              Python TCP Server")
    print("=" * 64)
    print(f"Listen IP       : {HOST}")
    print(f"Listen Port     : {PORT}")
    print(
        f"Expected Packet : "
        f"{EXPECTED_PACKET_SIZE} bytes"
    )
    print(
        f"Write Block     : "
        f"{WRITE_BLOCK_SIZE / 1024 / 1024:.1f} MB"
    )
    print(
        f"Queue Size      : "
        f"{MAX_QUEUE_SIZE}"
    )
    print("=" * 64)
    print()
    print("Waiting for STM32 Client...")
    print()


    # ======================================================
    # Main Server Loop
    # ======================================================

    while is_running:

        try:

            conn, addr = server_socket.accept()

        except socket.timeout:

            continue

        except Exception as e:

            if is_running:

                with stats_lock:

                    last_error_message = (
                        f"accept error: {e}"
                    )

            continue


        # ==================================================
        # Client Connected
        # ==================================================

        conn.settimeout(1)

        client_ip = addr[0]
        client_port = addr[1]
        client_connect_time = time.time()

        client_connected = True

        # ==================================================
        # 每一個 Client 都重新開始 packet counter
        # ==================================================

        total_packets = 0
        total_received_bytes = 0

        last_packet_number = 0
        last_packet_data = b""

        invalid_packet_count = 0

        ack_sent_count = 0
        ack_error_count = 0
        last_ack_number = 0

        last_recv_size = 0

        recv_call_count = 0

        last_error_message = ""

        monitor_start_time = time.time()


        # ==================================================
        # TCP Stream Parser Buffer
        #
        # TCP 沒有 packet boundary。
        #
        # 所以 recv() 收到多少 bytes 都不能直接當成
        # 一個 204-byte packet。
        # ==================================================

        packet_buffer = bytearray()


        # ==================================================
        # Disk Write Buffer
        #
        # 與 packet parser 分離。
        #
        # parser 負責確認 204-byte data。
        #
        # write_buffer 負責保存原始 TCP stream。
        # ==================================================

        write_buffer = bytearray()


        # ==================================================
        # Application ACK Counter
        # ==================================================

        ack_counter = 0


        try:

            while is_running:

                try:

                    data = conn.recv(65536)

                    recv_call_count += 1

                    if not data:

                        break


                    # ======================================
                    # TCP RX Statistics
                    # ======================================

                    received_length = len(data)

                    last_recv_size = received_length

                    total_received_bytes += received_length


                    # ======================================
                    # Packet Parser Buffer
                    # ======================================

                    packet_buffer.extend(data)


                    # ======================================
                    # Disk Writer Buffer
                    #
                    # 保存收到的原始 TCP data。
                    # ======================================

                    write_buffer.extend(data)


                    # ======================================
                    # 解析完整 204-byte packet
                    # ======================================

                    while (
                        len(packet_buffer)
                        >= EXPECTED_PACKET_SIZE
                    ):

                        packet = bytes(
                            packet_buffer[
                                :EXPECTED_PACKET_SIZE
                            ]
                        )

                        del packet_buffer[
                            :EXPECTED_PACKET_SIZE
                        ]


                        # ==================================
                        # Packet Counter
                        # ==================================

                        total_packets += 1

                        last_packet_number = (
                            total_packets
                        )

                        last_packet_data = packet


                        # ==================================
                        # Application ACK
                        #
                        # ACK Format:
                        #
                        #   Byte 0 = 0xAC
                        #   Byte 1 = 0x4B
                        #   Byte 2 = counter LSB
                        #   Byte 3 = counter MSB
                        #
                        # Server 收到完整 204 bytes
                        # 後立即送 ACK。
                        #
                        # 不等待 Disk Writer。
                        # ==================================

                        ack_counter += 1

                        ack = bytes([
                            0xAC,
                            0x4B,
                            ack_counter & 0xFF,
                            (ack_counter >> 8) & 0xFF
                        ])


                        try:

                            conn.sendall(ack)

                            ack_sent_count += 1

                            last_ack_number = (
                                ack_counter
                            )

                        except Exception as e:

                            ack_error_count += 1

                            last_error_message = (
                                f"ACK send error: {e}"
                            )

                            break


                    # ======================================
                    # Disk Buffer Flush
                    # ======================================

                    if (
                        len(write_buffer)
                        >= WRITE_BLOCK_SIZE
                    ):

                        try:

                            buffer_queue.put(
                                bytes(write_buffer),
                                timeout=5
                            )

                            write_buffer = bytearray()

                        except queue.Full:

                            last_error_message = (
                                "Disk Writer Queue FULL"
                            )


                except socket.timeout:

                    continue


                except Exception as e:

                    last_error_message = (
                        f"recv error: {e}"
                    )

                    break


        finally:

            # ==================================================
            # Client Disconnect
            # ==================================================

            client_connected = False


            # ==================================================
            # 保存剩餘資料
            # ==================================================

            if len(write_buffer) > 0:

                try:

                    buffer_queue.put(
                        bytes(write_buffer),
                        timeout=5
                    )

                    write_buffer = bytearray()

                except queue.Full:

                    last_error_message = (
                        "Final Flush failed: Queue FULL"
                    )


            # ==================================================
            # 如果 packet_buffer 還有資料
            #
            # 表示 TCP stream 最後留下不足 204 bytes 的資料。
            #
            # 這是重要的 debug 資訊。
            # ==================================================

            if len(packet_buffer) > 0:

                with stats_lock:

                    invalid_packet_count += 1

                    last_error_message = (
                        f"Connection closed with "
                        f"{len(packet_buffer)} "
                        f"unprocessed bytes"
                    )


            try:

                conn.close()

            except Exception:

                pass


    # ======================================================
    # Server Shutdown
    # ======================================================

    print()
    print("[SERVER] Waiting for Disk Writer...")
    print()

    buffer_queue.join()

    server_socket.close()

    print()
    print("[SERVER] Server 結束")


# ==========================================================
# Main
# ==========================================================

if __name__ == "__main__":

    threading.Thread(
        target=listen_for_quit,
        daemon=True
    ).start()

    start_server()

    # 不使用 os._exit(0)
    #
    # 讓 Python 有機會正常結束所有 thread。

    sys.exit(0)
```

> 最後二版
```python
import socket
import os
import threading
import sys
import time
import queue
from datetime import datetime


# ==========================================================
# TCP Server Configuration
# ==========================================================

HOST = '0.0.0.0'
PORT = 8888

SAVE_DIR = r'D:\RaspberryPi\Python_TCP_Server\LogData'

# 每累積 64 MB 就交給 Disk Writer
WRITE_BLOCK_SIZE = 64 * 1024 * 1024

# Disk Writer Queue 最大數量
MAX_QUEUE_SIZE = 100

# STM32 每一筆真正資料
EXPECTED_PACKET_SIZE = 204

# Terminal Monitor 更新週期
MONITOR_INTERVAL = 1.0


# ==========================================================
# Global Control
# ==========================================================

is_running = True


# ==========================================================
# Disk Writer Queue
# ==========================================================

buffer_queue = queue.Queue(maxsize=MAX_QUEUE_SIZE)


# ==========================================================
# Monitor Statistics
#
# 注意：
# 這些變數只用來顯示，不參與 TCP 傳輸流程。
# ==========================================================

stats_lock = threading.Lock()

client_connected = False

client_ip = ""
client_port = 0

client_connect_time = 0.0

total_packets = 0
total_received_bytes = 0

last_packet_number = 0
last_packet_data = b""

invalid_packet_count = 0

ack_sent_count = 0
ack_error_count = 0

last_ack_number = 0

last_recv_size = 0

last_error_message = ""

monitor_start_time = time.time()

# 本次 1 秒統計區間
interval_packets = 0
interval_bytes = 0

# TCP recv() 次數
recv_call_count = 0


# ==========================================================
# Create Save Directory
# ==========================================================

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)


# ==========================================================
# Utility
# ==========================================================

def format_bytes(value):

    if value < 1024:
        return f"{value} B"

    if value < 1024 * 1024:
        return f"{value / 1024:.2f} KB"

    if value < 1024 * 1024 * 1024:
        return f"{value / 1024 / 1024:.2f} MB"

    return f"{value / 1024 / 1024 / 1024:.2f} GB"


def format_duration(seconds):

    if seconds < 0:
        seconds = 0

    seconds = int(seconds)

    hours = seconds // 3600
    minutes = (seconds % 3600) // 60
    secs = seconds % 60

    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def format_hex(data):

    if not data:
        return "-"

    return " ".join(f"{byte:02x}" for byte in data)


# ==========================================================
# Terminal Monitor
#
# 不再每個 packet print。
#
# 每秒重新繪製固定畫面。
# ==========================================================

def monitor_thread():

    global is_running

    previous_packets = 0
    previous_bytes = 0
    previous_time = time.time()

    while is_running:

        time.sleep(MONITOR_INTERVAL)

        now = time.time()

        with stats_lock:

            connected = client_connected

            ip = client_ip
            port = client_port
            connect_time = client_connect_time

            packets = total_packets
            received_bytes = total_received_bytes

            last_packet = last_packet_number
            last_data = last_packet_data

            invalid_packets = invalid_packet_count

            ack_sent = ack_sent_count
            ack_errors = ack_error_count
            last_ack = last_ack_number

            recv_size = last_recv_size

            error_message = last_error_message

            recv_calls = recv_call_count

        elapsed = now - previous_time

        if elapsed > 0:

            instant_packets = (
                packets - previous_packets
            ) / elapsed

            instant_bytes = (
                received_bytes - previous_bytes
            ) / elapsed

        else:

            instant_packets = 0
            instant_bytes = 0

        previous_packets = packets
        previous_bytes = received_bytes
        previous_time = now

        total_elapsed = now - monitor_start_time

        if total_elapsed > 0:

            average_packets = packets / total_elapsed
            average_bytes = received_bytes / total_elapsed

        else:

            average_packets = 0
            average_bytes = 0


        # ==================================================
        # Connection Information
        # ==================================================

        if connected:

            status = "CONNECTED"

            connection_duration = format_duration(
                now - connect_time
            )

        else:

            status = "WAITING"

            connection_duration = "-"


        # ==================================================
        # Clear / redraw terminal
        #
        # ANSI escape sequence:
        #   ESC[2J = clear screen
        #   ESC[H  = cursor home
        #
        # Windows Terminal / modern Windows CMD 支援。
        # ==================================================

        print("\033[2J\033[H", end="")


        print("=" * 64)
        print("              STM32 TCP CLIENT MONITOR")
        print("=" * 64)

        print()

        print("Connection")
        print("-" * 64)

        print(f"Status          : {status}")
        print(f"Client IP       : {ip if connected else '-'}")
        print(f"Client Port     : {port if connected else '-'}")
        print(f"Connected Time  : {connection_duration}")

        print()

        print("TCP RX")
        print("-" * 64)

        print(f"Packets         : {packets:,}")
        print(f"Bytes           : {format_bytes(received_bytes)}")

        print(
            f"RX Rate         : "
            f"{instant_packets:,.1f} packets/s"
        )

        print(
            f"Data Rate       : "
            f"{format_bytes(instant_bytes)}/s"
        )

        print(
            f"Average Rate    : "
            f"{average_packets:,.1f} packets/s"
        )

        print()

        print("Packet")
        print("-" * 64)

        print(
            f"Expected Size   : "
            f"{EXPECTED_PACKET_SIZE} bytes"
        )

        print(
            f"Last recv()     : "
            f"{recv_size} bytes"
        )

        print(
            f"Last Packet     : "
            f"#{last_packet:08d}"
        )

        print(
            f"First 16 Bytes  : "
            f"{format_hex(last_data[:16])}"
        )

        print(
            f"Invalid Packet  : "
            f"{invalid_packets}"
        )

        print()

        print("Application ACK")
        print("-" * 64)

        print("ACK Enabled     : YES")

        print(
            f"ACK Sent        : "
            f"{ack_sent:,}"
        )

        print(
            f"Last ACK        : "
            f"#{last_ack:08d}"
        )

        print(
            f"ACK Error       : "
            f"{ack_errors}"
        )

        print()

        print("TCP Debug")
        print("-" * 64)

        print(
            f"recv() Calls    : "
            f"{recv_calls:,}"
        )

        print()

        print("Disk Writer")
        print("-" * 64)

        print(
            f"Queue           : "
            f"{buffer_queue.qsize()} / "
            f"{MAX_QUEUE_SIZE}"
        )

        print(
            f"Write Block     : "
            f"{WRITE_BLOCK_SIZE / 1024 / 1024:.1f} MB"
        )

        print()

        print("Status")
        print("-" * 64)

        if error_message:

            print(
                f"Last Error      : "
                f"{error_message}"
            )

        else:

            print("Last Error      : NONE")

        print()

        print("-" * 64)
        print("Press [Q] to stop server")
        print("-" * 64)

    print()


# ==========================================================
# Disk Writer Thread
# ==========================================================

def disk_writer_thread():

    global is_running

    print("[DISK] Disk Writer Thread 啟動")

    while is_running or not buffer_queue.empty():

        try:

            data_to_write = buffer_queue.get(
                timeout=1
            )

        except queue.Empty:

            continue

        try:

            filename = os.path.join(
                SAVE_DIR,
                datetime.now().strftime(
                    '%Y%m%d-%H%M%S-%f.bin'
                )
            )

            with open(filename, 'wb') as f:

                f.write(data_to_write)

            # Disk Writer 不再每次寫檔就 print。
            #
            # 避免 Terminal 被 Disk Writer 訊息洗掉。
            #
            # 狀態由 Monitor 顯示。

        except Exception as e:

            with stats_lock:

                last_error_message = (
                    f"Disk Writer Error: {e}"
                )

        finally:

            buffer_queue.task_done()

    print("[DISK] Disk Writer Thread 結束")


# ==========================================================
# Keyboard Listener
# ==========================================================

def listen_for_quit():

    global is_running

    while is_running:

        try:

            cmd = sys.stdin.readline()

        except Exception:

            break

        if not cmd:

            continue

        cmd = cmd.strip().lower()

        if cmd == 'q':

            print()
            print("[SERVER] 收到 Q，準備結束...")

            is_running = False

            break


# ==========================================================
# Reset Client Statistics
# ==========================================================

def reset_client_statistics():

    global client_connected
    global client_ip
    global client_port
    global client_connect_time

    global total_packets
    global total_received_bytes

    global last_packet_number
    global last_packet_data

    global invalid_packet_count

    global ack_sent_count
    global ack_error_count
    global last_ack_number

    global last_recv_size

    global last_error_message

    global monitor_start_time

    global recv_call_count

    with stats_lock:

        client_connected = False

        client_ip = ""
        client_port = 0

        client_connect_time = 0

        total_packets = 0
        total_received_bytes = 0

        last_packet_number = 0
        last_packet_data = b""

        invalid_packet_count = 0

        ack_sent_count = 0
        ack_error_count = 0

        last_ack_number = 0

        last_recv_size = 0

        last_error_message = ""

        recv_call_count = 0

        monitor_start_time = time.time()


# ==========================================================
# TCP Server
# ==========================================================

def start_server():

    global is_running

    global client_connected
    global client_ip
    global client_port
    global client_connect_time

    global total_packets
    global total_received_bytes

    global last_packet_number
    global last_packet_data

    global invalid_packet_count

    global ack_sent_count
    global ack_error_count
    global last_ack_number

    global last_recv_size
    global last_error_message

    global interval_packets
    global interval_bytes

    global recv_call_count


    # ======================================================
    # Start Disk Writer
    # ======================================================

    threading.Thread(
        target=disk_writer_thread,
        daemon=True
    ).start()


    # ======================================================
    # Start Monitor
    # ======================================================

    threading.Thread(
        target=monitor_thread,
        daemon=True
    ).start()


    # ======================================================
    # Create Server Socket
    # ======================================================

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

    server_socket.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_RCVBUF,
        4 * 1024 * 1024
    )


    server_socket.bind(
        (HOST, PORT)
    )

    server_socket.listen(5)

    server_socket.settimeout(1)


    # ======================================================
    # Startup Information
    # ======================================================

    print()
    print("=" * 64)
    print("              Python TCP Server")
    print("=" * 64)
    print(f"Listen IP       : {HOST}")
    print(f"Listen Port     : {PORT}")
    print(
        f"Expected Packet : "
        f"{EXPECTED_PACKET_SIZE} bytes"
    )
    print(
        f"Write Block     : "
        f"{WRITE_BLOCK_SIZE / 1024 / 1024:.1f} MB"
    )
    print(
        f"Queue Size      : "
        f"{MAX_QUEUE_SIZE}"
    )
    print("=" * 64)
    print()
    print("Waiting for STM32 Client...")
    print()


    # ======================================================
    # Main Server Loop
    # ======================================================

    while is_running:

        try:

            conn, addr = server_socket.accept()

        except socket.timeout:

            continue

        except Exception as e:

            if is_running:

                with stats_lock:

                    last_error_message = (
                        f"accept error: {e}"
                    )

            continue


        # ==================================================
        # Client Connected
        # ==================================================

        conn.settimeout(1)

        client_ip = addr[0]
        client_port = addr[1]
        client_connect_time = time.time()

        client_connected = True

        # ==================================================
        # 每一個 Client 都重新開始 packet counter
        # ==================================================

        total_packets = 0
        total_received_bytes = 0

        last_packet_number = 0
        last_packet_data = b""

        invalid_packet_count = 0

        ack_sent_count = 0
        ack_error_count = 0
        last_ack_number = 0

        last_recv_size = 0

        recv_call_count = 0

        last_error_message = ""

        monitor_start_time = time.time()


        # ==================================================
        # TCP Stream Parser Buffer
        #
        # TCP 沒有 packet boundary。
        #
        # 所以 recv() 收到多少 bytes 都不能直接當成
        # 一個 204-byte packet。
        # ==================================================

        packet_buffer = bytearray()


        # ==================================================
        # Disk Write Buffer
        #
        # 與 packet parser 分離。
        #
        # parser 負責確認 204-byte data。
        #
        # write_buffer 負責保存原始 TCP stream。
        # ==================================================

        write_buffer = bytearray()


        # ==================================================
        # Application ACK Counter
        # ==================================================

        ack_counter = 0


        try:

            while is_running:

                try:

                    data = conn.recv(65536)

                    recv_call_count += 1

                    if not data:

                        break


                    # ======================================
                    # TCP RX Statistics
                    # ======================================

                    received_length = len(data)

                    last_recv_size = received_length

                    total_received_bytes += received_length


                    # ======================================
                    # Packet Parser Buffer
                    # ======================================

                    packet_buffer.extend(data)


                    # ======================================
                    # Disk Writer Buffer
                    #
                    # 保存收到的原始 TCP data。
                    # ======================================

                    write_buffer.extend(data)


                    # ======================================
                    # 解析完整 204-byte packet
                    # ======================================

                    while (
                        len(packet_buffer)
                        >= EXPECTED_PACKET_SIZE
                    ):

                        packet = bytes(
                            packet_buffer[
                                :EXPECTED_PACKET_SIZE
                            ]
                        )

                        del packet_buffer[
                            :EXPECTED_PACKET_SIZE
                        ]


                        # ==================================
                        # Packet Counter
                        # ==================================

                        total_packets += 1

                        last_packet_number = (
                            total_packets
                        )

                        last_packet_data = packet


                        # ==================================
                        # Application ACK
                        #
                        # ACK Format:
                        #
                        #   Byte 0 = 0xAC
                        #   Byte 1 = 0x4B
                        #   Byte 2 = counter LSB
                        #   Byte 3 = counter MSB
                        #
                        # Server 收到完整 204 bytes
                        # 後立即送 ACK。
                        #
                        # 不等待 Disk Writer。
                        # ==================================

                        ack_counter += 1

                        ack = bytes([
                            0xAC,
                            0x4B,
                            ack_counter & 0xFF,
                            (ack_counter >> 8) & 0xFF
                        ])


                        try:

                            conn.sendall(ack)

                            ack_sent_count += 1

                            last_ack_number = (
                                ack_counter
                            )

                        except Exception as e:

                            ack_error_count += 1

                            last_error_message = (
                                f"ACK send error: {e}"
                            )

                            break


                    # ======================================
                    # Disk Buffer Flush
                    # ======================================

                    if (
                        len(write_buffer)
                        >= WRITE_BLOCK_SIZE
                    ):

                        try:

                            buffer_queue.put(
                                bytes(write_buffer),
                                timeout=5
                            )

                            write_buffer = bytearray()

                        except queue.Full:

                            last_error_message = (
                                "Disk Writer Queue FULL"
                            )


                except socket.timeout:

                    continue


                except Exception as e:

                    last_error_message = (
                        f"recv error: {e}"
                    )

                    break


        finally:

            # ==================================================
            # Client Disconnect
            # ==================================================

            client_connected = False


            # ==================================================
            # 保存剩餘資料
            # ==================================================

            if len(write_buffer) > 0:

                try:

                    buffer_queue.put(
                        bytes(write_buffer),
                        timeout=5
                    )

                    write_buffer = bytearray()

                except queue.Full:

                    last_error_message = (
                        "Final Flush failed: Queue FULL"
                    )


            # ==================================================
            # 如果 packet_buffer 還有資料
            #
            # 表示 TCP stream 最後留下不足 204 bytes 的資料。
            #
            # 這是重要的 debug 資訊。
            # ==================================================

            if len(packet_buffer) > 0:

                with stats_lock:

                    invalid_packet_count += 1

                    last_error_message = (
                        f"Connection closed with "
                        f"{len(packet_buffer)} "
                        f"unprocessed bytes"
                    )


            try:

                conn.close()

            except Exception:

                pass


    # ======================================================
    # Server Shutdown
    # ======================================================

    print()
    print("[SERVER] Waiting for Disk Writer...")
    print()

    buffer_queue.join()

    server_socket.close()

    print()
    print("[SERVER] Server 結束")


# ==========================================================
# Main
# ==========================================================

if __name__ == "__main__":

    threading.Thread(
        target=listen_for_quit,
        daemon=True
    ).start()

    start_server()

    # 不使用 os._exit(0)
    #
    # 讓 Python 有機會正常結束所有 thread。

    sys.exit(0)
```

> 最後三版
```python
import socket
import os
import threading
import sys
import time
import queue
from datetime import datetime

# ==========================================================
# Server 設定
# ==========================================================

HOST = '0.0.0.0'
PORT = 8888

SAVE_DIR = r'D:\RaspberryPi\Python_TCP_Server\LogData'

# 每累積 64MB 就交給寫檔執行緒
WRITE_BLOCK_SIZE = 64 * 1024 * 1024

# Queue 最大允許數量
MAX_QUEUE_SIZE = 100

# STM32 每一筆真正資料長度
EXPECTED_PACKET_SIZE = 204

is_running = True


# ==========================================================
# Queue
# ==========================================================

buffer_queue = queue.Queue(maxsize=MAX_QUEUE_SIZE)


# ==========================================================
# 建立資料夾
# ==========================================================

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)


# ==========================================================
# 磁碟寫入執行緒
# ==========================================================

def disk_writer_thread():

    global is_running

    print("[DISK] Disk Writer Thread 啟動")

    while is_running or not buffer_queue.empty():

        try:

            data_to_write = buffer_queue.get(timeout=1)

        except queue.Empty:

            continue

        try:

            filename = os.path.join(
                SAVE_DIR,
                datetime.now().strftime('%Y%m%d-%H%M%S-%f.bin')
            )

            with open(filename, 'wb') as f:

                f.write(data_to_write)

            print(
                f"[DISK] Write Complete | "
                f"{os.path.basename(filename)} | "
                f"{len(data_to_write) / 1024 / 1024:.2f} MB | "
                f"Queue={buffer_queue.qsize()}"
            )

        except Exception as e:

            print(f"[DISK] Write Error: {e}")

        finally:

            buffer_queue.task_done()

    print("[DISK] Disk Writer Thread 結束")


# ==========================================================
# 鍵盤輸入 q 離開
# ==========================================================

def listen_for_quit():

    global is_running

    while True:

        cmd = sys.stdin.readline().strip().lower()

        if cmd == 'q':

            print("[SYSTEM] 收到結束指令")

            is_running = False

            break


# ==========================================================
# Client 統計資訊
# ==========================================================

class ClientStatistics:

    def __init__(self):

        self.total_received = 0

        self.total_packets = 0

        self.recv_calls = 0

        self.partial_bytes = 0

        self.invalid_packet_count = 0

        self.start_time = time.perf_counter()

        self.last_report_time = self.start_time

        self.last_report_bytes = 0

        self.last_report_packets = 0


    def add_recv(self, size):

        self.total_received += size

        self.recv_calls += 1


    def add_packet(self):

        self.total_packets += 1


    def print_periodic_report(self, client_addr):

        now = time.perf_counter()

        elapsed = now - self.last_report_time

        if elapsed < 1.0:

            return

        delta_bytes = (
            self.total_received -
            self.last_report_bytes
        )

        delta_packets = (
            self.total_packets -
            self.last_report_packets
        )

        rx_bytes_per_sec = delta_bytes / elapsed

        rx_mb_per_sec = (
            rx_bytes_per_sec /
            1024 /
            1024
        )

        packet_rate = (
            delta_packets / elapsed
        )

        total_elapsed = (
            now - self.start_time
        )

        total_mb = (
            self.total_received /
            1024 /
            1024
        )

        print(
            f"[RX] {client_addr} | "
            f"Rate={rx_bytes_per_sec:.0f} B/s "
            f"({rx_mb_per_sec:.3f} MB/s) | "
            f"Packets={packet_rate:.1f}/s | "
            f"Total={total_mb:.3f} MB | "
            f"Packets={self.total_packets} | "
            f"recv()={self.recv_calls} | "
            f"Queue={buffer_queue.qsize()}"
        )

        self.last_report_time = now

        self.last_report_bytes = (
            self.total_received
        )

        self.last_report_packets = (
            self.total_packets
        )


    def print_final(self, client_addr):

        elapsed = (
            time.perf_counter() -
            self.start_time
        )

        if elapsed <= 0:

            elapsed = 0.000001

        avg_bytes_per_sec = (
            self.total_received /
            elapsed
        )

        avg_mb_per_sec = (
            avg_bytes_per_sec /
            1024 /
            1024
        )

        print("")
        print("--------------------------------------------------")
        print(f"[CLIENT] {client_addr}")
        print(
            f"[CLIENT] Connection Time : "
            f"{elapsed:.3f} sec"
        )
        print(
            f"[CLIENT] Total RX        : "
            f"{self.total_received} bytes "
            f"({self.total_received / 1024 / 1024:.3f} MB)"
        )
        print(
            f"[CLIENT] Total Packets   : "
            f"{self.total_packets}"
        )
        print(
            f"[CLIENT] recv() Calls    : "
            f"{self.recv_calls}"
        )
        print(
            f"[CLIENT] Average RX      : "
            f"{avg_bytes_per_sec:.0f} B/s "
            f"({avg_mb_per_sec:.3f} MB/s)"
        )
        print(
            f"[CLIENT] Invalid Packets : "
            f"{self.invalid_packet_count}"
        )
        print(
            f"[CLIENT] Partial Bytes   : "
            f"{self.partial_bytes}"
        )
        print("--------------------------------------------------")
        print("")


# ==========================================================
# 顯示收到的 204-byte Packet
# ==========================================================

def print_packet_info(packet_number, packet):

    print(
        f"[PACKET] #{packet_number:08d} | "
        f"Length={len(packet)} | "
        f"DATA="
        f"{packet[:16].hex(' ')} ..."
    )


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

    server_socket.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_RCVBUF,
        4 * 1024 * 1024
    )

    server_socket.bind(
        (HOST, PORT)
    )

    server_socket.listen(5)

    server_socket.settimeout(1)

    print("")
    print("==============================================")
    print("        Python TCP Server")
    print("==============================================")
    print(f"Listen IP       : {HOST}")
    print(f"Listen Port     : {PORT}")
    print(
        f"Expected Packet : "
        f"{EXPECTED_PACKET_SIZE} bytes"
    )
    print(
        f"Write Block     : "
        f"{WRITE_BLOCK_SIZE / 1024 / 1024:.1f} MB"
    )
    print(
        f"Queue Size      : "
        f"{MAX_QUEUE_SIZE}"
    )
    print("==============================================")
    print("Waiting for STM32 Client...")
    print("")

    while is_running:

        try:

            conn, addr = server_socket.accept()

            client_ip = addr[0]
            client_port = addr[1]

            print("")
            print("==============================================")
            print("[CLIENT] Connected")
            print(
                f"[CLIENT] IP        : "
                f"{client_ip}"
            )
            print(
                f"[CLIENT] Port      : "
                f"{client_port}"
            )
            print(
                f"[CLIENT] Time      : "
                f"{datetime.now()}"
            )
            print("==============================================")

            conn.settimeout(1)

            # ==================================================
            # Client Buffer
            #
            # TCP 是 byte stream。
            #
            # 一次 recv() 不一定等於一個 packet。
            #
            # 所以使用 bytearray 累積資料，
            # 再按照 204 bytes 切出完整 packet。
            # ==================================================

            current_buffer = bytearray()

            statistics = ClientStatistics()

            while is_running:

                try:

                    data = conn.recv(65536)

                    if not data:

                        print(
                            "[CLIENT] Client Disconnect"
                        )

                        break

                    # ==========================================
                    # recv() 層級資訊
                    # ==========================================

                    statistics.add_recv(
                        len(data)
                    )

                    print(
                        f"[TCP] recv() = "
                        f"{len(data)} bytes"
                    )

                    # ==========================================
                    # 加入 TCP stream buffer
                    # ==========================================

                    current_buffer.extend(data)

                    # ==========================================
                    # 解析完整 204-byte packet
                    # ==========================================

                    while (
                        len(current_buffer)
                        >= EXPECTED_PACKET_SIZE
                    ):

                        packet = bytes(
                            current_buffer[
                                :EXPECTED_PACKET_SIZE
                            ]
                        )

                        del current_buffer[
                            :EXPECTED_PACKET_SIZE
                        ]

                        statistics.add_packet()

                        print_packet_info(
                            statistics.total_packets,
                            packet
                        )

                    # ==========================================
                    # 目前尚未形成完整 packet 的 bytes
                    # ==========================================

                    statistics.partial_bytes = (
                        len(current_buffer)
                    )

                    # ==========================================
                    # 定期顯示統計
                    # ==========================================

                    statistics.print_periodic_report(
                        addr
                    )

                    # ==========================================
                    # 寫檔
                    #
                    # 注意：
                    # 目前仍然保留原本「收到資料就累積」
                    # 的寫檔設計。
                    # ==========================================

                    if len(current_buffer) >= WRITE_BLOCK_SIZE:

                        try:

                            buffer_queue.put(
                                bytes(current_buffer),
                                timeout=5
                            )

                            print(
                                f"[BUFFER] Flush | "
                                f"{len(current_buffer) / 1024 / 1024:.2f} MB | "
                                f"Queue={buffer_queue.qsize()}"
                            )

                            current_buffer = bytearray()

                        except queue.Full:

                            print(
                                "[WARNING] "
                                "Disk Writer 跟不上，"
                                "Queue 已滿"
                            )

                except socket.timeout:

                    continue

                except Exception as e:

                    print(
                        f"[TCP] recv error: {e}"
                    )

                    break

            # ==================================================
            # Client 結束
            # ==================================================

            statistics.partial_bytes = (
                len(current_buffer)
            )

            statistics.print_final(addr)

            # ==================================================
            # 最後剩餘資料
            # ==================================================

            if len(current_buffer) > 0:

                try:

                    buffer_queue.put(
                        bytes(current_buffer),
                        timeout=5
                    )

                    print(
                        f"[BUFFER] Final Flush | "
                        f"{len(current_buffer)} bytes"
                    )

                except queue.Full:

                    print(
                        "[ERROR] "
                        "Final Flush 失敗，Queue 已滿"
                    )

            conn.close()

            print(
                f"[CLIENT] Connection closed "
                f"{addr}"
            )

        except socket.timeout:

            continue

        except Exception as e:

            print(
                f"[SERVER] accept error: {e}"
            )

    # ==========================================================
    # Server shutdown
    # ==========================================================

    print("")
    print("[SYSTEM] 等待剩餘資料寫入磁碟...")

    buffer_queue.join()

    server_socket.close()

    print("[SYSTEM] Server 結束")


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

---

***
---


## [2026-07-22]
```text
1. 修正 每一封包都會有自己的 sequence number，採遞增方式，使用 4 byte Little Endian，所以，22 4B 00 00 就是 sequence=0x00004B22 。 下一個 400 byte 封包， sequence=0x00004B23 ， 23 4B 00 00 。
2. 因為採用每 1 ms 送出 400 byte，所以，在 Server 端的 RX Speed = 0.38 MB/s ，從 Wireshark 抓封包來看，封包間隔為 997us，接近 1 ms 。 這是正常的。
3. 增加一個 us 計時器，uint32_t GetCustomTime(uint32_t step_us)，當設定為 500us 間隔，因為時間太快，間隔時間太短，寫入時間與讀出時間會跟不上，因為有 32 個 buffer 當作緩衝，觀察 wireshark封包，序號 sequence number 還是連續的，沒有遺失資料。
4. 模擬資料與傳送資料都是在 main 的 while loop 執行，所以，while loop 若有其他工作超過 1 ms 間隔，會有問題。
```


## [2026-07-22]
```text
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

