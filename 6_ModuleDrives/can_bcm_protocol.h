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

/* BODY_CMD: Byte0=control bitmask, Byte1=command sequence.
 * BODY_STATUS: Byte0=status bitmask, Byte1=node mode, Byte2=echoed sequence. */
#define CAN_BODY_CMD_BYTE_MASK      0U
#define CAN_BODY_CMD_BYTE_SEQ       1U

#define CAN_BODY_STATUS_BYTE_MASK   0U
#define CAN_BODY_STATUS_BYTE_MODE   1U
#define CAN_BODY_STATUS_BYTE_SEQ    2U

#define CAN_HEARTBEAT_BYTE_MODE     0U
#define CAN_HEARTBEAT_BYTE_MASK     1U
#define CAN_HEARTBEAT_BYTE_SEQ      2U

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

typedef enum
{
	CAN_DTC_NONE = 0x00U,
	CAN_DTC_TX_FAIL = 0x01U,
	CAN_DTC_NODE_TIMEOUT = 0x02U,
	CAN_DTC_SEQ_TIMEOUT = 0x03U,
	CAN_DTC_SEQ_MISMATCH = 0x04U
} CanDtcCode;

#define CAN_DTC_MASK_TX_FAIL       (1UL << 0)
#define CAN_DTC_MASK_NODE_TIMEOUT  (1UL << 1)
#define CAN_DTC_MASK_SEQ_TIMEOUT   (1UL << 2)
#define CAN_DTC_MASK_SEQ_MISMATCH  (1UL << 3)

#endif
