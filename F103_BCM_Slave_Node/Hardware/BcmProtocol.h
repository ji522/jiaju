#ifndef __BCM_PROTOCOL_H
#define __BCM_PROTOCOL_H

#include <stdint.h>

#define CAN_ID_BODY_CMD         0x100U
#define CAN_ID_BODY_STATUS      0x101U
#define CAN_ID_NODE_HEARTBEAT   0x1F0U

#define CAN_BODY_CTRL_LAMP      (1U << 0)
#define CAN_BODY_CTRL_HAZARD    (1U << 1)
#define CAN_BODY_CTRL_FAN       (1U << 2)

typedef enum
{
	CAN_NODE_INIT = 0,
	CAN_NODE_NORMAL = 1,
	CAN_NODE_DEGRADED = 2,
	CAN_NODE_FAULT = 3
} CanNodeMode;

#endif
