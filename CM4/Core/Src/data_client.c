#include "data_client.h"

#include "main.h"
#include <string.h>

/*----------------------------------------------------------
 * Ring Buffer
 *---------------------------------------------------------*/
static DATA_PACKET ring[BUFFER_COUNT]; // 32個400 byte buffer

/*----------------------------------------------------------
 * 真正的 204-byte EtherCAT 測試資料
 *
 * 此資料由另一個資料來源每 1ms 更新一次。
 *
 * 目前第一階段先使用這組資料驗證：
 * STM32H755 CM4 -> TCP Client -> TCP Server
 *
 * 後續再將這裡替換成實際 EtherCAT 資料來源。
 *---------------------------------------------------------*/
static const uint8_t test[204] =
{
    0xA7, 0x3C, 0x91, 0xE2, 0x5B, 0x08, 0xD4, 0x6F,
    0x19, 0xC3, 0x72, 0xAE, 0x45, 0xF8, 0x0D, 0xB6,
    0x53, 0x9A, 0x27, 0xE1, 0x6C, 0x04, 0xD9, 0x8F,
    0x31, 0xB2, 0x5E, 0xC7, 0x16, 0xFA, 0x83, 0x49,
    0xD0, 0x25, 0x68, 0xAC, 0xF3, 0x17, 0x9E, 0x42,
    0x7B, 0xCE, 0x03, 0x95, 0xD8, 0x21, 0x6A, 0xB4,
    0xE7, 0x38, 0x5D, 0xA1, 0x0F, 0xC9, 0x74, 0x2B,
    0x86, 0xF5, 0x13, 0xDA, 0x4E, 0x60, 0xB8, 0x97,
    0x2F, 0xC1, 0x5A, 0x09, 0xED, 0x73, 0x34, 0xA6,
    0x18, 0xD2, 0x8B, 0x4F, 0xC6, 0x57, 0x90, 0x3E,
    0xFB, 0x24, 0x69, 0xA8, 0x12, 0xDC, 0x45, 0x7F,
    0x83, 0x0A, 0xB5, 0xE9, 0x36, 0x5C, 0xD7, 0x41,
    0x98, 0x2D, 0xF0, 0x64, 0x17, 0xCA, 0x53, 0x8E,
    0xB1, 0x76, 0x0C, 0xE4, 0x39, 0xAD, 0x62, 0xF7,
    0x20, 0x5F, 0xC8, 0x93, 0x14, 0xEA, 0x47, 0x6B,
    0xD5, 0x82, 0x0E, 0xB9, 0x3A, 0xF1, 0x65, 0xAC,
    0x28, 0x74, 0xD3, 0x09, 0xBE, 0x51, 0xE6, 0x3F,
    0x8A, 0xC4, 0x16, 0x70, 0xF9, 0x35, 0xA2, 0x5B,
    0xDB, 0x43, 0x87, 0x1C, 0xE0, 0x6D, 0xB3, 0x52,
    0x0B, 0xCF, 0x79, 0x24, 0xA5, 0x68, 0xF4, 0x31,
    0x9D, 0x47, 0xC2, 0x15, 0xEA, 0x80, 0x36, 0x5E,
    0xB7, 0x04, 0xD1, 0x92, 0x6F, 0x28, 0xFA, 0x53,
    0x81, 0xCE, 0x39, 0xA7, 0x10, 0xD6, 0x4B, 0x75,
    0xE3, 0x2A, 0x96, 0x5D, 0x08, 0xBC, 0x61, 0xF2,
    0x44, 0x89, 0x17, 0xDA
};


static volatile uint16_t write_index = 0; // EtherCAT寫入位置
static volatile uint16_t read_index = 0; // TCP讀取位置
static volatile uint32_t write_count = 0; // 寫入總數 不會因為index繞圈而失去資訊
static volatile uint32_t read_count = 0; // 讀取總數

static uint32_t sequence = 0; // EtherCAT資料序號

/*----------------------------------------------------------
 * 每1ms呼叫一次
 *---------------------------------------------------------*/
static uint32_t last_tick = 0; // 1ms Timer



/**
 * @brief  取得自開機以來的自定義微秒/高解析度時間
 * @param  step_us: 每個 SysTick 週期代表的微秒數 (例如: 1000代表1ms, 500代表500us, 800代表800us)
 * @retval 計算出的時間數值
    如何使用？
    你可以直接傳入你想要的微秒數作為參數：
    取得標準毫秒 (1000us)：
    uint32_t time_ms = GetCustomTime(1000);
    取得 500us 解析度的時間：
    uint32_t time_500us = GetCustomTime(500);
 */
uint32_t GetCustomTime(uint32_t step_us) {
    uint32_t m = HAL_GetTick();
    uint32_t tms = SysTick->LOAD + 1;
    uint32_t u = tms - SysTick->VAL;

    if ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) != 0) {
        m = HAL_GetTick();
        u = tms - SysTick->VAL;
    }
    
    // m * step_us: 已經過的大單位時間
    // (u * step_us) / tms: 當前 SysTick 週期內的精細補償
    return (m * step_us + (u * step_us) / tms);
}


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

    last_tick = HAL_GetTick(); // 1ms 間隔時間
    //last_tick = GetCustomTime(500); // // 0.5ms timer  採用 0.5ms 間隔方式，封包傳送可能會遺失，因為間隔時間太短
}

/*----------------------------------------------------------
 * 每1ms呼叫一次
 *
 * 功能：
 * 1. 每 1ms 建立一個 DATA_PACKET
 * 2. 將真正的 204-byte 資料放入 packet
 * 3. packet->length = 204
 * 4. 剩餘 buffer 空間清零
 * 5. Ring Buffer write pointer 往下一格
 *
 * 注意：
 * 目前先不加入 GPIO。
 * GPIO timing / Server ACK 將在第一階段資料傳送確認
 * 成功後，再進行第二階段修改。
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
     * 設定真正資料長度
     *
     * 原本：
     *     DATA_PACKET_SIZE = 312
     *
     * 現在：
     *     DATA_PACKET_SIZE = 204
     *=====================================================
     */
    pkt->length = DATA_PACKET_SIZE;

    /*
     *=====================================================
     * 保留 sequence
     *
     * sequence 不再放入 TCP payload。
     *
     * 它目前仍然可以用來追蹤：
     * 第幾筆 1ms 資料。
     *=====================================================
     */
    pkt->sequence = sequence;

    sequence++;

    /*
     *=====================================================
     * 複製真正的 204-byte 資料
     *
     * TCP payload：
     *
     * data[0]   ~ data[203]
     *     ↓
     *     真正的 204-byte EtherCAT 資料
     *=====================================================
     */
    memcpy(pkt->data,
           test,
           DATA_PACKET_SIZE);

    /*
     *=====================================================
     * 剩餘 buffer 清零
     *
     * data[204] ~ data[399] = 0
     *
     * 注意：
     * pkt->length = 204
     *
     * 所以正常 TCP Client 應該只傳送前 204 bytes，
     * 後面的 196 bytes 不應該被送出去。
     *=====================================================
     */
    memset(&pkt->data[DATA_PACKET_SIZE],
           0,
           BUFFER_SIZE - DATA_PACKET_SIZE);

    /*
     * Producer 往下一格
     */
    write_index++;

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







































