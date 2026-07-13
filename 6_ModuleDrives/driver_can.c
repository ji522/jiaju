/**
 * @file driver_can.c
 * @brief F407 CAN1 driver for BCM gateway prototype
 * @note PB8=CAN1_RX, PB9=CAN1_TX, current bit timing = 250kbps
 */
#include "driver_can.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>

#define CAN_RX_ISR_QUEUE_LENGTH 16U

static CAN_HandleTypeDef hcan1;
static CAN_TxHeaderTypeDef TxHeader;
static uint32_t TxMailbox;
static volatile uint32_t s_can_error_snapshot = 0U;
static volatile uint32_t s_can_esr_snapshot = 0U;
static volatile uint8_t s_can_error_pending = 0U;
static volatile uint32_t s_can_rx_drop_count = 0U;

typedef struct
{
	uint32_t id;
	uint8_t len;
	uint8_t data[8];
} DriverCanRxFrame;

static QueueHandle_t s_can_rx_isr_queue = NULL;

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

	HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
	HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
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
	sFilterConfig.FilterMaskIdHigh = prvCanStdIdToFilterReg(CAN_ID_GATEWAY_HEARTBEAT);
	sFilterConfig.FilterMaskIdLow = prvCanStdIdToFilterReg(CAN_ID_SLAVE_HEARTBEAT);
#else
	sFilterConfig.FilterIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
	sFilterConfig.FilterIdLow = prvCanStdIdToFilterReg(CAN_ID_SLAVE_HEARTBEAT);
	sFilterConfig.FilterMaskIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_STATUS);
	sFilterConfig.FilterMaskIdLow = prvCanStdIdToFilterReg(CAN_ID_SLAVE_HEARTBEAT);
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

	if(HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING) != HAL_OK)
	{
		printf("[CAN] ActivateNotification failed\r\n");
		return -1;
	}

	if(HAL_CAN_ActivateNotification(&hcan1,
		CAN_IT_ERROR_WARNING |
		CAN_IT_ERROR_PASSIVE |
		CAN_IT_BUSOFF |
		CAN_IT_LAST_ERROR_CODE) != HAL_OK)
	{
		printf("[CAN] ActivateErrorNotification failed\r\n");
		return -1;
	}

	if(s_can_rx_isr_queue != NULL)
	{
		vQueueDelete(s_can_rx_isr_queue);
		s_can_rx_isr_queue = NULL;
	}
	s_can_rx_isr_queue = xQueueCreate(CAN_RX_ISR_QUEUE_LENGTH, sizeof(DriverCanRxFrame));
	if(s_can_rx_isr_queue == NULL)
	{
		printf("[CAN] RX ISR queue create failed\r\n");
		return -1;
	}

#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
	printf("[CAN] Init OK (loopback 250kbps)\r\n");
#else
	printf("[CAN] Init OK (normal 250kbps)\r\n");
#endif
	return 0;
}

void Driver_CAN_IRQHandler(void)
{
	HAL_CAN_IRQHandler(&hcan1);
}

uint8_t Driver_CAN_TakeErrorSnapshot(uint32_t *error, uint32_t *esr)
{
	if(error == NULL || esr == NULL || s_can_error_pending == 0U)
	{
		return 0U;
	}

	*error = s_can_error_snapshot;
	*esr = s_can_esr_snapshot;
	s_can_error_pending = 0U;
	return 1U;
}

uint32_t Driver_CAN_TakeRxDropCount(void)
{
	uint32_t count = 0U;

	taskENTER_CRITICAL();
	count = s_can_rx_drop_count;
	s_can_rx_drop_count = 0U;
	taskEXIT_CRITICAL();

	return count;
}

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
	CAN_RxHeaderTypeDef rx_header = {0};
	uint8_t rx_data[8] = {0};
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if(hcan == NULL || hcan->Instance != CAN1)
	{
		return;
	}

	while(HAL_CAN_GetRxFifoFillLevel(hcan, CAN_RX_FIFO0) > 0U)
	{
		DriverCanRxFrame frame = {0};

		if(HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rx_header, rx_data) != HAL_OK)
		{
			break;
		}

		frame.id = (rx_header.IDE == CAN_ID_STD) ? rx_header.StdId : rx_header.ExtId;
		frame.len = (rx_header.RTR == CAN_RTR_DATA) ? rx_header.DLC : 0U;
		for(uint8_t i = 0; i < frame.len; ++i)
		{
			frame.data[i] = rx_data[i];
		}
		for(uint8_t i = frame.len; i < 8U; ++i)
		{
			frame.data[i] = 0U;
		}

		if(s_can_rx_isr_queue != NULL)
		{
			if(xQueueSendFromISR(s_can_rx_isr_queue, &frame, &xHigherPriorityTaskWoken) != pdPASS)
			{
				s_can_rx_drop_count++;
			}
		}
	}

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan)
{
	if(hcan == NULL || hcan->Instance != CAN1)
	{
		return;
	}

	s_can_error_snapshot = HAL_CAN_GetError(hcan);
	s_can_esr_snapshot = hcan->Instance->ESR;
	s_can_error_pending = 1U;
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
	DriverCanRxFrame frame = {0};

	if(id == NULL || data == NULL || len == NULL || s_can_rx_isr_queue == NULL)
	{
		return -1;
	}

	if(xQueueReceive(s_can_rx_isr_queue, &frame, pdMS_TO_TICKS(timeout_ms)) != pdPASS)
	{
		return -1;
	}

	*id = frame.id;
	*len = frame.len;
	for(uint8_t i = 0; i < frame.len; ++i)
	{
		data[i] = frame.data[i];
	}
	for(uint8_t i = frame.len; i < 8U; ++i)
	{
		data[i] = 0U;
	}

	return 0;
}

uint32_t Driver_CAN_GetError(void)
{
	return HAL_CAN_GetError(&hcan1);
}

uint32_t Driver_CAN_GetESR(void)
{
	return hcan1.Instance->ESR;
}
