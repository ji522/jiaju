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

QueueHandle_t xCanTxQueue = NULL;
QueueHandle_t xCanRxQueue = NULL;

volatile uint32_t g_can_tx_count = 0;
volatile uint32_t g_can_rx_count = 0;
volatile uint32_t g_can_last_rx_id = 0;
volatile uint8_t g_can_node_mode = CAN_NODE_INIT;
volatile uint8_t g_can_body_status = 0;
volatile uint8_t g_can_status_dirty = 0;

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

static void prvBuildHeartbeatFrame(CanFrame *frame)
{
	if(frame == NULL)
	{
		return;
	}

	frame->id = CAN_ID_NODE_HEARTBEAT;
	frame->dlc = 4;
	frame->data[0] = g_can_node_mode;
	frame->data[1] = g_can_body_status;
	frame->data[2] = (uint8_t)(g_can_tx_count & 0xFFU);
	frame->data[3] = (uint8_t)(g_can_rx_count & 0xFFU);
	memset(&frame->data[4], 0, 4);
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
		g_can_body_status = frame->data[0];
		g_can_node_mode = CAN_NODE_NORMAL;
		g_can_status_dirty = 1U;
		prvApplyBodyStatus(g_can_body_status);
	}
	else if(frame->id == CAN_ID_NODE_HEARTBEAT)
	{
		g_can_node_mode = CAN_NODE_NORMAL;
	}
}

void CanTask(void *parameter)
{
	CanFrame tx_frame = {0};
	CanFrame rx_frame = {0};
	uint32_t rx_id = 0;
	uint8_t rx_data[8] = {0};
	uint8_t rx_len = 0;
	TickType_t xLastWakeTime = xTaskGetTickCount();

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
	printf("[CAN] Gateway node init OK\r\n");

	while(1)
	{
		/* 1. Send queued body-control commands generated from MQTT. */
		while(xCanTxQueue != NULL &&
			xQueueReceive(xCanTxQueue, &tx_frame, 0) == pdPASS)
		{
			printf("[CAN] TX queue pop: id=0x%03lX dlc=%u data0=0x%02X\r\n",
				(unsigned long)tx_frame.id,
				(unsigned)tx_frame.dlc,
				(unsigned)tx_frame.data[0]);
			if(Driver_CAN_Send(tx_frame.id, tx_frame.data, tx_frame.dlc) == 0)
			{
				g_can_tx_count++;
				printf("[CAN] TX sent: id=0x%03lX count=%lu\r\n",
					(unsigned long)tx_frame.id,
					(unsigned long)g_can_tx_count);
			}
			else
			{
				g_can_node_mode = CAN_NODE_DEGRADED;
				printf("[CAN] TX send failed: id=0x%03lX\r\n",
					(unsigned long)tx_frame.id);
			}
		}

		/* 2. Build a periodic heartbeat frame so the node has automotive-like liveness. */
		prvBuildHeartbeatFrame(&tx_frame);
		if(Driver_CAN_Send(tx_frame.id, tx_frame.data, tx_frame.dlc) == 0)
		{
			g_can_tx_count++;
		}
		else
		{
			g_can_node_mode = CAN_NODE_DEGRADED;
		}

		/* 3. Drain loopback or bus RX frames, then mirror them to the RX queue for MQTT uplink. */
		while(Driver_CAN_Recv(&rx_id, rx_data, &rx_len, 5) == 0)
		{
			memset(&rx_frame, 0, sizeof(rx_frame));
			rx_frame.id = rx_id;
			rx_frame.dlc = rx_len;
			memcpy(rx_frame.data, rx_data, rx_len);

			printf("[CAN] RX frame: id=0x%03lX dlc=%u data0=0x%02X\r\n",
				(unsigned long)rx_frame.id,
				(unsigned)rx_frame.dlc,
				(unsigned)rx_frame.data[0]);

			prvHandleReceivedFrame(&rx_frame);

			if(xCanRxQueue != NULL)
			{
				(void)xQueueSendToBack(xCanRxQueue, &rx_frame, 0);
				printf("[CAN] RX queued for MQTT: id=0x%03lX\r\n",
					(unsigned long)rx_frame.id);
			}

			if(rx_frame.id == CAN_ID_BODY_CMD)
			{
				CanFrame status_frame = {0};
				status_frame.id = CAN_ID_BODY_STATUS;
				status_frame.dlc = 2;
				status_frame.data[0] = g_can_body_status;
				status_frame.data[1] = g_can_node_mode;

				if(Driver_CAN_Send(status_frame.id, status_frame.data, status_frame.dlc) == 0)
				{
					g_can_tx_count++;
					printf("[CAN] BODY_STATUS sent: data0=0x%02X mode=%u\r\n",
						(unsigned)status_frame.data[0],
						(unsigned)status_frame.data[1]);
				}
			}
		}

		vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
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
