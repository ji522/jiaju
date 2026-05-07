/**
 * @file driver_can.c
 * @brief F407 CAN1 驱动 —— Loopback 模式自收发测试
 * @note PB8=CAN1_RX, PB9=CAN1_TX, 当前位时序为 250kbps
 */
#include "driver_can.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

static CAN_HandleTypeDef hcan1;
static CAN_TxHeaderTypeDef TxHeader;
static CAN_RxHeaderTypeDef RxHeader;
static uint32_t TxMailbox;

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

	hcan1.Init.Mode = CAN_MODE_LOOPBACK;
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
	sFilterConfig.FilterMode = CAN_FILTERMODE_IDMASK;
	sFilterConfig.FilterScale = CAN_FILTERSCALE_32BIT;
	sFilterConfig.FilterIdHigh = 0x0000;
	sFilterConfig.FilterIdLow = 0x0000;
	sFilterConfig.FilterMaskIdHigh = 0x0000;
	sFilterConfig.FilterMaskIdLow = 0x0000;
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

	printf("[CAN] Init OK (loopback 250kbps)\r\n");
	return 0;
}

int Driver_CAN_Send(uint32_t id, uint8_t *data, uint8_t len)
{
	if(len > 8) len = 8;

	TxHeader.StdId = id & 0x7FF;
	TxHeader.ExtId = 0;
	TxHeader.IDE = CAN_ID_STD;
	TxHeader.RTR = CAN_RTR_DATA;
	TxHeader.DLC = len;
	TxHeader.TransmitGlobalTime = DISABLE;

	if(HAL_CAN_AddTxMessage(&hcan1, &TxHeader, data, &TxMailbox) != HAL_OK)
		return -1;

	return 0;
}

int Driver_CAN_Recv(uint32_t *id, uint8_t *data, uint8_t *len, uint32_t timeout_ms)
{
	uint32_t start = xTaskGetTickCount();

	while((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms))
	{
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
