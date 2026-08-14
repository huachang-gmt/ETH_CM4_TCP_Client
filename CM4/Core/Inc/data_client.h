#ifndef DATA_CLIENT_H
#define DATA_CLIENT_H

#include "stdint.h"

#define DATA_PACKET_SIZE     204
#define BUFFER_SIZE          400
#define BUFFER_COUNT         32

typedef struct
{
    uint16_t length;  // 有效EtherCAT資料長度
    uint32_t sequence; // 資料序號

    uint8_t  data[BUFFER_SIZE]; //  TCP送出的400 byte

} DATA_PACKET;


void data_client_init(void); // 初始化

void data_client_process(void); // EtherCAT simulator  每1ms呼叫一次

/*----------------------------------------------------------
 * 提供給 tcp_client 使用
 *---------------------------------------------------------*/
DATA_PACKET *data_client_get_packet(void); // TCP取得資料

void data_client_release_packet(void); // TCP送完後 read pointer往前移

uint16_t data_client_available(void); // 目前等待傳送數量

uint16_t data_client_get_write_index(void); // 取得write index

uint16_t data_client_get_read_index(void); // 取得read index


#endif