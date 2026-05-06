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

extern TaskHandle_t ledTaskHandle;
extern QueueHandle_t xKeyQueue;

/* ========== MQTT 服务器参数 ========== */
const static char clientID[] = "STM32_SmartHome_F407";
const static char username[] = "";
const static char password[] = "";

const static char LedTopic[] = "/smarthome/led/cmd";
const static char KeyTopic[] = "/smarthome/key/info";

/* 重连状态机 */
typedef enum {
	RECONN_INIT,           /* 初始化网络和 MQTT 客户端 */
	RECONN_TCP,            /* 建立 TCP 连接到 MQTT Broker */
	RECONN_MQTT_CONNECT,   /* MQTT CONNECT 握手 */
	RECONN_MQTT_SUB,       /* 订阅下行主题 */
	RECONN_RUNNING,        /* 正常运行：处理按键上报 + MQTT Yield */
	RECONN_RETRY_DELAY     /* 断开后等待 5 秒再重连 */
} ReconnState;

/* 全局重连计数器，供诊断任务读取。 */
volatile uint32_t g_mqtt_reconn_count = 0;
volatile ReconnState g_mqtt_state = RECONN_INIT;

/**
 * @brief MQTT 消息接收回调函数
 */
void messageArrived(MessageData* data)
{
	printf("Message arrived on topic %.*s: %.*s\n",
		data->topicName->lenstring.len, data->topicName->lenstring.data,
		data->message->payloadlen, (char*)data->message->payload);

	if(strstr(data->topicName->lenstring.data, LedTopic) != 0)
	{
		if(strstr((char*)data->message->payload, "led on") != 0)
		{
			xTaskNotify(ledTaskHandle, 1, eSetValueWithOverwrite);
		}
		else if(strstr((char*)data->message->payload, "led off") != 0)
		{
			xTaskNotify(ledTaskHandle, 0, eSetValueWithOverwrite);
		}
	}
}

/**
 * @brief MQTT 主任务：状态机驱动的连接管理和业务处理
 */
static void prvMQTTEchoTask(void *pvParameters)
{
	KeyEvent key = {0};
	MQTTClient client;
	Network network;
	unsigned char sendbuf[256];
	unsigned char readbuf[256];
	int rc = 0;
	MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
	char* address = "www.yanzmain.com.cn";
	ReconnState state = RECONN_INIT;

	(void)pvParameters;

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
			NetworkInit(&network);
			MQTTClientInit(&client, &network, 30000,
				sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));
			state = RECONN_TCP;
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
			printf("[MQTT] SUBSCRIBE: %s\r\n", LedTopic);
			rc = MQTTSubscribe(&client, LedTopic, 0, messageArrived);
			if(rc == 0)
			{
				printf("[MQTT] Subscribe OK, entering RUNNING\r\n");
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
				char payload[64];

				message.qos = 0;
				message.retained = 0;
				message.payload = payload;
				sprintf(payload, "key number %d, Press time:%d ms",
					key.num, key.time);
				message.payloadlen = strlen(payload);
				printf("%s\r\n", payload);

				rc = MQTTPublish(&client, KeyTopic, &message);
				if(rc != 0)
				{
					printf("[MQTT] Publish failed (rc=%d)\r\n", rc);
				}
			}

#if !defined(MQTT_TASK)
			rc = MQTTYield(&client, 100);
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
			NULL) == pdPASS)
	{
		printf("Create MQTT Task success.\r\n");
	}
	else
	{
		printf("Create MQTT Task failed.\r\n");
	}
}
