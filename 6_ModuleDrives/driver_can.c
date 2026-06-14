/**
 * @file driver_can.c
 * @brief F407 CAN1 driver for BCM gateway prototype
 * @note PB8=CAN1_RX, PB9=CAN1_TX, current bit timing = 250kbps
 */
#include "driver_can.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

static CAN_HandleTypeDef hcan1;
static CAN_TxHeaderTypeDef TxHeader;
static CAN_RxHeaderTypeDef RxHeader;
static uint32_t TxMailbox;

static uint16_t prvCanStdIdToFilterReg(uint16_t std_id)
{
	return (uint16_t)((std_id & 0x7FFU) << 5);
}

void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(hcan == NULL || hcan->Instance != CAN1)
	{
		return;
	}

	__HAL_RCC_CAN1_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* PB8=CAN1_RX, PB9=CAN1_TX, AF9 */
	GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_9;
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF9_CAN1;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

	/* Current loopback test uses polling in Driver_CAN_Recv(), so RX interrupts
	 * are intentionally left disabled to avoid mixing two receive paths. */
}

int Driver_CAN_Init(void)
{
	CAN_FilterTypeDef sFilterConfig = {0};

	hcan1.Instance = CAN1;
	HAL_CAN_DeInit(&hcan1);

#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
	hcan1.Init.Mode = CAN_MODE_LOOPBACK;
#else
	hcan1.Init.Mode = CAN_MODE_NORMAL;
#endif
	hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
	hcan1.Init.TimeSeg1 = CAN_BS1_6TQ;
	hcan1.Init.TimeSeg2 = CAN_BS2_1TQ;
	hcan1.Init.TimeTriggeredMode = DISABLE;
	hcan1.Init.AutoBusOff = ENABLE;
	hcan1.Init.AutoWakeUp = DISABLE;
	hcan1.Init.AutoRetransmission = ENABLE;
	hcan1.Init.ReceiveFifoLocked = DISABLE;
	hcan1.Init.TransmitFifoPriority = DISABLE;
	hcan1.Init.Prescaler = 21; /* 42MHz / 21 / 8TQ = 250kbps */

	if(HAL_CAN_Init(&hcan1) != HAL_OK)
	{
		printf("[CAN] HAL_CAN_Init failed\r\n");
		return -1;
	}

	sFilterConfig.FilterBank = 0;
	sFilterConfig.FilterMode = CAN_FILTERMODE_IDLIST;
	sFilterConfig.FilterScale = CAN_FILTERSCALE_16BIT;
	/* One 16-bit filter bank provides four exact-match standard-ID slots.
	 * NORMAL mode only needs slave status/heartbeat; LOOPBACK also accepts
	 * BODY_CMD so single-board CAN self-tests still work. */
#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
	sFilterConfig.FilterIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_CMD);
	sFilterConfig.FilterIdLow = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
	sFilterConfig.FilterMaskIdHigh = prvCanStdIdToFilterReg(CAN_ID_NODE_HEARTBEAT);
	sFilterConfig.FilterMaskIdLow = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
#else
	sFilterConfig.FilterIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
	sFilterConfig.FilterIdLow = prvCanStdIdToFilterReg(CAN_ID_NODE_HEARTBEAT);
	sFilterConfig.FilterMaskIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
	sFilterConfig.FilterMaskIdLow = prvCanStdIdToFilterReg(CAN_ID_NODE_HEARTBEAT);
#endif
	sFilterConfig.FilterFIFOAssignment = CAN_RX_FIFO0;
	sFilterConfig.FilterActivation = ENABLE;
	sFilterConfig.SlaveStartFilterBank = 14;

	if(HAL_CAN_ConfigFilter(&hcan1, &sFilterConfig) != HAL_OK)
	{
		printf("[CAN] ConfigFilter failed\r\n");
		return -1;
	}

	if(HAL_CAN_Start(&hcan1) != HAL_OK)
	{
		printf("[CAN] Start failed\r\n");
		return -1;
	}

#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
	printf("[CAN] Init OK (loopback 250kbps)\r\n");
#else
	printf("[CAN] Init OK (normal 250kbps)\r\n");
#endif
	return 0;
}

int Driver_CAN_Send(uint32_t id, uint8_t *data, uint8_t len)
{
	uint32_t start_tick = xTaskGetTickCount();

	if(len > 8) len = 8;

	(void)HAL_CAN_ResetError(&hcan1);

	TxHeader.StdId = id & 0x7FF;
	TxHeader.ExtId = 0;
	TxHeader.IDE = CAN_ID_STD;
	TxHeader.RTR = CAN_RTR_DATA;
	TxHeader.DLC = len;
	TxHeader.TransmitGlobalTime = DISABLE;

	if(HAL_CAN_AddTxMessage(&hcan1, &TxHeader, data, &TxMailbox) != HAL_OK)
		return -1;

	/* Wait until the mailbox really leaves the pending state. This gives the
	 * upper layer a real TX completion result instead of only trusting that
	 * HAL_CAN_AddTxMessage accepted the frame. */
	while(HAL_CAN_IsTxMessagePending(&hcan1, TxMailbox) != 0U)
	{
		if((xTaskGetTickCount() - start_tick) >= pdMS_TO_TICKS(5))
		{
			(void)HAL_CAN_AbortTxRequest(&hcan1, TxMailbox);
			return -1;
		}
		vTaskDelay(1);
	}

	return 0;
}

int Driver_CAN_Recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms)
{
	uint32_t start = xTaskGetTickCount();

	while((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms))
	{
		if(HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U)
		{
			vTaskDelay(1);
			continue;
		}

		if(HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &RxHeader, data) == HAL_OK)
		{
			*id = RxHeader.StdId;
			*len = RxHeader.DLC;
			return 0;
		}
		vTaskDelay(1);
	}

	return -1;
}

uint32_t Driver_CAN_GetError(void)
{
	return HAL_CAN_GetError(&hcan1);
}

uint32_t Driver_CAN_GetESR(void)
{
	return hcan1.Instance->ESR;
}
