#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "LED.h"
#include "MyCAN.h"
#include "BcmProtocol.h"

#define BCM_CMD_TIMEOUT_MS        1000U
#define BCM_HEARTBEAT_PERIOD_MS    500U

uint8_t BodyOutputMask = 0;
uint8_t LastCmdSeq = 0;
uint8_t BodyNodeMode = CAN_NODE_NORMAL;

static void BCM_ApplyOutputs(uint8_t bodyMask)
{
	BodyOutputMask = bodyMask;

	if ((bodyMask & CAN_BODY_CTRL_LAMP) != 0U)
	{
		LED1_ON();
	}
	else
	{
		LED1_OFF();
	}

	/* Use LED2 as a visible secondary actuator indicator.
	 * Map HAZARD to LED2 for easy bench verification. */
	if ((bodyMask & CAN_BODY_CTRL_HAZARD) != 0U)
	{
		LED2_ON();
	}
	else
	{
		LED2_OFF();
	}
}

static void BCM_SendBodyStatus(void)
{
	uint8_t TxData[3];

	TxData[CAN_BODY_STATUS_BYTE_MASK] = BodyOutputMask;
	TxData[CAN_BODY_STATUS_BYTE_MODE] = BodyNodeMode;
	TxData[CAN_BODY_STATUS_BYTE_SEQ] = LastCmdSeq;
	MyCAN_Transmit(CAN_ID_BODY_STATUS, 3, TxData);
}

static void BCM_SendHeartbeat(void)
{
	uint8_t TxData[3];

	TxData[CAN_HEARTBEAT_BYTE_MODE] = BodyNodeMode;
	TxData[CAN_HEARTBEAT_BYTE_MASK] = BodyOutputMask;
	TxData[CAN_HEARTBEAT_BYTE_SEQ] = LastCmdSeq;
	MyCAN_Transmit(CAN_ID_NODE_HEARTBEAT, 3, TxData);
}

int main(void)
{
	uint32_t RxID = 0;
	uint8_t RxLength = 0;
	uint8_t RxData[8] = {0};
	uint16_t cmdTimeoutMs = 0;
	uint16_t heartbeatElapsedMs = 0;
	uint8_t commandSeen = 0;

	LED_Init();
	LED1_OFF();
	LED2_OFF();
	MyCAN_Init();

	while (1)
	{
		if (MyCAN_ReceiveFlag())
		{
			MyCAN_Receive(&RxID, &RxLength, RxData);

			if (RxID == CAN_ID_BODY_CMD && RxLength >= 1U)
			{
				LastCmdSeq = (RxLength > CAN_BODY_CMD_BYTE_SEQ) ?
					RxData[CAN_BODY_CMD_BYTE_SEQ] : 0U;
				BodyNodeMode = CAN_NODE_NORMAL;
				commandSeen = 1U;
				cmdTimeoutMs = 0U;
				BCM_ApplyOutputs(RxData[CAN_BODY_CMD_BYTE_MASK]);
				BCM_SendBodyStatus();
			}
		}

		if (commandSeen != 0U)
		{
			if (cmdTimeoutMs < BCM_CMD_TIMEOUT_MS)
			{
				cmdTimeoutMs++;
			}
			else if (BodyNodeMode != CAN_NODE_DEGRADED)
			{
				/* Communication timeout only degrades the node state.
				 * Keep the last valid actuator output instead of forcing it off. */
				BodyNodeMode = CAN_NODE_DEGRADED;
				BCM_SendBodyStatus();
			}
		}

		heartbeatElapsedMs++;
		if (heartbeatElapsedMs >= BCM_HEARTBEAT_PERIOD_MS)
		{
			heartbeatElapsedMs = 0U;
			BCM_SendHeartbeat();
		}

		Delay_ms(1);
	}
}
