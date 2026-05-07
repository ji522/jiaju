/**
 * @file can_bcm_protocol.h
 * @brief Shared CAN protocol definition for the BCM gateway/master and slave node.
 */

#ifndef __CAN_BCM_PROTOCOL_H
#define __CAN_BCM_PROTOCOL_H

#include <stdint.h>

/* Node link mode selection is local to each project.
 * LOOPBACK is useful for single-board bring-up, while NORMAL is used when
 * the F407 master talks to the F103 slave over a real CAN bus. */
#define CAN_LINK_MODE_LOOPBACK 0U
#define CAN_LINK_MODE_NORMAL   1U

/* Application-level CAN IDs used by the BCM demo network. */
#define CAN_ID_BODY_CMD         0x100U
#define CAN_ID_BODY_STATUS      0x101U
#define CAN_ID_NODE_HEARTBEAT   0x1F0U

/* Byte0 bits used by BODY_CMD/BODY_STATUS. */
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

#endif
