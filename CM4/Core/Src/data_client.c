#include "data_client.h"

#include "main.h"
#include <string.h>

/*----------------------------------------------------------
 * Ring Buffer
 *---------------------------------------------------------*/
static DATA_PACKET ring[BUFFER_COUNT]; // 32個400 byte buffer

static volatile uint16_t write_index = 0; // EtherCAT寫入位置
static volatile uint16_t read_index = 0; // TCP讀取位置
static volatile uint32_t write_count = 0; // 寫入總數 不會因為index繞圈而失去資訊
static volatile uint32_t read_count = 0; // 讀取總數

static uint32_t sequence = 0; // EtherCAT資料序號

/*----------------------------------------------------------
 * 每1ms呼叫一次
 *---------------------------------------------------------*/
static uint32_t last_tick = 0; // 1ms Timer

/*----------------------------------------------------------
 * 初始化
 *---------------------------------------------------------*/
void data_client_init(void)
{
    memset(ring, 0, sizeof(ring));

    write_index = 0;
    read_index = 0;
    write_count = 0;
    read_count = 0;
    sequence = 0;

    last_tick = HAL_GetTick();
}

/*----------------------------------------------------------
 * 模擬 EtherCAT
 *
 * 每1ms產生312Bytes

    封包內容如下 
    TCP Payload：
    Offset
    0~3      sequence(uint32_t)
    4~311    Pattern
    312~399  0
    也就是
        Byte0
        Byte1
        Byte2
        Byte3
    就是 sequence。
    STM32 是 Little Endian
    STM32H755 是 ARM Cortex-M4。
    因此
    sequence = 1;
    封包會變成
    01 00 00 00
 *---------------------------------------------------------*/
void data_client_process(void)
{
    uint32_t now;

    now = HAL_GetTick();

    /*
     * 1ms一次
     */
    if(now == last_tick)
    {
        return;
    }

    last_tick = now;

    DATA_PACKET *pkt;

    /*
     * 取得目前寫入buffer
     */
    pkt = &ring[write_index];

    /*
     *=====================================================
     * 直接覆蓋
     *
     * 不檢查buffer是否被讀走
     *
     * EtherCAT不能停止
     *=====================================================
     */

    pkt->length = DATA_PACKET_SIZE;

    pkt->sequence = sequence;

    sequence++;

    /*
     * sequence overflow
     *
     * uint32_t自然回0
     */

    /*
     *=====================================================
     * 模擬 EtherCAT Data
     *
     * 前4 byte:
     * sequence
     *
     * 後面:
     * pattern
        400 byte payload
            =
            4 byte sequence
            308 byte pattern
            88 byte zero
     *=====================================================
     */

    memcpy(pkt->data,
           &pkt->sequence,
           sizeof(uint32_t));

    for(uint32_t i = 4;
        i < DATA_PACKET_SIZE;
        i++)
    {

        pkt->data[i] = (uint8_t)i;

    }

    /*
     * 312~399補0
     */
    memset(&pkt->data[DATA_PACKET_SIZE],
           0,
           BUFFER_SIZE - DATA_PACKET_SIZE);

    /*
     * Producer往下一格
     */
    write_index++;

    //HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0); // 綠燈閃爍 (PB0)

    if(write_index >= BUFFER_COUNT)
    {
        write_index = 0;
    }

    write_count++;

}

/*----------------------------------------------------------
 * 給TCP取得下一包
 *---------------------------------------------------------*/
DATA_PACKET *data_client_get_packet(void)
{
    /*
     * 沒有新資料
     */
    if(read_count == write_count)
    {
        return NULL;
    }

    /*
     * 回傳目前read位置
     */
    return &ring[read_index];
}

/*----------------------------------------------------------
 * TCP送完後呼叫   read pointer前進
 *---------------------------------------------------------*/
void data_client_release_packet(void)
{
    read_index++;

    //HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_1); // 黃燈閃爍 (PE1)

    if(read_index >= BUFFER_COUNT)
    {
        read_index = 0;
    }
    read_count++;
}

/*
 *=========================================================
 * 目前等待資料數量
 *
 * Debug
 *
 *=========================================================
 */

uint16_t data_client_available(void)
{

    uint32_t count;


    count = write_count - read_count;



    /*
     * 最大只可能32筆
     *
     * 如果TCP太慢
     *
     * 顯示32即可
     */
    if(count > BUFFER_COUNT)
    {
        count = BUFFER_COUNT;
    }



    return (uint16_t)count;

}



/*
 *=========================================================
 * Debug
 *=========================================================
 */

uint16_t data_client_get_write_index(void)
{

    return write_index;

}



uint16_t data_client_get_read_index(void)
{

    return read_index;

}







































