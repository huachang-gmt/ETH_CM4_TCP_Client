#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

//#define RPI_SERVER_IP "192.168.88.200"  //  Raspberry Pi 5 IP 位址
#define RPI_SERVER_IP "192.168.88.100"  // 我的電腦位址 
#define RPI_SERVER_PORT 8888
#define BUFFER_SIZE 400

// 狀態機定義
typedef enum {
    CLIENT_IDLE,
    CLIENT_CONNECTING,
    CLIENT_CONNECTED,
    CLIENT_SENDING
} client_state_t;

void tcp_client_init(void);
void tcp_client_handler(void);

#endif