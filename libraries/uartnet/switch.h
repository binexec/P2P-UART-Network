#ifndef _UARTNET_SWITCHH_
#define _UARTNET_SWITCHH_

//#include "link_layer/link.h"
#include <link.h>


#include <TimerOne.h>


//Link Layer Configuration
#define TOTAL_LINKS         3
#define SEND_QUEUE_SIZE     6
#define RECV_BUFFER_SIZE    2*(MAX_PAYLOAD_SIZE + 16)     //add extra bytes for headers and other
#define FLUSH_THRESHOLD     RECV_BUFFER_SIZE * 0.5


void switch_init();
void switch_task(uint8_t continuous);


#endif
