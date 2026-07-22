#include "tcp_client.h"
#include "lwip/tcp.h"
#include <string.h>
#include "data_client.h"

static struct tcp_pcb *tpcb = NULL;
static client_state_t state = CLIENT_IDLE;

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

    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);      // 紅燈亮表示斷線
}


/*
 *========================================================
 * ACK Callback
 *
 * TCP真正收到ACK後
 *
 * 才代表上一包資料完成
 *
 *========================================================
 */
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
        tcp_nagle_disable(pcb);

        tcp_sent(pcb, tcp_client_sent);

        tx_credit = BUFFER_SIZE * 2;

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
    last_sequence = 0;

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
                        

            while(tx_credit >= BUFFER_SIZE)
            {
                
                current_packet = data_client_get_packet();

                if(current_packet == NULL)
                {
                    break;
                }             
                

                /*
                * TCP buffer確認
                */
                if(tcp_sndbuf(tpcb) < BUFFER_SIZE)
                {
                    current_packet = NULL;
                    break;
                }


                if(tpcb->snd_queuelen >= TCP_SND_QUEUELEN)
                {
                    current_packet = NULL;
                    break;
                }

                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);// 熄滅綠燈
                HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_RESET);// 熄滅黃燈
                HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);// 熄滅紅燈
                /*
                *================================================
                * Sequence Debug
                *
                * 如果跳號
                *
                * 表示Ring Buffer被覆蓋
                *
                *================================================
                */
                /*
                * 紅燈快速閃一下
                * EtherCAT Producer 速度 > TCP Consumer 中間資料被覆蓋
                * 表示資料跳號
                */
                
                if(current_packet->sequence != last_sequence + 1)
                {
                    if(current_packet->sequence >= last_sequence + 1)
                        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);// 亮綠燈 (PB0)
                    else
                        HAL_GPIO_WritePin(GPIOE, GPIO_PIN_1, GPIO_PIN_SET);// 亮黃燈 (PE1)

                }

                if(current_packet->sequence == last_sequence)
                    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);// 亮紅燈 (PB14)
                else
                    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);// 熄滅紅燈
                

                last_sequence = current_packet->sequence;

                err_t err;
               
                err = tcp_write(
                            tpcb,
                            current_packet->data,
                            BUFFER_SIZE,
                            TCP_WRITE_FLAG_COPY);

                if(err == ERR_OK)
                {
                    tx_credit -= BUFFER_SIZE;

                    tcp_output(tpcb);

                    data_client_release_packet();
                    current_packet = NULL;

                }
                else
                {
                    current_packet = NULL;                   

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
