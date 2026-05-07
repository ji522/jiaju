/**
 * @file driver_can.h
 * @brief F407 CAN1 驱动头文件（Loopback 模式测试）
 */
#ifndef __DRIVER_CAN_H
#define __DRIVER_CAN_H

#include "stm32f4xx_hal.h"

/* Simple application-level CAN IDs for the gateway prototype. */
#define CAN_ID_BODY_CMD         0x100U
#define CAN_ID_BODY_STATUS      0x101U
#define CAN_ID_NODE_HEARTBEAT   0x1F0U

/* Body-control bit definitions carried in CAN_ID_BODY_CMD byte0. */
#define CAN_BODY_CTRL_LAMP      (1U << 0)
#define CAN_BODY_CTRL_HAZARD    (1U << 1)
#define CAN_BODY_CTRL_FAN       (1U << 2)

typedef struct
{
	uint32_t id;
	uint8_t dlc;
	uint8_t data[8];
} CanFrame;

typedef enum
{
	CAN_NODE_INIT = 0,
	CAN_NODE_NORMAL = 1,
	CAN_NODE_DEGRADED = 2,
	CAN_NODE_FAULT = 3
} CanNodeMode;

int Driver_CAN_Init(void);
int Driver_CAN_Send(uint32_t id, uint8_t *data, uint8_t len);
int Driver_CAN_Recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms);

#endif
