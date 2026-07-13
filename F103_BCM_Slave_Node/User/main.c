#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "LED.h"
#include "MyCAN.h"
#include "BcmProtocol.h"

#define BCM_LINK_TIMEOUT_MS      15000U
#define BCM_HEARTBEAT_PERIOD_MS    500U
#define BCM_TX_FAIL_DEGRADED_COUNT    3U
#define BCM_TX_RECOVERY_COUNT         3U

uint8_t BodyOutputMask = 0;
uint8_t LastCmdSeq = 0;
uint8_t BodyNodeMode = CAN_NODE_INIT;

static uint8_t s_consecutiveTxFailures = 0U;
static uint8_t s_consecutiveTxSuccesses = 0U;
static uint8_t s_masterOnline = 0U;

static void BCM_RecordTxResult(int txResult)
{
	MyCanDiag diag;

	MyCAN_GetDiag(&diag);
	if (txResult == 0)
	{
		s_consecutiveTxFailures = 0U;
		if (s_consecutiveTxSuccesses < 0xFFU)
		{
			s_consecutiveTxSuccesses++;
		}

		if (s_masterOnline != 0U &&
			s_consecutiveTxSuccesses >= BCM_TX_RECOVERY_COUNT &&
			(diag.esr & CAN_ESR_BOFF) == 0U)
		{
			BodyNodeMode = CAN_NODE_NORMAL;
		}
		return;
	}

	s_consecutiveTxSuccesses = 0U;
	if (s_consecutiveTxFailures < 0xFFU)
	{
		s_consecutiveTxFailures++;
	}

	if ((diag.esr & CAN_ESR_BOFF) != 0U)
	{
		BodyNodeMode = CAN_NODE_FAULT;
	}
	else if (BodyNodeMode != CAN_NODE_FAULT &&
		s_consecutiveTxFailures >= BCM_TX_FAIL_DEGRADED_COUNT)
	{
		BodyNodeMode = CAN_NODE_DEGRADED;
	}
}

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
	int txResult = 0;

	TxData[CAN_BODY_STATUS_BYTE_MASK] = BodyOutputMask;
	TxData[CAN_BODY_STATUS_BYTE_MODE] = BodyNodeMode;
	TxData[CAN_BODY_STATUS_BYTE_SEQ] = LastCmdSeq;
	txResult = MyCAN_Transmit(CAN_ID_BODY_STATUS, 3, TxData);
	BCM_RecordTxResult(txResult);
}

static void BCM_SendHeartbeat(void)
{
	uint8_t TxData[3];
	int txResult = 0;

	TxData[CAN_HEARTBEAT_BYTE_MODE] = BodyNodeMode;
	TxData[CAN_HEARTBEAT_BYTE_MASK] = BodyOutputMask;
	TxData[CAN_HEARTBEAT_BYTE_SEQ] = LastCmdSeq;
	txResult = MyCAN_Transmit(CAN_ID_SLAVE_HEARTBEAT, 3, TxData);
	BCM_RecordTxResult(txResult);
}

int main(void)
{
	uint32_t RxID = 0;
	uint8_t RxLength = 0;
	uint8_t RxData[8] = {0};
	uint32_t lastLinkRxMs = 0U;
	uint32_t lastHeartbeatTxMs = 0U;
	uint32_t nowMs = 0U;

	Delay_Init();
	LED_Init();
	LED1_OFF();
	LED2_OFF();
	MyCAN_Init();

	while (1)
	{
		nowMs = Delay_GetTickMs();

		if (MyCAN_ReceiveFlag())
		{
			MyCAN_Receive(&RxID, &RxLength, RxData);

			if (RxID == CAN_ID_BODY_CMD &&
				RxLength == 2U &&
				(RxData[CAN_BODY_CMD_BYTE_MASK] &
					(uint8_t)~(CAN_BODY_CTRL_LAMP | CAN_BODY_CTRL_HAZARD | CAN_BODY_CTRL_FAN)) == 0U)
			{
				LastCmdSeq = RxData[CAN_BODY_CMD_BYTE_SEQ];
				s_masterOnline = 1U;
				lastLinkRxMs = nowMs;
				BCM_ApplyOutputs(RxData[CAN_BODY_CMD_BYTE_MASK]);
				BCM_SendBodyStatus();
			}
			else if (RxID == CAN_ID_GATEWAY_HEARTBEAT &&
				RxLength == 5U &&
				RxData[CAN_HEARTBEAT_BYTE_MODE] <= CAN_NODE_FAULT &&
				(RxData[CAN_HEARTBEAT_BYTE_MASK] &
					(uint8_t)~(CAN_BODY_CTRL_LAMP | CAN_BODY_CTRL_HAZARD | CAN_BODY_CTRL_FAN)) == 0U)
			{
				/* Treat a valid master heartbeat as link-alive traffic so
				 * event-driven actuator outputs are not misclassified as faults. */
				s_masterOnline = 1U;
				lastLinkRxMs = nowMs;
			}
		}

		if ((uint32_t)(nowMs - lastLinkRxMs) >= BCM_LINK_TIMEOUT_MS)
		{
			s_masterOnline = 0U;
			if (BodyNodeMode != CAN_NODE_DEGRADED && BodyNodeMode != CAN_NODE_FAULT)
			{
				/* Keep the last valid actuator output while the link is degraded. */
				BodyNodeMode = CAN_NODE_DEGRADED;
				BCM_SendBodyStatus();
			}
		}

		if ((uint32_t)(nowMs - lastHeartbeatTxMs) >= BCM_HEARTBEAT_PERIOD_MS)
		{
			lastHeartbeatTxMs = nowMs;
			BCM_SendHeartbeat();
		}
	}
}
