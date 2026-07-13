/**
 * @file driver_can.h
 * @brief F407 CAN1 驱动头文件（Loopback 模式测试）
 */
#ifndef __DRIVER_CAN_H
#define __DRIVER_CAN_H

#include "stm32f4xx_hal.h"
#include "can_bcm_protocol.h"

#ifndef CAN_LINK_MODE
#define CAN_LINK_MODE CAN_LINK_MODE_NORMAL
#endif

int Driver_CAN_Init(void);
int Driver_CAN_Send(uint32_t id, uint8_t *data, uint8_t len);
int Driver_CAN_Recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms);
void Driver_CAN_IRQHandler(void);
uint8_t Driver_CAN_TakeErrorSnapshot(uint32_t *error, uint32_t *esr);
uint32_t Driver_CAN_TakeRxDropCount(void);

/* Optional helper for future diagnostics / Bus-Off handling. */
uint32_t Driver_CAN_GetError(void);
uint32_t Driver_CAN_GetESR(void);

#endif
