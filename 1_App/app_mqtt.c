/**
 * @file app_mqtt.c
 * @brief 智能家居项目 - MQTT 网络通信应用层逻辑（F407 重连版）
 * @note 状态机驱动的 WiFi/TCP/MQTT 断线自动重连，支持弱网环境自愈
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "MQTTClient.h"
#include "dev_io.h"
#include "cJSON.h"
#include "driver_can.h"

extern void CAN_RequestClearDiagnostics(void);
extern TaskHandle_t ledTaskHandle;
extern QueueHandle_t xKeyQueue;
extern TaskHandle_t xMqttTaskHandle;
extern QueueHandle_t xCanTxQueue;
extern QueueHandle_t xCanRxQueue;
extern volatile uint32_t g_can_tx_count;
extern volatile uint32_t g_can_tx_fail_count;
extern volatile uint32_t g_can_last_tx_fail_id;
extern volatile uint32_t g_can_last_error;
extern volatile uint32_t g_can_last_esr;
extern volatile uint32_t g_can_rx_count;
extern volatile uint32_t g_can_last_rx_id;
extern volatile uint8_t g_can_last_cmd_seq;
extern volatile uint8_t g_can_last_status_seq;
extern volatile uint8_t g_can_pending_cmd_seq;
extern volatile uint8_t g_can_pending_cmd_active;
extern volatile uint8_t g_can_seq_consistent;
extern volatile uint32_t g_can_seq_match_count;
extern volatile uint32_t g_can_seq_mismatch_count;
extern volatile uint8_t g_can_last_seq_expected;
extern volatile uint8_t g_can_last_seq_observed;
extern volatile uint8_t g_can_gateway_mode;
extern volatile uint8_t g_can_slave_mode;
extern volatile uint8_t g_can_body_status;
extern volatile uint8_t g_can_status_dirty;
extern volatile uint8_t g_can_slave_online;
extern volatile uint32_t g_can_slave_timeout_count;
extern volatile uint32_t g_can_last_slave_rx_age_ms;
extern volatile uint32_t g_can_dtc_mask;
extern volatile uint8_t g_can_last_dtc;
extern volatile uint32_t g_can_cmd_drop_count;
extern volatile uint32_t g_can_isr_drop_count;
extern volatile uint32_t g_can_uplink_drop_count;
extern volatile uint32_t g_mqtt_reconn_count;
extern void CAN_RecordCommandQueueDrop(void);

/* ========== MQTT 服务器参数 ========== */
const static char clientID[] = "STM32_SmartHome_F407";
const static char username[] = "";
const static char password[] = "";

const static char LedTopic[] = "/smarthome/led/cmd";
const static char KeyTopic[] = "/smarthome/key/info";
const static char VehicleKeyTopic[] = "/vehicle/bcm/input";
const static char BcmCmdTopic[] = "/vehicle/bcm/command";
const static char BcmStatusTopic[] = "/vehicle/bcm/status";
const static char LegacyVehicleCmdTopic[] = "/vehicle/body/cmd";

static uint8_t s_next_cmd_seq = 1U;

/* Reconnect state machine shared with diagnostics/status reporting. */
typedef enum {
	RECONN_INIT,           /* 初始化网络和 MQTT 客户端 */
	RECONN_TCP,            /* 建立 TCP 连接到 MQTT Broker */
	RECONN_MQTT_CONNECT,   /* MQTT CONNECT 握手 */
	RECONN_MQTT_SUB,       /* 订阅下行主题 */
	RECONN_RUNNING,        /* 正常运行：处理按键上报 + MQTT Yield */
	RECONN_RETRY_DELAY     /* 断开后等待 5 秒再重连 */
} ReconnState;

extern volatile ReconnState g_mqtt_state;

static int prvPublishText(MQTTClient *client, const char *topic, const char *payload)
{
	MQTTMessage message;
	int rc = -1;

	if(client == NULL || topic == NULL || payload == NULL)
	{
		return -1;
	}

	message.qos = QOS0;
	message.retained = 0;
	message.payload = (void *)payload;
	message.payloadlen = strlen(payload);
	printf("[MQTT] Publish -> %s: %s\r\n", topic, payload);
	rc = MQTTPublish(client, topic, &message);
	if(rc != 0)
	{
		printf("[MQTT] Publish failed: topic=%s rc=%d\r\n", topic, rc);
	}
	return rc;
}

static void prvPublishCanFrame(MQTTClient *client, const CanFrame *frame)
{
	char payload[192];

	if(client == NULL || frame == NULL)
	{
		return;
	}

	if(frame->id == CAN_ID_BODY_STATUS && frame->dlc >= 2U)
	{
		snprintf(payload, sizeof(payload),
			"{\"src\":\"bcm\",\"frame\":\"body_status\",\"can_id\":%lu,"
			"\"lamp\":%s,\"hazard\":%s,\"fan\":%s,\"node_mode\":%u,"
			"\"seq\":%u}",
			(unsigned long)frame->id,
			(frame->data[CAN_BODY_STATUS_BYTE_MASK] & CAN_BODY_CTRL_LAMP) ? "true" : "false",
			(frame->data[CAN_BODY_STATUS_BYTE_MASK] & CAN_BODY_CTRL_HAZARD) ? "true" : "false",
			(frame->data[CAN_BODY_STATUS_BYTE_MASK] & CAN_BODY_CTRL_FAN) ? "true" : "false",
			(unsigned)frame->data[CAN_BODY_STATUS_BYTE_MODE],
			(frame->dlc > CAN_BODY_STATUS_BYTE_SEQ) ? (unsigned)frame->data[CAN_BODY_STATUS_BYTE_SEQ] : 0U);
	}
	else
	{
		snprintf(payload, sizeof(payload),
			"{\"src\":\"can\",\"frame\":\"raw\",\"can_id\":%lu,\"dlc\":%u,"
			"\"data\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
			(unsigned long)frame->id,
			(unsigned)frame->dlc,
			(unsigned)frame->data[0], (unsigned)frame->data[1],
			(unsigned)frame->data[2], (unsigned)frame->data[3],
			(unsigned)frame->data[4], (unsigned)frame->data[5],
			(unsigned)frame->data[6], (unsigned)frame->data[7]);
	}
	prvPublishText(client, BcmStatusTopic, payload);
}

static int prvPublishGatewayStatus(MQTTClient *client)
{
	char payload[448];
	int written = 0;
	int rc = -1;

	if(client == NULL)
	{
		return -1;
	}

	written = snprintf(payload, sizeof(payload),
		"{\"src\":\"bcm\",\"frame\":\"node_status\",\"gateway_mode\":%u,\"slave_mode\":%u,"
		"\"output_mask\":%u,\"lamp\":%s,\"hazard\":%s,\"fan\":%s,"
		"\"last_cmd_seq\":%u,\"last_status_seq\":%u,"
		"\"seq_ok\":%s,\"pending_seq\":%u,\"pending_active\":%s,"
		"\"seq_expected\":%u,\"seq_observed\":%u,"
		"\"dtc_mask\":%lu,\"last_dtc\":%u,"
		"\"slave_online\":%s,\"slave_timeout_cnt\":%lu,\"last_slave_age_ms\":%lu,"
		"\"mqtt_state\":%d}",
		(unsigned)g_can_gateway_mode,
		(unsigned)g_can_slave_mode,
		(unsigned)g_can_body_status,
		(g_can_body_status & CAN_BODY_CTRL_LAMP) ? "true" : "false",
		(g_can_body_status & CAN_BODY_CTRL_HAZARD) ? "true" : "false",
		(g_can_body_status & CAN_BODY_CTRL_FAN) ? "true" : "false",
		(unsigned)g_can_last_cmd_seq,
		(unsigned)g_can_last_status_seq,
		g_can_seq_consistent ? "true" : "false",
		(unsigned)g_can_pending_cmd_seq,
		g_can_pending_cmd_active ? "true" : "false",
		(unsigned)g_can_last_seq_expected,
		(unsigned)g_can_last_seq_observed,
		(unsigned long)g_can_dtc_mask,
		(unsigned)g_can_last_dtc,
		g_can_slave_online ? "true" : "false",
		(unsigned long)g_can_slave_timeout_count,
		(unsigned long)g_can_last_slave_rx_age_ms,
		(int)g_mqtt_state);
	if(written < 0 || written >= (int)sizeof(payload))
	{
		printf("[MQTT] node_status payload truncated\r\n");
		return -1;
	}
	rc = prvPublishText(client, BcmStatusTopic, payload);
	if(rc != 0)
	{
		return rc;
	}

	written = snprintf(payload, sizeof(payload),
		"{\"src\":\"bcm\",\"frame\":\"diag_status\","
		"\"can_tx\":%lu,\"can_rx\":%lu,\"last_rx_id\":%lu,"
		"\"can_tx_fail\":%lu,\"last_tx_fail_id\":%lu,"
		"\"can_error\":%lu,\"can_esr\":%lu,"
		"\"seq_match\":%lu,\"seq_mismatch\":%lu,"
		"\"slave_timeout\":%lu,\"cmd_drop\":%lu,\"isr_drop\":%lu,"
		"\"uplink_drop\":%lu,\"mqtt_reconn\":%lu}",
		(unsigned long)g_can_tx_count,
		(unsigned long)g_can_rx_count,
		(unsigned long)g_can_last_rx_id,
		(unsigned long)g_can_tx_fail_count,
		(unsigned long)g_can_last_tx_fail_id,
		(unsigned long)g_can_last_error,
		(unsigned long)g_can_last_esr,
		(unsigned long)g_can_seq_match_count,
		(unsigned long)g_can_seq_mismatch_count,
		(unsigned long)g_can_slave_timeout_count,
		(unsigned long)g_can_cmd_drop_count,
		(unsigned long)g_can_isr_drop_count,
		(unsigned long)g_can_uplink_drop_count,
		(unsigned long)g_mqtt_reconn_count);
	if(written < 0 || written >= (int)sizeof(payload))
	{
		printf("[MQTT] diag_status payload truncated\r\n");
		return -1;
	}

	return prvPublishText(client, BcmStatusTopic, payload);
}

static void prvPublishGatewayStatusSnapshot(MQTTClient *client)
{
	taskENTER_CRITICAL();
	g_can_status_dirty = 0U;
	taskEXIT_CRITICAL();

	if(prvPublishGatewayStatus(client) != 0)
	{
		g_can_status_dirty = 1U;
	}
}

static int prvTopicEquals(const MQTTString *topic, const char *literal)
{
	size_t literal_len = strlen(literal);

	return topic != NULL &&
		topic->lenstring.data != NULL &&
		topic->lenstring.len == (int)literal_len &&
		memcmp(topic->lenstring.data, literal, literal_len) == 0;
}

static int prvPayloadEquals(const MQTTMessage *message, const char *literal)
{
	size_t literal_len = strlen(literal);

	return message != NULL &&
		message->payload != NULL &&
		message->payloadlen == literal_len &&
		memcmp(message->payload, literal, literal_len) == 0;
}

/* 全局重连计数器，供诊断任务读取。 */
volatile uint32_t g_mqtt_reconn_count = 0;
volatile ReconnState g_mqtt_state = RECONN_INIT;

/**
 * @brief MQTT 消息接收回调函数
 */
void messageArrived(MessageData* data)
{
	char buf[128];
	int plen;

	printf("Message arrived on topic %.*s: %.*s\n",
		data->topicName->lenstring.len, data->topicName->lenstring.data,
		data->message->payloadlen, (char*)data->message->payload);

	if(!(prvTopicEquals(data->topicName, LedTopic) ||
		prvTopicEquals(data->topicName, BcmCmdTopic) ||
		prvTopicEquals(data->topicName, LegacyVehicleCmdTopic)))
		return;

	plen = data->message->payloadlen;
	if(plen <= 0 || plen >= (int)sizeof(buf) || data->message->payload == NULL)
		return;

	memcpy(buf, data->message->payload, plen);
	buf[plen] = '\0';

	cJSON *root = cJSON_Parse(buf);
	if(root != NULL)
	{
		cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
		if(cJSON_IsString(cmd) && cmd->valuestring != NULL)
		{
			if(strcmp(cmd->valuestring, "led") == 0)
			{
				if(prvTopicEquals(data->topicName, LedTopic))
				{
					cJSON *action = cJSON_GetObjectItem(root, "action");
					if(cJSON_IsString(action) && action->valuestring != NULL)
					{
						printf("[MQTT] LED cmd parsed: action=%s\r\n", action->valuestring);
						if(strcmp(action->valuestring, "on") == 0)
							xTaskNotify(ledTaskHandle, 1, eSetValueWithOverwrite);
						else if(strcmp(action->valuestring, "off") == 0)
							xTaskNotify(ledTaskHandle, 0, eSetValueWithOverwrite);
					}
				}
				else
				{
					printf("[MQTT] Ignore plain LED cmd on BCM topic\r\n");
				}
			}
			else if(strcmp(cmd->valuestring, "body_ctrl") == 0 ||
				strcmp(cmd->valuestring, "bcm_ctrl") == 0)
			{
				CanFrame frame = {0};
				cJSON *lamp = cJSON_GetObjectItem(root, "lamp");
				cJSON *hazard = cJSON_GetObjectItem(root, "hazard");
				cJSON *fan = cJSON_GetObjectItem(root, "fan");
				cJSON *seq = cJSON_GetObjectItem(root, "seq");

				frame.id = CAN_ID_BODY_CMD;
				frame.dlc = 2;
				frame.data[CAN_BODY_CMD_BYTE_MASK] = 0;
				if(cJSON_IsNumber(seq) && seq->valueint >= 0 && seq->valueint <= 255)
				{
					frame.data[CAN_BODY_CMD_BYTE_SEQ] = (uint8_t)seq->valueint;
				}
				else
				{
					frame.data[CAN_BODY_CMD_BYTE_SEQ] = s_next_cmd_seq++;
					if(s_next_cmd_seq == 0U)
					{
						s_next_cmd_seq = 1U;
					}
				}

				if(cJSON_IsBool(lamp) && cJSON_IsTrue(lamp))
					frame.data[CAN_BODY_CMD_BYTE_MASK] |= CAN_BODY_CTRL_LAMP;
				if(cJSON_IsBool(hazard) && cJSON_IsTrue(hazard))
					frame.data[CAN_BODY_CMD_BYTE_MASK] |= CAN_BODY_CTRL_HAZARD;
				if(cJSON_IsBool(fan) && cJSON_IsTrue(fan))
					frame.data[CAN_BODY_CMD_BYTE_MASK] |= CAN_BODY_CTRL_FAN;

				printf("[MQTT] body_ctrl parsed: lamp=%d hazard=%d fan=%d data0=0x%02X seq=%u\r\n",
					cJSON_IsBool(lamp) ? cJSON_IsTrue(lamp) : -1,
					cJSON_IsBool(hazard) ? cJSON_IsTrue(hazard) : -1,
					cJSON_IsBool(fan) ? cJSON_IsTrue(fan) : -1,
					(unsigned)frame.data[CAN_BODY_CMD_BYTE_MASK],
					(unsigned)frame.data[CAN_BODY_CMD_BYTE_SEQ]);

				if(xCanTxQueue != NULL)
				{
					if(xQueueSendToBack(xCanTxQueue, &frame, 0) != pdPASS)
					{
						CAN_RecordCommandQueueDrop();
						printf("[MQTT] CAN TX queue full\r\n");
					}
					else
					{
						printf("[MQTT] CAN TX queued: id=0x%03lX data0=0x%02X\r\n",
							(unsigned long)frame.id,
							(unsigned)frame.data[CAN_BODY_CMD_BYTE_MASK]);
					}
				}
				else
				{
					printf("[MQTT] xCanTxQueue is NULL\r\n");
				}
			}
			else if(strcmp(cmd->valuestring, "get_status") == 0)
			{
				printf("[MQTT] get_status: uptime=%lu reconns=%lu state=%d\r\n",
					(unsigned long)xTaskGetTickCount(),
					(unsigned long)g_mqtt_reconn_count,
					(int)g_mqtt_state);
			}
			else if(strcmp(cmd->valuestring, "clear_dtc") == 0)
			{
				CAN_RequestClearDiagnostics();
				printf("[MQTT] clear_dtc requested\r\n");
			}
		}
		cJSON_Delete(root);
	}
	else
	{
		/* JSON 解析失败，回退到纯文本匹配（兼容旧版控制端） */
		if(prvPayloadEquals(data->message, "led on"))
			xTaskNotify(ledTaskHandle, 1, eSetValueWithOverwrite);
		else if(prvPayloadEquals(data->message, "led off"))
			xTaskNotify(ledTaskHandle, 0, eSetValueWithOverwrite);
	}
}

/**
 * @brief MQTT 主任务：状态机驱动的连接管理和业务处理
 */
static void prvMQTTEchoTask(void *pvParameters)
{
	KeyEvent key = {0};
	CanFrame can_frame = {0};
	MQTTClient client;
	Network network = {0};
	unsigned char sendbuf[512];
	unsigned char readbuf[512];
	int rc = 0;
	MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
	char* address = "www.yanzmain.com.cn";
	ReconnState state = RECONN_INIT;

	(void)pvParameters;
	printf("[MQTT] Task started\r\n");

	/* 一次性配置 MQTT 连接参数（每次重连前复用） */
	connectData.MQTTVersion = 3;
	connectData.clientID.cstring = (char*)clientID;
	connectData.username.cstring = (char*)username;
	connectData.password.cstring = (char*)password;

	while(1)
	{
		g_mqtt_state = state;

		switch(state)
		{
		case RECONN_INIT:
			printf("[MQTT] INIT: Network + WiFi init...\r\n");
			memset(&network, 0, sizeof(network));
			NetworkInit(&network);
			if(network.mqttread != NULL && network.mqttwrite != NULL)
			{
				MQTTClientInit(&client, &network, 30000,
					sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));
				state = RECONN_TCP;
			}
			else
			{
				state = RECONN_RETRY_DELAY;
			}
			break;

		case RECONN_TCP:
			printf("[MQTT] TCP: connecting to %s:1883...\r\n", address);
			rc = NetworkConnect(&network, address, 1883);
			if(rc == 0)
			{
				state = RECONN_MQTT_CONNECT;
			}
			else
			{
				printf("[MQTT] TCP connect failed (rc=%d)\r\n", rc);
				state = RECONN_RETRY_DELAY;
			}
			break;

		case RECONN_MQTT_CONNECT:
			printf("[MQTT] MQTT CONNECT...\r\n");
			rc = MQTTConnect(&client, &connectData);
			if(rc == 0)
			{
				printf("[MQTT] MQTT Connected\r\n");
				state = RECONN_MQTT_SUB;
			}
			else
			{
				printf("[MQTT] MQTT connect failed (rc=%d)\r\n", rc);
				state = RECONN_RETRY_DELAY;
			}
			break;

		case RECONN_MQTT_SUB:
			printf("[MQTT] SUBSCRIBE: %s + %s (+ legacy %s)\r\n",
				LedTopic, BcmCmdTopic, LegacyVehicleCmdTopic);
			rc = MQTTSubscribe(&client, LedTopic, QOS0, messageArrived);
			if(rc == 0)
			{
				rc = MQTTSubscribe(&client, BcmCmdTopic, QOS0, messageArrived);
			}
			if(rc == 0)
			{
				rc = MQTTSubscribe(&client, LegacyVehicleCmdTopic, QOS0, messageArrived);
			}
			if(rc == 0)
			{
				printf("[MQTT] Subscribe OK, entering RUNNING\r\n");
				prvPublishGatewayStatusSnapshot(&client);
				state = RECONN_RUNNING;
			}
			else
			{
				printf("[MQTT] Subscribe failed (rc=%d)\r\n", rc);
				state = RECONN_RETRY_DELAY;
			}
			break;

		case RECONN_RUNNING:
		{
			/* 按键事件：队列 → MQTT Publish */
			if(xKeyQueue != NULL &&
				xQueueReceive(xKeyQueue, (uint8_t*)&key, 10) == pdPASS)
			{
				MQTTMessage message;
				char payload[128];

				message.qos = QOS0;
				message.retained = 0;
				message.payload = payload;
				snprintf(payload, sizeof(payload),
					"{\"src\":\"input\",\"device\":\"key\",\"key_id\":%u,\"press_ms\":%u}",
					(unsigned)key.num, (unsigned)key.time);
				message.payloadlen = strlen(payload);
				printf("[MQTT] Key event payload: %s\r\n", payload);

				rc = MQTTPublish(&client, VehicleKeyTopic, &message);
				if(rc != 0)
				{
					printf("[MQTT] Publish failed (rc=%d)\r\n", rc);
				}
				else
				{
					printf("[MQTT] Key event published -> %s\r\n", VehicleKeyTopic);
				}

				(void)MQTTPublish(&client, KeyTopic, &message);
			}

			while(xCanRxQueue != NULL &&
				xQueueReceive(xCanRxQueue, &can_frame, 0) == pdPASS)
			{
				if(can_frame.id != CAN_ID_GATEWAY_HEARTBEAT &&
					can_frame.id != CAN_ID_SLAVE_HEARTBEAT)
				{
					printf("[MQTT] CAN RX dequeued for uplink: id=0x%03lX dlc=%u data0=0x%02X\r\n",
						(unsigned long)can_frame.id,
						(unsigned)can_frame.dlc,
						(unsigned)can_frame.data[0]);
					prvPublishCanFrame(&client, &can_frame);
				}
			}

			if(g_can_status_dirty != 0U)
			{
				prvPublishGatewayStatusSnapshot(&client);
			}

#if !defined(MQTT_TASK)
			rc = MQTTYield(&client, 500);
			if(rc != 0)
			{
				printf("[MQTT] Yield error (rc=%d), connection lost\r\n", rc);
				state = RECONN_RETRY_DELAY;
				break;
			}
#endif
			vTaskDelay(1);
			break;
		}

		case RECONN_RETRY_DELAY:
			printf("[MQTT] RETRY: cleanup, reconnecting in 5s "
				"(total reconns=%lu)\r\n",
				(unsigned long)g_mqtt_reconn_count);

			/* 清理 MQTT 和 TCP 连接上下文 */
			MQTTDisconnect(&client);
			NetworkDisconnect(&network);
			/* 如果底层驱动支持 WiFi 断开，可在此处调用 */

			g_mqtt_reconn_count++;
			vTaskDelay(pdMS_TO_TICKS(5000));
			state = RECONN_INIT;
			break;

		default:
			state = RECONN_INIT;
			break;
		}
	}
}

void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	/* 重连状态机 + 后续 cJSON 需要更大栈空间 */
	if(xTaskCreate(prvMQTTEchoTask,
			"MQTTEcho0",
			usTaskStackSize < 1024 ? 1024 : usTaskStackSize,
			(void *)x,
			uxTaskPriority,
			&xMqttTaskHandle) == pdPASS)
	{
		printf("Create MQTT Task success.\r\n");
	}
	else
	{
		printf("Create MQTT Task failed.\r\n");
	}
}
