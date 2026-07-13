#include "stm32f10x.h"                  // Device header
#include "MyCAN.h"
#include "BcmProtocol.h"
#include "Delay.h"

#define CAN_TX_TIMEOUT_MS 5U

static volatile uint32_t s_last_esr = 0U;
static volatile uint32_t s_tx_fail_count = 0U;
static volatile uint8_t s_last_tx_status = CAN_TxStatus_Ok;
static volatile uint8_t s_last_tec = 0U;
static volatile uint8_t s_last_rec = 0U;
static volatile uint8_t s_last_lec = 0U;

static uint16_t prvCanStdIdToFilterReg(uint16_t std_id)
{
	return (uint16_t)((std_id & 0x7FFU) << 5);
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
	if (CAN_MessagePending(CAN1, CAN_FIFO0) > 0)
	{
		return 1;
	}
	return 0;
}

void MyCAN_Receive(uint32_t *ID, uint8_t *Length, uint8_t *Data)
{
	CanRxMsg RxMessage;
	CAN_Receive(CAN1, CAN_FIFO0, &RxMessage);
	
	if (RxMessage.IDE == CAN_Id_Standard)
	{
		*ID = RxMessage.StdId;
	}
	else
	{
		*ID = RxMessage.ExtId;
	}
	
	if (RxMessage.RTR == CAN_RTR_Data)
	{
		*Length = RxMessage.DLC;
		for (uint8_t i = 0; i < *Length; i ++)
		{
			Data[i] = RxMessage.Data[i];
		}
	}
	else
	{
		*Length = 0;
	}
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
	diag->last_tx_status = s_last_tx_status;
}
