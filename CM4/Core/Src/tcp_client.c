#include "tcp_client.h"
#include "lwip/tcp.h"
#include <string.h>
#include "data_client.h"

static struct tcp_pcb *tpcb = NULL;
static client_state_t state = CLIENT_IDLE;

/*
 *========================================================
 * Application ACK
 *
 * Python Server 收到 204-byte packet 後，
 * 會回傳 4-byte Application ACK：
 *
 *     Byte0 = 0xAC
 *     Byte1 = 0x4B
 *     Byte2 = ACK counter LSB
 *     Byte3 = ACK counter MSB
 *
 * 注意：
 *
 * 這不是 TCP ACK。
 *
 * 這是 Python application 主動回傳的 ACK。
 *========================================================
 */
static volatile uint32_t app_ack_count = 0;

static volatile uint16_t last_app_ack = 0;


/*
 *========================================================
 * 目前正在等待 ACK 的封包
 *
 * 注意：
 *
 * data_client_release_packet()
 *
 * 必須等 tcp_sent()
 * 才能呼叫
 *
 *========================================================
 */
static DATA_PACKET *current_packet = NULL;

/*
 *========================================================
 * TCP ACK Credit
 *
 * 控制TCP傳送量
 *
 *========================================================
 */
static volatile uint32_t tx_credit = 0;

/*
 *========================================================
 * Debug
 *
 * 記錄上一個sequence
 *
 * 用來判斷資料是否被覆蓋
 *
 *========================================================
 */
static uint32_t last_sequence = 0;


/*
 *========================================================
 * Application ACK Receive Callback
 *
 * Python Server 收到 204-byte data 後，
 * 主動回傳 4-byte Application ACK：
 *
 *     AC 4B XX XX
 *
 * 這裡處理的是：
 *
 *     Python Application ACK
 *
 * 不是 TCP ACK。
 *========================================================
 */
static err_t tcp_client_recv(void *arg,
                             struct tcp_pcb *pcb,
                             struct pbuf *p,
                             err_t err)
{
    (void)arg;

    /*
     *====================================================
     * Connection / Receive Error
     *====================================================
     */
    if(err != ERR_OK)
    {
        if(p != NULL)
        {
            pbuf_free(p);
        }

        return err;
    }

    /*
     *====================================================
     * p == NULL
     *
     * Remote side closed the connection.
     *====================================================
     */
    if(p == NULL)
    {
        state = CLIENT_IDLE;
        tpcb = NULL;
        current_packet = NULL;
        tx_credit = 0;

        HAL_GPIO_WritePin(
            GPIOC,
            GPIO_PIN_6,
            GPIO_PIN_RESET);

        return ERR_OK;
    }

    /*
     *====================================================
     * 告訴 lwIP：
     *
     * 這些資料已經被 application 接收。
     *====================================================
     */
    tcp_recved(pcb, p->tot_len);

    /*
     *====================================================
     * Application ACK 必須是 4 bytes。
     *
     * 這裡先確認整個 pbuf chain 的長度。
     *====================================================
     */
    if(p->tot_len >= 4)
    {
        uint8_t ack[4];

        /*
         * pbuf_copy_partial()
         *
         * 可以正確處理 ACK 被切成多個 pbuf 的情況。
         */
        pbuf_copy_partial(
            p,
            ack,
            4,
            0);

        /*
         *================================================
         * 檢查 Application ACK Signature
         *================================================
         */
        if((ack[0] == 0xAC) &&
           (ack[1] == 0x4B))
        {
            uint16_t ack_number;

            ack_number =
                ((uint16_t)ack[3] << 8) |
                ((uint16_t)ack[2]);

            last_app_ack = ack_number;

            app_ack_count++;

            /*
            *================================================
            * Application ACK Timing End
            *
            * Python Server 已經收到 204-byte packet，
            * 並將 Application ACK 傳回 STM32。
            *
            * ACK 到達 STM32 lwIP receive callback。
            *
            * PC6 = LOW
            *
            * 示波器：
            *
            *     HIGH ─────────────── LOW
            *            <--- T --->
            *
            * T = 送出 → Server ACK 回到 STM32
            *================================================
            */
            HAL_GPIO_WritePin(
                GPIOC,
                GPIO_PIN_6,
                GPIO_PIN_RESET);
                }
    }

    /*
     *====================================================
     * 釋放 pbuf
     *====================================================
     */
    pbuf_free(p);

    return ERR_OK;
}



/*------------------------------------------------------------------
 * Error Callback
 *-----------------------------------------------------------------*/
static void tcp_client_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;

    state = CLIENT_IDLE;
    tpcb = NULL;
    current_packet = NULL;
    tx_credit = 0;

    /*
     * Application ACK timing GPIO
     *
     * 連線異常時強制回到 LOW，
     * 避免示波器一直維持 HIGH。
     */
    HAL_GPIO_WritePin(
        GPIOC,
        GPIO_PIN_6,
        GPIO_PIN_RESET);

    HAL_GPIO_WritePin(
        GPIOB,
        GPIO_PIN_14,
        GPIO_PIN_SET);
}


/*
 *========================================================
 * ACK Callback
 *
 * tcp_sent() 被呼叫代表：
 *
 * Server 已經 ACK 了先前送出的 TCP data。
 *
 * 因此在這裡才正式釋放 Ring Buffer 封包。
 *
 * 這一點非常重要：
 *
 * tcp_write() 成功
 *     !=
 * Server 已經收到
 *
 * tcp_sent()
 *     =
 * TCP stack 已收到對方 ACK
 *
 * 後續 GPIO timing 也會以這裡作為
 * 「收到 Server TCP ACK」的時間點。
 *========================================================
 */
static err_t tcp_client_sent(void *arg,
                             struct tcp_pcb *pcb,
                             u16_t len)
{
    (void)arg;
    (void)pcb;

    /*
     * TCP ACK credit
     *
     * Server ACK 了多少 bytes，
     * 就增加多少 credit。
     */
    tx_credit += len;

    /*
     *====================================================
     * Ring Buffer 封包只能在 TCP ACK 後釋放
     *====================================================
     */
    if(current_packet != NULL)
    {
        data_client_release_packet();

        current_packet = NULL;
    }

    return ERR_OK;
}


/*------------------------------------------------------------------
 * Connected Callback
 *-----------------------------------------------------------------*/
static err_t tcp_client_connected(void *arg,
                                  struct tcp_pcb *pcb,
                                  err_t err)
{
    if(err == ERR_OK)
    {        
        tcp_nagle_disable(pcb);

        tcp_sent(pcb, tcp_client_sent);

        tcp_recv(pcb, tcp_client_recv);

        //tx_credit = BUFFER_SIZE;
        tx_credit = DATA_PACKET_SIZE;

        state = CLIENT_CONNECTED;

        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);// 與 server 連上線，紅燈熄滅

        return ERR_OK;
    }

    return err;
}


/*------------------------------------------------------------------
 * Init
 *-----------------------------------------------------------------*/
void tcp_client_init(void)
{
    /*
     *====================================================
     * PC6 GPIO 初始化
     *
     * NUCLEO-H755ZI-Q
     * CN7 Pin 1 = PC6
     *
     * PC6 用於示波器時間量測：
     *
     *     HIGH = 204-byte packet 已開始傳送
     *     LOW  = Python Application ACK 已收到
     *====================================================
     */

    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /*
     * 初始狀態：
     *
     * 沒有等待 Application ACK
     *
     * 所以 PC6 = LOW
     */
    HAL_GPIO_WritePin(
        GPIOC,
        GPIO_PIN_6,
        GPIO_PIN_RESET);


    last_sequence = 0;

    /*
     *====================================================
     * Application ACK Debug
     *====================================================
     */
    app_ack_count = 0;
    last_app_ack = 0;
}


/*------------------------------------------------------------------
 * Main Handler
 *-----------------------------------------------------------------*/
void tcp_client_handler(void)
{
    switch(state)
    {
        case CLIENT_IDLE:
        {
            tpcb = tcp_new();

            if(tpcb != NULL)
            {
                tcp_err(tpcb,
                        tcp_client_err);

                state = CLIENT_CONNECTING;                

                ip_addr_t dest_ip;

                if(ipaddr_aton(RPI_SERVER_IP,
                               &dest_ip))
                {
                    err_t err;

                    err = tcp_connect(
                                tpcb,
                                &dest_ip,
                                RPI_SERVER_PORT,
                                tcp_client_connected);

                    if(err != ERR_OK)
                    {
                        tcp_abort(tpcb);

                        tpcb = NULL;

                        state = CLIENT_IDLE;

                        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);// 亮紅燈 (PB14)
                    }
                }
            }

            break;
        }

        case CLIENT_CONNECTING:
        {
            /*
             * 等待 tcp_client_connected()
             */
            break;
        }

        case CLIENT_CONNECTED:
        {
            if(tpcb == NULL)
            {
                state = CLIENT_IDLE;
                break;
            }

            /*
            *====================================================
            * 每次至少要有一個完整 204-byte packet 的 credit
            * 才允許送出。
            *
            * tx_credit 來自：
            *
            * 1. tcp_client_connected()
            * 2. tcp_client_sent()
            *====================================================
            */
            while(tx_credit >= DATA_PACKET_SIZE)
            {
                /*
                *================================================
                * 取得目前 Ring Buffer 封包
                *================================================
                */
                current_packet = data_client_get_packet();

                if(current_packet == NULL)
                {
                    break;
                }

                /*
                *================================================
                * TCP send buffer 確認
                *
                * 目前真正資料長度為 204 bytes。
                *
                * 不再要求 400 bytes。
                *================================================
                */
                if(tcp_sndbuf(tpcb) < current_packet->length)
                {
                    current_packet = NULL;
                    break;
                }

                /*
                *================================================
                * TCP queue 數量確認
                *================================================
                */
                if(tpcb->snd_queuelen >= TCP_SND_QUEUELEN)
                {
                    current_packet = NULL;
                    break;
                }

                /*
                * Debug LED
                */
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);

                /*
                *================================================
                * Sequence Debug
                *
                * 第一個 packet 的 sequence = 0，
                * 因此不把 sequence = 0 當成錯誤。
                *
                * 後續 packet 必須連續。
                *================================================
                */
                if(last_sequence != 0)
                {
                    if(current_packet->sequence != last_sequence + 1)
                    {
                        /*
                        * Producer 速度 > TCP Consumer
                        * 可能造成 Ring Buffer 被覆蓋。
                        */
                        if(current_packet->sequence > last_sequence + 1)
                        {
                            HAL_GPIO_WritePin(
                                GPIOB,
                                GPIO_PIN_0,
                                GPIO_PIN_SET);
                        }
                        else
                        {
                            HAL_GPIO_WritePin(
                                GPIOE,
                                GPIO_PIN_1,
                                GPIO_PIN_SET);
                        }
                    }
                }

                /*
                *================================================
                * 重複 sequence Debug
                *================================================
                */
                if(current_packet->sequence == last_sequence &&
                last_sequence != 0)
                {
                    HAL_GPIO_WritePin(
                        GPIOB,
                        GPIO_PIN_14,
                        GPIO_PIN_SET);
                }
                else
                {
                    HAL_GPIO_WritePin(
                        GPIOB,
                        GPIO_PIN_14,
                        GPIO_PIN_RESET);
                }

                last_sequence = current_packet->sequence;

                /*
                *================================================
                * 傳送真正的 EtherCAT Data
                *
                * 以前：
                *
                *     BUFFER_SIZE = 400 bytes
                *
                * 現在：
                *
                *     current_packet->length = 204 bytes
                *
                * 因此 TCP 真正送出的資料只有 204 bytes。
                *================================================
                */
                err_t err;

                err = tcp_write(
                            tpcb,
                            current_packet->data,
                            current_packet->length,
                            TCP_WRITE_FLAG_COPY);

                if(err == ERR_OK)
                {
                    /*
                    *================================================
                    * 扣除本次送出的 204 bytes credit
                    *================================================
                    */
                    tx_credit -= current_packet->length;

                    /*
                    *================================================
                    * GPIO Timing Start
                    *
                    * tcp_write() 已成功將 204-byte packet
                    * 交給 lwIP。
                    *
                    * 先拉高 PC6，再呼叫 tcp_output()
                    * 要求 lwIP 立即送出 TCP data。
                    *
                    * 示波器：
                    *
                    *     HIGH ───────────────── LOW
                    *             <--- T --->
                    *
                    * T = STM32 啟動傳送
                    *     到收到 Python Application ACK
                    *================================================
                    */
                    HAL_GPIO_WritePin(
                        GPIOC,
                        GPIO_PIN_6,
                        GPIO_PIN_SET);


                    /*
                    * 立即要求 lwIP 發送 TCP data
                    */
                    tcp_output(tpcb);

                    /*
                    *================================================
                    * 注意！
                    *
                    * 這裡絕對不能呼叫：
                    *
                    *     data_client_release_packet();
                    *
                    * 因為 tcp_write() 成功不代表 Server
                    * 已經 ACK。
                    *
                    * current_packet 必須保持有效，
                    * 等 tcp_client_sent() 被呼叫後，
                    * 才 release。
                    *================================================
                    */

                }
                else
                {
                    /*
                    * tcp_write() 失敗。
                    *
                    * 此 packet 尚未被接受，
                    * 所以不能 release。
                    */
                    current_packet = NULL;

                    break;
                }

                /*
                *================================================
                * 第一階段測試採用：
                *
                * 一次只允許一個 packet 在等待 ACK。
                *
                * tcp_write() 後 tx_credit 已經減到 0，
                * 因此 while 條件自然停止。
                *================================================
                */
            }

            break;
        }

        default:
        {
            state = CLIENT_IDLE;
            break;
        }
    }
}
