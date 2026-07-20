#include "tcp_client.h"
#include "lwip/tcp.h"
#include <string.h>

static struct tcp_pcb *tpcb = NULL;
static client_state_t state = CLIENT_IDLE;

static uint8_t payload[BUFFER_SIZE];

/*------------------------------------------------------------------
 * 修改1：
 * 使用 ACK credit 機制取代固定 1ms 發送
 *-----------------------------------------------------------------*/
static volatile uint32_t tx_credit = 0;


/*------------------------------------------------------------------
 * Error Callback
 *-----------------------------------------------------------------*/
static void tcp_client_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;

    state = CLIENT_IDLE;
    tpcb = NULL;
    tx_credit = 0;

    HAL_GPIO_WritePin(GPIOB,
                      GPIO_PIN_14,
                      GPIO_PIN_SET);      // 紅燈亮表示斷線
}


/*------------------------------------------------------------------
 * 修改2：
 * tcp_sent callback
 *
 * 每當對方 ACK 某些資料時，
 * len 就是本次被 ACK 的 byte 數量。
 *-----------------------------------------------------------------*/
static err_t tcp_client_sent(void *arg,
                             struct tcp_pcb *pcb,
                             u16_t len)
{
    (void)arg;
    (void)pcb;

    tx_credit += len;

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
        /*----------------------------------------------------------
         * 修改3：
         * 關閉 Nagle
         *---------------------------------------------------------*/
        tcp_nagle_disable(pcb);

        /*----------------------------------------------------------
         * 修改4：
         * 註冊 ACK callback
         *---------------------------------------------------------*/
        tcp_sent(pcb,
                 tcp_client_sent);

        /*
         * 初始允許先送兩包
         */
        tx_credit = BUFFER_SIZE * 2;

        state = CLIENT_CONNECTED;

        return ERR_OK;
    }

    return err;
}


/*------------------------------------------------------------------
 * Init
 *-----------------------------------------------------------------*/
void tcp_client_init(void)
{
    memset(payload,
           'A',
           BUFFER_SIZE - 2);

    payload[BUFFER_SIZE - 2] = '\r';
    payload[BUFFER_SIZE - 1] = '\n';
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

                HAL_GPIO_WritePin(GPIOB,
                                  GPIO_PIN_14,
                                  GPIO_PIN_RESET);

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

                        HAL_GPIO_WritePin(GPIOB,
                                          GPIO_PIN_14,
                                          GPIO_PIN_SET);
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

            /*------------------------------------------------------
             * 修改5：
             * 有 credit 才能發送
             *-----------------------------------------------------*/
            while(tx_credit >= BUFFER_SIZE)
            {
                /*--------------------------------------------------
                 * 修改6：
                 * 同時檢查 sndbuf
                 *-------------------------------------------------*/
                if(tcp_sndbuf(tpcb) < BUFFER_SIZE)
                {
                    break;
                }

                /*--------------------------------------------------
                 * 修改7：
                 * 同時檢查 segment queue
                 *-------------------------------------------------*/
                if(tpcb->snd_queuelen >= TCP_SND_QUEUELEN)
                {
                    break;
                }

                err_t err;

                /*--------------------------------------------------
                 * 修改8：
                 * 不使用 COPY
                 *-------------------------------------------------*/
                err = tcp_write(
                            tpcb,
                            payload,
                            BUFFER_SIZE,
                            0); // 如果 payload 永遠是同一塊

                if(err == ERR_OK)
                {
                    tx_credit -= BUFFER_SIZE;

                    tcp_output(tpcb);

                    HAL_GPIO_TogglePin(
                        GPIOE,
                        GPIO_PIN_1);      // 黃燈
                }
                else
                {
                    /*----------------------------------------------
                     * 修改9：
                     * 顯示資源不足
                     *---------------------------------------------*/
                    HAL_GPIO_TogglePin(
                        GPIOB,
                        GPIO_PIN_0);      // 綠燈

                    break;
                }
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
