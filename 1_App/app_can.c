/**
 * @file app_can.c
 * @brief CAN gateway task: body-control command TX + status/heartbeat RX/TX
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>

#include "driver_can.h"
#include "driver_led_key.h"

#define CAN_TX_QUEUE_LENGTH 8
#define CAN_RX_QUEUE_LENGTH 8
#define CAN_TASK_PERIOD_MS 20U
#define CAN_HEARTBEAT_PERIOD_MS 5000U
#define CAN_SLAVE_TIMEOUT_MS 1500U
#define CAN_RX_POLL_TIMEOUT_MS 1U

QueueHandle_t xCanTxQueue = NULL;
QueueHandle_t xCanRxQueue = NULL;

volatile uint32_t g_can_tx_count = 0;
volatile uint32_t g_can_tx_fail_count = 0;
volatile uint32_t g_can_last_tx_fail_id = 0;
volatile uint32_t g_can_last_error = 0;
volatile uint32_t g_can_last_esr = 0;
volatile uint32_t g_can_rx_count = 0;
volatile uint32_t g_can_last_rx_id = 0;
volatile uint8_t g_can_last_cmd_seq = 0;
volatile uint8_t g_can_last_status_seq = 0;
volatile uint8_t g_can_pending_cmd_seq = 0;
volatile uint8_t g_can_pending_cmd_active = 0;
volatile uint8_t g_can_seq_consistent = 1;
volatile uint32_t g_can_seq_match_count = 0;
volatile uint32_t g_can_seq_mismatch_count = 0;
volatile uint8_t g_can_last_seq_expected = 0;
volatile uint8_t g_can_last_seq_observed = 0;
volatile uint8_t g_can_node_mode = CAN_NODE_INIT;
volatile uint8_t g_can_body_status = 0;
volatile uint8_t g_can_status_dirty = 0;
volatile uint8_t g_can_slave_online = 0;
volatile uint32_t g_can_slave_timeout_count = 0;
volatile uint32_t g_can_last_slave_rx_age_ms = 0;
volatile uint32_t g_can_dtc_mask = 0;
volatile uint8_t g_can_last_dtc = CAN_DTC_NONE;

static TickType_t s_last_slave_rx_tick = 0;
static uint8_t s_slave_timeout_latched = 0U;
static uint8_t s_seq_mismatch_latched = 0U;

void CAN_ClearDiagnostics(void)
{
	g_can_tx_fail_count = 0U;
	g_can_last_tx_fail_id = 0U;
	g_can_last_error = 0U;
	g_can_last_esr = 0U;
	g_can_seq_consistent = 1U;
	g_can_seq_match_count = 0U;
	g_can_seq_mismatch_count = 0U;
	g_can_last_seq_expected = 0U;
	g_can_last_seq_observed = 0U;
	g_can_pending_cmd_seq = 0U;
	g_can_pending_cmd_active = 0U;
	g_can_dtc_mask = 0U;
	g_can_last_dtc = CAN_DTC_NONE;
	g_can_slave_timeout_count = 0U;
	s_slave_timeout_latched = 0U;
	s_seq_mismatch_latched = 0U;
	g_can_status_dirty = 1U;
	printf("[CAN] Diagnostics cleared\r\n");
}

static void prvRaiseDtc(uint32_t dtc_mask, uint8_t dtc_code)
{
	if((g_can_dtc_mask & dtc_mask) == 0U)
	{
		g_can_dtc_mask |= dtc_mask;
		g_can_status_dirty = 1U;
	}
	g_can_last_dtc = dtc_code;
}

static void prvApplyBodyStatus(uint8_t body_status)
{
	/* Reuse the board LED as a simple actuator indicator for the lamp bit. */
	if((body_status & CAN_BODY_CTRL_LAMP) != 0U)
	{
		LED(1);
	}
	else
	{
		LED(0);
	}
}

static void prvRecordCanTxFailure(uint32_t frame_id)
{
	g_can_tx_fail_count++;
	g_can_last_tx_fail_id = frame_id & 0x7FFU;
	g_can_last_error = Driver_CAN_GetError();
	g_can_last_esr = Driver_CAN_GetESR();
	g_can_node_mode = CAN_NODE_DEGRADED;
	prvRaiseDtc(CAN_DTC_MASK_TX_FAIL, CAN_DTC_TX_FAIL);

	printf("[CAN] TX failed: id=0x%03lX err=0x%08lX esr=0x%08lX fails=%lu\r\n",
		(unsigned long)g_can_last_tx_fail_id,
		(unsigned long)g_can_last_error,
		(unsigned long)g_can_last_esr,
		(unsigned long)g_can_tx_fail_count);
}

static void prvNoteSlaveActivity(uint32_t frame_id)
{
	s_last_slave_rx_tick = xTaskGetTickCount();
	g_can_last_slave_rx_age_ms = 0U;
	s_slave_timeout_latched = 0U;

	if(g_can_slave_online == 0U)
	{
		g_can_slave_online = 1U;
		g_can_status_dirty = 1U;
		printf("[CAN] Slave online: id=0x%03lX\r\n", (unsigned long)(frame_id & 0x7FFU));
	}
}

static void prvCheckSlaveTimeout(void)
{
	TickType_t now = xTaskGetTickCount();
	TickType_t elapsed_ticks = now - s_last_slave_rx_tick;

	g_can_last_slave_rx_age_ms = (uint32_t)elapsed_ticks * (uint32_t)portTICK_PERIOD_MS;

	if(s_slave_timeout_latched == 0U &&
		elapsed_ticks >= pdMS_TO_TICKS(CAN_SLAVE_TIMEOUT_MS))
	{
		s_slave_timeout_latched = 1U;
		g_can_slave_online = 0U;
		g_can_slave_timeout_count++;
		g_can_node_mode = CAN_NODE_DEGRADED;
		g_can_seq_consistent = 0U;
		prvRaiseDtc(CAN_DTC_MASK_NODE_TIMEOUT, CAN_DTC_NODE_TIMEOUT);
		if(g_can_pending_cmd_active != 0U)
		{
			g_can_seq_mismatch_count++;
			prvRaiseDtc(CAN_DTC_MASK_SEQ_TIMEOUT, CAN_DTC_SEQ_TIMEOUT);
			printf("[CAN] Seq timeout: expected=%u\r\n",
				(unsigned)g_can_pending_cmd_seq);
		}
		g_can_pending_cmd_active = 0U;
		s_seq_mismatch_latched = 0U;
		g_can_status_dirty = 1U;
		printf("[CAN] Slave timeout: no BODY_STATUS/HEARTBEAT for %lu ms\r\n",
			(unsigned long)g_can_last_slave_rx_age_ms);
	}
}

static void prvTrackSeqObservation(uint8_t observed_seq)
{
	g_can_last_seq_observed = observed_seq;

	if(g_can_pending_cmd_active == 0U)
	{
		return;
	}

	if(observed_seq == g_can_pending_cmd_seq)
	{
		g_can_seq_match_count++;
		g_can_seq_consistent = 1U;
		g_can_pending_cmd_active = 0U;
		s_seq_mismatch_latched = 0U;
		g_can_status_dirty = 1U;
	}
	else if(s_seq_mismatch_latched == 0U)
	{
		g_can_seq_mismatch_count++;
		g_can_seq_consistent = 0U;
		g_can_last_seq_expected = g_can_pending_cmd_seq;
		s_seq_mismatch_latched = 1U;
		prvRaiseDtc(CAN_DTC_MASK_SEQ_MISMATCH, CAN_DTC_SEQ_MISMATCH);
		g_can_status_dirty = 1U;
		printf("[CAN] Seq mismatch: expected=%u got=%u\r\n",
			(unsigned)g_can_pending_cmd_seq,
			(unsigned)observed_seq);
	}
}

static void prvBuildHeartbeatFrame(CanFrame *frame)
{
	if(frame == NULL)
	{
		return;
	}

	frame->id = CAN_ID_NODE_HEARTBEAT;
	frame->dlc = 5;
	frame->data[0] = g_can_node_mode;
	frame->data[1] = g_can_body_status;
	frame->data[2] = (uint8_t)(g_can_tx_count & 0xFFU);
	frame->data[3] = (uint8_t)(g_can_rx_count & 0xFFU);
	frame->data[4] = g_can_last_status_seq;
	memset(&frame->data[5], 0, 3);
}

static void prvHandleReceivedFrame(const CanFrame *frame)
{
	if(frame == NULL)
	{
		return;
	}

	g_can_last_rx_id = frame->id;
	g_can_rx_count++;

	if(frame->id == CAN_ID_BODY_CMD && frame->dlc > 0U)
	{
#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
		g_can_body_status = frame->data[CAN_BODY_CMD_BYTE_MASK];
		g_can_last_cmd_seq = (frame->dlc > CAN_BODY_CMD_BYTE_SEQ) ?
			frame->data[CAN_BODY_CMD_BYTE_SEQ] : 0U;
		g_can_last_status_seq = g_can_last_cmd_seq;
		g_can_node_mode = CAN_NODE_NORMAL;
		g_can_status_dirty = 1U;
		prvApplyBodyStatus(g_can_body_status);
#endif
	}
	else if(frame->id == CAN_ID_BODY_STATUS && frame->dlc >= 2U)
	{
		uint8_t prev_mode = g_can_node_mode;
		uint8_t prev_body = g_can_body_status;
		uint8_t prev_seq = g_can_last_status_seq;

		prvNoteSlaveActivity(frame->id);
		g_can_body_status = frame->data[CAN_BODY_STATUS_BYTE_MASK];
		g_can_node_mode = frame->data[CAN_BODY_STATUS_BYTE_MODE];
		g_can_last_status_seq = (frame->dlc > CAN_BODY_STATUS_BYTE_SEQ) ?
			frame->data[CAN_BODY_STATUS_BYTE_SEQ] : 0U;
		prvTrackSeqObservation(g_can_last_status_seq);
		if(prev_mode != g_can_node_mode ||
			prev_body != g_can_body_status ||
			prev_seq != g_can_last_status_seq)
		{
			g_can_status_dirty = 1U;
		}
		prvApplyBodyStatus(g_can_body_status);
	}
	else if(frame->id == CAN_ID_NODE_HEARTBEAT)
	{
		uint8_t prev_mode = g_can_node_mode;
		uint8_t prev_body = g_can_body_status;
		uint8_t prev_seq = g_can_last_status_seq;

		prvNoteSlaveActivity(frame->id);
		if(frame->dlc > CAN_HEARTBEAT_BYTE_MODE)
		{
			g_can_node_mode = frame->data[CAN_HEARTBEAT_BYTE_MODE];
		}
		if(frame->dlc > CAN_HEARTBEAT_BYTE_MASK)
		{
			g_can_body_status = frame->data[CAN_HEARTBEAT_BYTE_MASK];
		}
		if(frame->dlc > CAN_HEARTBEAT_BYTE_SEQ)
		{
			g_can_last_status_seq = frame->data[CAN_HEARTBEAT_BYTE_SEQ];
			g_can_last_seq_observed = g_can_last_status_seq;
			if(g_can_pending_cmd_active != 0U &&
				g_can_last_status_seq == g_can_pending_cmd_seq)
			{
				prvTrackSeqObservation(g_can_last_status_seq);
			}
		}
		if(prev_mode != g_can_node_mode ||
			prev_body != g_can_body_status ||
			prev_seq != g_can_last_status_seq)
		{
			g_can_status_dirty = 1U;
		}
	}
}

void CanTask(void *parameter)
{
	CanFrame tx_frame = {0};
	CanFrame rx_frame = {0};
	uint32_t rx_id = 0;
	uint8_t rx_data[8] = {0};
	uint8_t rx_len = 0;
	TickType_t xLastHeartbeatTick = xTaskGetTickCount();

	(void)parameter;

	printf("[CAN] Task started\r\n");

	while(Driver_CAN_Init() != 0)
	{
		g_can_node_mode = CAN_NODE_FAULT;
		printf("[CAN] Init retry in 5s\r\n");
		vTaskDelay(pdMS_TO_TICKS(5000));
	}

	Driver_LED_Init();
	LED(0);

	g_can_node_mode = CAN_NODE_NORMAL;
	s_last_slave_rx_tick = xTaskGetTickCount();
	s_slave_timeout_latched = 0U;
	g_can_slave_online = 0U;
	g_can_slave_timeout_count = 0U;
	g_can_last_slave_rx_age_ms = 0U;
	g_can_dtc_mask = 0U;
	g_can_last_dtc = CAN_DTC_NONE;
	printf("[CAN] Gateway node init OK\r\n");

	while(1)
	{
		/* 1. Send queued body-control commands generated from MQTT. */
		while(xCanTxQueue != NULL &&
			xQueueReceive(xCanTxQueue, &tx_frame, 0) == pdPASS)
		{
			if(tx_frame.id == CAN_ID_BODY_CMD && tx_frame.dlc > CAN_BODY_CMD_BYTE_SEQ)
			{
				g_can_last_cmd_seq = tx_frame.data[CAN_BODY_CMD_BYTE_SEQ];
			}
			printf("[CAN] TX queue pop: id=0x%03lX dlc=%u data0=0x%02X seq=%u\r\n",
				(unsigned long)tx_frame.id,
				(unsigned)tx_frame.dlc,
				(unsigned)tx_frame.data[CAN_BODY_CMD_BYTE_MASK],
				(tx_frame.dlc > CAN_BODY_CMD_BYTE_SEQ) ?
					(unsigned)tx_frame.data[CAN_BODY_CMD_BYTE_SEQ] : 0U);
			if(Driver_CAN_Send(tx_frame.id, tx_frame.data, tx_frame.dlc) == 0)
			{
				g_can_tx_count++;
				if(tx_frame.id == CAN_ID_BODY_CMD && tx_frame.dlc > CAN_BODY_CMD_BYTE_SEQ)
				{
					g_can_pending_cmd_seq = tx_frame.data[CAN_BODY_CMD_BYTE_SEQ];
					g_can_pending_cmd_active = 1U;
					g_can_last_seq_expected = g_can_pending_cmd_seq;
					g_can_seq_consistent = 1U;
					s_seq_mismatch_latched = 0U;
					g_can_status_dirty = 1U;
				}
				printf("[CAN] TX sent: id=0x%03lX count=%lu\r\n",
					(unsigned long)tx_frame.id,
					(unsigned long)g_can_tx_count);
			}
			else
			{
				prvRecordCanTxFailure(tx_frame.id);
			}
		}

		/* 2. Send heartbeat on a slower cadence without delaying control traffic. */
		if((xTaskGetTickCount() - xLastHeartbeatTick) >= pdMS_TO_TICKS(CAN_HEARTBEAT_PERIOD_MS))
		{
			xLastHeartbeatTick = xTaskGetTickCount();
			prvBuildHeartbeatFrame(&tx_frame);
			if(Driver_CAN_Send(tx_frame.id, tx_frame.data, tx_frame.dlc) == 0)
			{
				g_can_tx_count++;
			}
			else
			{
				prvRecordCanTxFailure(tx_frame.id);
			}
		}

		/* 3. Drain loopback or bus RX frames, then mirror them to the RX queue for MQTT uplink. */
		while(Driver_CAN_Recv(&rx_id, rx_data, &rx_len, CAN_RX_POLL_TIMEOUT_MS) == 0)
		{
			memset(&rx_frame, 0, sizeof(rx_frame));
			rx_frame.id = rx_id;
			rx_frame.dlc = rx_len;
			memcpy(rx_frame.data, rx_data, rx_len);

			if(rx_frame.id != CAN_ID_NODE_HEARTBEAT)
			{
				printf("[CAN] RX frame: id=0x%03lX dlc=%u data0=0x%02X seq=%u\r\n",
					(unsigned long)rx_frame.id,
					(unsigned)rx_frame.dlc,
					(unsigned)rx_frame.data[CAN_BODY_STATUS_BYTE_MASK],
					(rx_frame.dlc > CAN_BODY_STATUS_BYTE_SEQ) ?
						(unsigned)rx_frame.data[CAN_BODY_STATUS_BYTE_SEQ] : 0U);
			}

			prvHandleReceivedFrame(&rx_frame);

			if(xCanRxQueue != NULL)
			{
				(void)xQueueSendToBack(xCanRxQueue, &rx_frame, 0);
				if(rx_frame.id != CAN_ID_NODE_HEARTBEAT)
				{
					printf("[CAN] RX queued for MQTT: id=0x%03lX\r\n",
						(unsigned long)rx_frame.id);
				}
			}

			if(rx_frame.id == CAN_ID_BODY_CMD)
			{
#if CAN_LINK_MODE == CAN_LINK_MODE_LOOPBACK
				CanFrame status_frame = {0};
				status_frame.id = CAN_ID_BODY_STATUS;
				status_frame.dlc = 3;
				status_frame.data[CAN_BODY_STATUS_BYTE_MASK] = g_can_body_status;
				status_frame.data[CAN_BODY_STATUS_BYTE_MODE] = g_can_node_mode;
				status_frame.data[CAN_BODY_STATUS_BYTE_SEQ] = g_can_last_status_seq;

				if(Driver_CAN_Send(status_frame.id, status_frame.data, status_frame.dlc) == 0)
				{
					g_can_tx_count++;
					printf("[CAN] BODY_STATUS sent: data0=0x%02X mode=%u\r\n",
						(unsigned)status_frame.data[0],
						(unsigned)status_frame.data[1]);
				}
				else
				{
					prvRecordCanTxFailure(status_frame.id);
				}
#endif
			}
		}

		prvCheckSlaveTimeout();

		vTaskDelay(pdMS_TO_TICKS(CAN_TASK_PERIOD_MS));
	}
}

void vStartCANTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	xCanTxQueue = xQueueCreate(CAN_TX_QUEUE_LENGTH, sizeof(CanFrame));
	xCanRxQueue = xQueueCreate(CAN_RX_QUEUE_LENGTH, sizeof(CanFrame));

	if(xCanTxQueue == NULL || xCanRxQueue == NULL)
	{
		printf("Create CAN queues failed.\r\n");
		return;
	}

	if(xTaskCreate(CanTask, "CAN",
		usTaskStackSize < 384 ? 384 : usTaskStackSize,
		NULL, uxTaskPriority, NULL) == pdPASS)
	{
		printf("Create CAN Task success.\r\n");
	}
	else
	{
		printf("Create CAN Task failed.\r\n");
	}
}
