/**
 * @file driver_net.c
 * @brief ESP8266 AT 驱动与 TCP 负载解析器
 */

#include "driver_net.h"
#include "driver_buffer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "string.h"
#include "stdio.h"

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart);
/* 逐字节解析 ESP8266 回包中的 +IPD 数据帧。 */
void NetDataProcess_Callback(uint8_t data);

/*
 * USART2 must stay within the FreeRTOS "syscall-safe" priority range because
 * the RX ISR wakes the MQTT task with vTaskNotifyGiveFromISR().
 * USART2 中断优先级必须位于 FreeRTOS 可调用系统 API 的安全范围内。
 */
#define NET_UART_IRQ_PRIORITY 12U

static UART_HandleTypeDef huart2;

/* 保存 AT 指令应答流（如 OK / ERROR / SEND OK）。 */
static RingBuffer CMDRetBuffer;
/* 保存从 +IPD 帧中提取出的纯 TCP 负载。 */
static RingBuffer NetDataBuffer;
/* 当前等待网络接收事件的任务句柄。 */
static TaskHandle_t xNetWaitTaskHandle = NULL;

static void Driver_Net_RegisterCurrentTask(void)
{
	/* 记录当前任务，便于 ISR 收到数据后定向唤醒。 */
	if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		xNetWaitTaskHandle = xTaskGetCurrentTaskHandle();
	}
}

static void Driver_Net_ClearWaitNotification(void)
{
	/* 清理旧通知，避免把历史事件误当成本次收包事件。 */
	if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
		xNetWaitTaskHandle == xTaskGetCurrentTaskHandle())
	{
		(void)ulTaskNotifyTake(pdTRUE, 0);
	}
}

static void Driver_Net_TaskDelay(uint32_t delay_ms)
{
	if(delay_ms == 0)
	{
		return;
	}

	if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		/* RTOS 运行时优先使用任务延时，避免忙等。 */
		vTaskDelay(pdMS_TO_TICKS(delay_ms));
	}
	else
	{
		/* 调度器未启动时退化为 HAL 阻塞延时。 */
		HAL_Delay(delay_ms);
	}
}

static void Driver_Net_WaitForRxActivity(uint32_t timeout_ms)
{
	if(timeout_ms == 0)
	{
		return;
	}

	if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
		xNetWaitTaskHandle != NULL &&
		xNetWaitTaskHandle == xTaskGetCurrentTaskHandle())
	{
		/* 等待 ISR 通过任务通知告知“有新数据到达”。 */
		(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
	}
	else
	{
		HAL_Delay(timeout_ms);
	}
}

static int Driver_Net_WaitForReply(const char *reply, uint16_t timeout)
{
	/* 在 AT 应答缓冲区中轮询匹配目标关键字。 */
	uint8_t i = 0;
	char buf[128] = {0};

	if(reply == NULL || timeout == 0)
	{
		return -1;
	}

	Driver_Net_ClearWaitNotification();

	while(timeout != 0)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, (uint8_t*)&buf[i]) == 0)
		{
			i = (i + 1) % sizeof(buf);
			if(strstr(buf, reply) != 0)
			{
				/* 找到目标应答。 */
				return 0;
			}
		}
		else
		{
			timeout--;
			Driver_Net_WaitForRxActivity(1);
		}
	}

	return -1;
}

static int Driver_Net_UART_Init(void)
{
	/* 初始化 USART2（连接 ESP8266）。 */
	huart2.Instance = USART2;
	huart2.Init.BaudRate = 115200;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART2_MspInit(&huart2);

	if(HAL_UART_Init(&huart2) != HAL_OK)
	{
		return -1;
	}

	/* Capture every received byte immediately through RXNE interrupts. */
	/* 开启 RXNE 中断，逐字节接收，降低数据丢失风险。 */
	__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

	return 0;
}

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart)
{
	/* 配置 USART2 的 GPIO 与 NVIC。 */
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(huart->Instance == USART2)
	{
		__HAL_RCC_USART2_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();

		/* USART2_TX -> PA2 */
		GPIO_InitStruct.Pin = GPIO_PIN_2;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		/* USART2_RX -> PA3 */
		GPIO_InitStruct.Pin = GPIO_PIN_3;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_INPUT;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		HAL_NVIC_SetPriority(USART2_IRQn, NET_UART_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(USART2_IRQn);
	}
}

void USART2_IRQHandler(void)
{
	/* 串口接收中断：保存 AT 流 + 解析 +IPD + 通知等待任务。 */
	uint8_t rx_data = 0;
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if(__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) == SET)
	{
		__HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_RXNE);
		rx_data = USART2->DR;

		/* Keep the raw modem reply stream for AT command matching. */
		Driver_Buffer_Write(&CMDRetBuffer, rx_data);

		/* Parse and extract pure TCP payload from +IPD frames. */
		NetDataProcess_Callback(rx_data);

		if(xNetWaitTaskHandle != NULL)
		{
			vTaskNotifyGiveFromISR(xNetWaitTaskHandle, &xHigherPriorityTaskWoken);
		}
	}

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static int Driver_Net_TransmitCmd(const char *cmd, const char *reply, uint16_t timeout)
{
	/* 发送 AT 指令，并等待指定应答字符串。 */
	char buf[128] = {0};
	int ret = -1;

	Driver_Net_RegisterCurrentTask();
	strcat(buf, cmd);

	if(strstr(buf, "\r\n") == NULL)
	{
		strcat(buf, "\r\n");
	}

	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500);

	ret = Driver_Net_WaitForReply(reply, timeout);
	memset(buf, 0, sizeof(buf));
	return ret;
}

int Driver_Net_TransmitSocket(const char *socket, int len, int timeout)
{
	/* 两阶段发送：先 CIPSEND 获取 '>'，再发实际数据等待 SEND OK。 */
	char cmd[16] = {0};
	int ret = -1;

	Driver_Net_RegisterCurrentTask();

	/* Stage 1: request the ESP8266 transmit prompt ('>'). */
	sprintf(cmd, "AT+CIPSEND=%d\r\n", len);
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 500);

	if(Driver_Net_WaitForReply(">", timeout) != 0)
	{
		return -1;
	}

	/* Stage 2: push the actual payload and wait for SEND OK. */
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)socket, len, 500);

	ret = Driver_Net_WaitForReply("SEND OK", timeout);
	return ret;
}

int Driver_Net_RecvSocket(char *buf, int len, int timeout)
{
	/* 从纯数据缓冲区按目标长度读取，超时返回未完成状态。 */
	int recvLen = 0;

	if(buf == NULL || len <= 0)
	{
		return -1;
	}

	Driver_Net_RegisterCurrentTask();
	Driver_Net_ClearWaitNotification();

	while(timeout != 0 && recvLen < len)
	{
		int onceReadLen = len - recvLen;
		if(onceReadLen > 255)
		{
			onceReadLen = 255;
		}

		onceReadLen = Driver_Buffer_ReadBytes(&NetDataBuffer, (uint8_t*)&buf[recvLen], (uint8_t)onceReadLen);
		if(onceReadLen > 0)
		{
			recvLen += onceReadLen;
			continue;
		}

		timeout--;
		Driver_Net_WaitForRxActivity(1);
	}

	return (recvLen == len) ? 0 : 1;
}

int Driver_Net_ConnectWiFi(const char *ssid, const char *pwd, int timeout)
{
	/* 组包并发送连接 WiFi 指令。 */
	char buf[50] = "AT+CWJAP=\"";

	strcat(buf, ssid);
	strcat(buf, "\",\"");
	strcat(buf, pwd);
	strcat(buf, "\"");

	return Driver_Net_TransmitCmd(buf, "GOT IP\r\n", timeout);
}

int Driver_Net_DisconnectWiFi(void)
{
	return Driver_Net_TransmitCmd("AT+CWQAP", "OK\r\n", 500);
}

int Driver_Net_ConnectTCP(const char *ip, int port, int timeout)
{
	/* 先配置单连接与普通传输模式，再发起 TCP 连接。 */
	char buf[128] = "AT+CIPSTART=\"TCP\",\"";

	if(Driver_Net_TransmitCmd("AT+CIPMUX=0", "OK\r\n", 500) != 0)
	{
		return -1;
	}
	Driver_Net_TaskDelay(1);

	if(Driver_Net_TransmitCmd("AT+CIPMODE=0", "OK\r\n", 500) != 0)
	{
		return -1;
	}
	Driver_Net_TaskDelay(1);

	sprintf(&buf[19], "%s\",%d", ip, port);
	return Driver_Net_TransmitCmd(buf, "OK\r\n", timeout);
}

int Driver_Net_Disconnect_TCP_UDP(void)
{
	return Driver_Net_TransmitCmd("AT+CIPCLOSE", "OK\r\n", 500);
}

typedef enum AT_STATUS{
	/* 解析 +IPD 的状态机：帧头 -> 长度 -> 数据体。 */
	INIT_STATUS,
	LEN_STATUS,
	DATA_STATUS
}AT_STATUS;

static uint8_t g_DataBuff[256] = {0};

void NetDataProcess_Callback(uint8_t data)
{
	/* 从串口字节流中提取 +IPD,<len>:<data> 里的 data 部分。 */
	uint8_t *buf = g_DataBuff;
	static AT_STATUS g_status = INIT_STATUS;
	static int g_DataBuffIndex = 0;
	static int g_DataLen = 0;
	int i = g_DataBuffIndex;
	int m = 0;

	buf[i] = data;
	g_DataBuffIndex++;

	switch(g_status)
	{
		case INIT_STATUS:
		{
			/* Find the "+IPD," frame prefix in the incoming byte stream. */
			if(buf[0] != '+')
			{
				g_DataBuffIndex = 0;
			}
			else if(i == 4)
			{
				if(strncmp((char*)buf, "+IPD,", 5) == 0)
				{
					g_status = LEN_STATUS;
				}
				g_DataBuffIndex = 0;
			}
			break;
		}

		case LEN_STATUS:
		{
			/* Collect ASCII digits until ':' and convert them to payload length. */
			if(buf[i] == ':')
			{
				for(m = 0; m < i; m++)
				{
					g_DataLen = g_DataLen * 10 + buf[m] - '0';
				}

				g_status = DATA_STATUS;
				g_DataBuffIndex = 0;
			}
			else if(i >= 9)
			{
				/* Reset the parser if the frame is malformed. */
				g_status = INIT_STATUS;
				g_DataBuffIndex = 0;
			}
			break;
		}

		case DATA_STATUS:
		{
			/* Once the declared number of bytes is received, push payload only. */
			if(g_DataBuffIndex == g_DataLen)
			{
				Driver_Buffer_WriteBytes(&NetDataBuffer, buf, g_DataLen);
				g_status = INIT_STATUS;
				g_DataBuffIndex = 0;
				g_DataLen = 0;
			}
			break;
		}

		default:
			break;
	}
}

int Driver_Net_Init(void)
{
	/* 初始化串口、缓冲区并完成 ESP8266 基础配置。 */
	Driver_Net_RegisterCurrentTask();

	if(Driver_Net_UART_Init() != 0)
	{
		return -1;
	}

	if(Driver_Buffer_Init(&CMDRetBuffer, 128) != 0)
	{
		return -1;
	}
	if(Driver_Buffer_Init(&NetDataBuffer, 1024) != 0)
	{
		return -1;
	}

	if(Driver_Net_TransmitCmd("AT+RST", "OK\r\n", 10000) != 0)
	{
		return -1;
	}
	Driver_Net_TaskDelay(500);

	if(Driver_Net_TransmitCmd("AT+CWMODE=1", "OK\r\n", 500) != 0)
	{
		return -1;
	}

	return 0;
}
