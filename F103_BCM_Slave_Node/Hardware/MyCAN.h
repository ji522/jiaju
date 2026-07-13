#ifndef __MYCAN_H
#define __MYCAN_H

#include <stdint.h>

typedef struct
{
	uint32_t esr;
	uint8_t tec;
	uint8_t rec;
	uint8_t lec;
	uint32_t tx_fail_count;
	uint8_t last_tx_status;
} MyCanDiag;

void MyCAN_Init(void);
int MyCAN_Transmit(uint32_t ID, uint8_t Length, uint8_t *Data);
uint8_t MyCAN_ReceiveFlag(void);
void MyCAN_Receive(uint32_t *ID, uint8_t *Length, uint8_t *Data);
void MyCAN_GetDiag(MyCanDiag *diag);

#endif
