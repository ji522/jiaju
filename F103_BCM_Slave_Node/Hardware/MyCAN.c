#include "stm32f10x.h"                  // Device header
#include "misc.h"
#include "MyCAN.h"
#include "BcmProtocol.h"
#include "Delay.h"

#define CAN_TX_TIMEOUT_MS 5U
#define CAN_RX_QUEUE_LENGTH 8U

typedef struct
{
	uint32_t id;
	uint8_t len;
	uint8_t data[8];
} MyCanRxFrame;

static volatile uint32_t s_last_esr = 0U;
static volatile uint32_t s_tx_fail_count = 0U;
static volatile uint32_t s_rx_irq_count = 0U;
static volatile uint32_t s_rx_drop_count = 0U;
static volatile uint8_t s_last_tx_status = CAN_TxStatus_Ok;
static volatile uint8_t s_last_tec = 0U;
static volatile uint8_t s_last_rec = 0U;
static volatile uint8_t s_last_lec = 0U;
static volatile uint8_t s_rx_head = 0U;
static volatile uint8_t s_rx_tail = 0U;
static volatile MyCanRxFrame s_rx_queue[CAN_RX_QUEUE_LENGTH];

static uint16_t prvCanStdIdToFilterReg(uint16_t std_id)
{
	return (uint16_t)((std_id & 0x7FFU) << 5);
}

static uint8_t prvRxNextIndex(uint8_t index)
{
	index++;
	return (index >= CAN_RX_QUEUE_LENGTH) ? 0U : index;
}

static void prvQueueRxMessage(const CanRxMsg *rxMessage)
{
	uint8_t next_head = 0U;
	uint8_t len = 0U;
	uint8_t head = s_rx_head;

	if (rxMessage == 0)
	{
		return;
	}

	next_head = prvRxNextIndex(head);
	if (next_head == s_rx_tail)
	{
		s_rx_drop_count++;
		return;
	}

	s_rx_queue[head].id = (rxMessage->IDE == CAN_Id_Standard) ?
		rxMessage->StdId : rxMessage->ExtId;
	s_rx_queue[head].len = (rxMessage->RTR == CAN_RTR_Data) ?
		rxMessage->DLC : 0U;
	len = s_rx_queue[head].len;
	for (uint8_t i = 0; i < len; i++)
	{
		s_rx_queue[head].data[i] = rxMessage->Data[i];
	}
	for (uint8_t i = len; i < 8U; i++)
	{
		s_rx_queue[head].data[i] = 0U;
	}

	s_rx_head = next_head;
	s_rx_irq_count++;
}

static void prvSnapshotCanDiag(uint8_t tx_status)
{
	s_last_esr = CAN1->ESR;
	s_last_tec = CAN_GetLSBTransmitErrorCounter(CAN1);
	s_last_rec = CAN_GetReceiveErrorCounter(CAN1);
	s_last_lec = (uint8_t)((CAN1->ESR >> 4) & 0x07U);
	s_last_tx_status = tx_status;

	if(tx_status != CAN_TxStatus_Ok)
	{
		s_tx_fail_count++;
	}
}

void MyCAN_Init(void)
{
	NVIC_InitTypeDef NVIC_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);
	
	GPIO_InitTypeDef GPIO_InitStructure;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
	
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_11;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(GPIOA, &GPIO_InitStructure);
	
	CAN_InitTypeDef CAN_InitStructure;
	CAN_InitStructure.CAN_Mode = CAN_Mode_Normal;
	CAN_InitStructure.CAN_Prescaler = 18;		// 36M / 18 / (1 + 5 + 2) = 250K
	CAN_InitStructure.CAN_BS1 = CAN_BS1_5tq;
	CAN_InitStructure.CAN_BS2 = CAN_BS2_2tq;
	CAN_InitStructure.CAN_SJW = CAN_SJW_1tq;
	CAN_InitStructure.CAN_NART = DISABLE;
	CAN_InitStructure.CAN_TXFP = DISABLE;
	CAN_InitStructure.CAN_RFLM = DISABLE;
	CAN_InitStructure.CAN_AWUM = DISABLE;
	CAN_InitStructure.CAN_TTCM = DISABLE;
	CAN_InitStructure.CAN_ABOM = ENABLE;
	CAN_Init(CAN1, &CAN_InitStructure);
	
	CAN_FilterInitTypeDef CAN_FilterInitStructure;
	CAN_FilterInitStructure.CAN_FilterNumber = 0;
	CAN_FilterInitStructure.CAN_FilterIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_CMD);
	CAN_FilterInitStructure.CAN_FilterIdLow = prvCanStdIdToFilterReg(CAN_ID_GATEWAY_HEARTBEAT);
	CAN_FilterInitStructure.CAN_FilterMaskIdHigh = prvCanStdIdToFilterReg(CAN_ID_BODY_CMD);
	CAN_FilterInitStructure.CAN_FilterMaskIdLow = prvCanStdIdToFilterReg(CAN_ID_GATEWAY_HEARTBEAT);
	CAN_FilterInitStructure.CAN_FilterScale = CAN_FilterScale_16bit;
	CAN_FilterInitStructure.CAN_FilterMode = CAN_FilterMode_IdList;
	CAN_FilterInitStructure.CAN_FilterFIFOAssignment = CAN_Filter_FIFO0;
	CAN_FilterInitStructure.CAN_FilterActivation = ENABLE;
	CAN_FilterInit(&CAN_FilterInitStructure);

	CAN_ITConfig(CAN1, CAN_IT_FMP0, ENABLE);

#ifdef STM32F10X_CL
	NVIC_InitStructure.NVIC_IRQChannel = CAN1_RX0_IRQn;
#else
	NVIC_InitStructure.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
#endif
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);
}

int MyCAN_Transmit(uint32_t ID, uint8_t Length, uint8_t *Data)
{
	CanTxMsg TxMessage;
	uint8_t tx_status = CAN_TxStatus_Failed;
	uint32_t start_tick = 0U;

	if(Data == 0 || Length > 8U || ID > 0x7FFU)
	{
		prvSnapshotCanDiag(CAN_TxStatus_Failed);
		return -1;
	}

	TxMessage.StdId = ID;
	TxMessage.ExtId = ID;
	TxMessage.IDE = CAN_Id_Standard;
	TxMessage.RTR = CAN_RTR_Data;
	TxMessage.DLC = Length;
	for (uint8_t i = 0; i < Length; i ++)
	{
		TxMessage.Data[i] = Data[i];
	}
	
	uint8_t TransmitMailbox = CAN_Transmit(CAN1, &TxMessage);
	if (TransmitMailbox == CAN_TxStatus_NoMailBox)
	{
		prvSnapshotCanDiag(CAN_TxStatus_NoMailBox);
		return -1;
	}
	
	start_tick = Delay_GetTickMs();
	while ((tx_status = CAN_TransmitStatus(CAN1, TransmitMailbox)) == CAN_TxStatus_Pending)
	{
		if ((uint32_t)(Delay_GetTickMs() - start_tick) >= CAN_TX_TIMEOUT_MS)
		{
			CAN_CancelTransmit(CAN1, TransmitMailbox);
			prvSnapshotCanDiag(CAN_TxStatus_Pending);
			return -1;
		}
	}

	prvSnapshotCanDiag(tx_status);
	return (tx_status == CAN_TxStatus_Ok) ? 0 : -1;
}

uint8_t MyCAN_ReceiveFlag(void)
{
	if (s_rx_head != s_rx_tail)
	{
		return 1;
	}
	return 0;
}

void MyCAN_Receive(uint32_t *ID, uint8_t *Length, uint8_t *Data)
{
	uint8_t tail = s_rx_tail;
	uint8_t len = 0U;

	if (ID == 0 || Length == 0 || Data == 0)
	{
		return;
	}

	if (tail == s_rx_head)
	{
		*Length = 0U;
		return;
	}

	*ID = s_rx_queue[tail].id;
	len = s_rx_queue[tail].len;
	*Length = len;
	for (uint8_t i = 0; i < len; i++)
	{
		Data[i] = s_rx_queue[tail].data[i];
	}
	for (uint8_t i = len; i < 8U; i++)
	{
		Data[i] = 0U;
	}

	s_rx_tail = prvRxNextIndex(tail);
}

void MyCAN_IRQHandler(void)
{
	CanRxMsg RxMessage;

	if (CAN_GetITStatus(CAN1, CAN_IT_FMP0) == RESET)
	{
		return;
	}

	while (CAN_MessagePending(CAN1, CAN_FIFO0) > 0U)
	{
		CAN_Receive(CAN1, CAN_FIFO0, &RxMessage);
		prvQueueRxMessage(&RxMessage);
	}

	CAN_ClearITPendingBit(CAN1, CAN_IT_FMP0);
}

void MyCAN_GetDiag(MyCanDiag *diag)
{
	if (diag == 0)
	{
		return;
	}

	diag->esr = s_last_esr;
	diag->tec = s_last_tec;
	diag->rec = s_last_rec;
	diag->lec = s_last_lec;
	diag->tx_fail_count = s_tx_fail_count;
	diag->rx_irq_count = s_rx_irq_count;
	diag->rx_drop_count = s_rx_drop_count;
	diag->last_tx_status = s_last_tx_status;
}
