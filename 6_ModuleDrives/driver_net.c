/**
 * @file driver_net.c
 * @brief ESP8266 AT 椹卞姩涓?TCP 璐熻浇瑙ｆ瀽鍣?
 */

#include "driver_net.h"
#include "driver_buffer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "string.h"
#include "stdio.h"

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart);
static void Driver_Net_AppendReplyByte(char *buf,
	uint16_t capacity,
	uint16_t *length,
	uint8_t data);
/* Parse ESP8266 +IPD frames. Returns 1 for TCP payload bytes. */
static uint8_t NetDataProcess_Callback(uint8_t data);

/*
 * USART2 must stay within the FreeRTOS "syscall-safe" priority range because
 * the RX ISR wakes the MQTT task with vTaskNotifyGiveFromISR().
 * USART2 涓柇浼樺厛绾у繀椤讳綅浜?FreeRTOS 鍙皟鐢ㄧ郴缁?API 鐨勫畨鍏ㄨ寖鍥村唴銆?
 */
#define NET_UART_IRQ_PRIORITY 12U

static UART_HandleTypeDef huart2;

/* 淇濆瓨 AT 鎸囦护搴旂瓟娴侊紙濡?OK / ERROR / SEND OK锛夈€?*/
static RingBuffer CMDRetBuffer;
/* 淇濆瓨浠?+IPD 甯т腑鎻愬彇鍑虹殑绾?TCP 璐熻浇銆?*/
static RingBuffer NetDataBuffer;
/* 褰撳墠绛夊緟缃戠粶鎺ユ敹浜嬩欢鐨勪换鍔″彞鏌勩€?*/
static TaskHandle_t xNetWaitTaskHandle = NULL;
/* Initialize the modem only once; later reconnects should reuse the live link. */
static uint8_t g_net_driver_inited = 0U;
static volatile uint32_t s_net_payload_drop_count = 0U;
static volatile uint8_t s_net_rx_fault_pending = 0U;
static volatile uint8_t s_net_parser_reset_requested = 0U;

static void Driver_Net_RegisterCurrentTask(void)
{
	/* 璁板綍褰撳墠浠诲姟锛屼究浜?ISR 鏀跺埌鏁版嵁鍚庡畾鍚戝敜閱掋€?*/
	if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
	{
		xNetWaitTaskHandle = xTaskGetCurrentTaskHandle();
	}
}

static void Driver_Net_ClearWaitNotification(void)
{
	/* 娓呯悊鏃ч€氱煡锛岄伩鍏嶆妸鍘嗗彶浜嬩欢璇綋鎴愭湰娆℃敹鍖呬簨浠躲€?*/
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
		/* RTOS 杩愯鏃朵紭鍏堜娇鐢ㄤ换鍔″欢鏃讹紝閬垮厤蹇欑瓑銆?*/
		vTaskDelay(pdMS_TO_TICKS(delay_ms));
	}
	else
	{
		/* 璋冨害鍣ㄦ湭鍚姩鏃堕€€鍖栦负 HAL 闃诲寤舵椂銆?*/
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
		/* 绛夊緟 ISR 閫氳繃浠诲姟閫氱煡鍛婄煡鈥滄湁鏂版暟鎹埌杈锯€濄€?*/
		(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
	}
	else
	{
		HAL_Delay(timeout_ms);
	}
}

static int Driver_Net_WaitForReply(const char *reply, uint16_t timeout)
{
	/* 鍦?AT 搴旂瓟缂撳啿鍖轰腑杞鍖归厤鐩爣鍏抽敭瀛椼€?*/
	uint8_t data = 0U;
	uint16_t length = 0U;
	uint32_t start_tick = 0U;
	char buf[128] = {0};

	if(reply == NULL || timeout == 0)
	{
		return -1;
	}

	Driver_Net_ClearWaitNotification();
	start_tick = HAL_GetTick();

	while((uint32_t)(HAL_GetTick() - start_tick) < timeout)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, &data) == 0)
		{
			Driver_Net_AppendReplyByte(buf, sizeof(buf), &length, data);
			if(strstr(buf, reply) != 0)
			{
				/* 鎵惧埌鐩爣搴旂瓟銆?*/
				return 0;
			}
		}
		else
		{
			Driver_Net_WaitForRxActivity(1);
		}
	}

	return -1;
}

static int Driver_Net_WaitForTcpConnect(uint16_t timeout)
{
	static const char *const xSuccessReplies[] = {
		"OK\r\n",
		"CONNECT",
		"Linked",
		"ALREADY CONNECTED"
	};
	static const char *const xFailReplies[] = {
		"ERROR",
		"busy p...",
		"link is not valid",
		"DNS Fail",
		"no ip"
	};
	uint8_t data = 0U;
	uint8_t r = 0;
	uint16_t length = 0U;
	uint32_t start_tick = 0U;
	char buf[192] = {0};

	Driver_Net_ClearWaitNotification();
	start_tick = HAL_GetTick();

	while((uint32_t)(HAL_GetTick() - start_tick) < timeout)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, &data) == 0)
		{
			Driver_Net_AppendReplyByte(buf, sizeof(buf), &length, data);

			for(r = 0; r < (sizeof(xSuccessReplies) / sizeof(xSuccessReplies[0])); r++)
			{
				if(strstr(buf, xSuccessReplies[r]) != 0)
				{
					return 0;
				}
			}

			for(r = 0; r < (sizeof(xFailReplies) / sizeof(xFailReplies[0])); r++)
			{
				if(strstr(buf, xFailReplies[r]) != 0)
				{
					printf("[NET] CIPSTART reply: %s\r\n", buf);
					return -1;
				}
			}
		}
		else
		{
			Driver_Net_WaitForRxActivity(1);
		}
	}

	printf("[NET] CIPSTART timeout.\r\n");
	return -1;
}

static int Driver_Net_WaitForWiFiJoin(uint16_t timeout)
{
	static const char *const xSuccessReplies[] = {
		"WIFI GOT IP",
		"GOT IP"
	};
	static const char *const xFailReplies[] = {
		"+CWJAP:",
		"FAIL",
		"ERROR"
	};
	uint8_t data = 0U;
	uint8_t r = 0;
	uint16_t length = 0U;
	uint32_t start_tick = 0U;
	char buf[192] = {0};

	Driver_Net_ClearWaitNotification();
	start_tick = HAL_GetTick();

	while((uint32_t)(HAL_GetTick() - start_tick) < timeout)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, &data) == 0)
		{
			Driver_Net_AppendReplyByte(buf, sizeof(buf), &length, data);

			for(r = 0; r < (sizeof(xSuccessReplies) / sizeof(xSuccessReplies[0])); r++)
			{
				if(strstr(buf, xSuccessReplies[r]) != 0)
				{
					return 0;
				}
			}

			for(r = 0; r < (sizeof(xFailReplies) / sizeof(xFailReplies[0])); r++)
			{
				if(strstr(buf, xFailReplies[r]) != 0)
				{
					printf("[NET] CWJAP reply: %s\r\n", buf);
					return -1;
				}
			}
		}
		else
		{
			Driver_Net_WaitForRxActivity(1);
		}
	}

	printf("[NET] CWJAP raw timeout.\r\n");
	return -1;
}

static int Driver_Net_HasValidStaIp(uint16_t timeout)
{
	uint8_t data = 0U;
	char buf[192] = {0};
	uint16_t length = 0U;
	uint32_t start_tick = 0U;

	Driver_Net_RegisterCurrentTask();
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)"AT+CIFSR\r\n", strlen("AT+CIFSR\r\n"), 500);
	Driver_Net_ClearWaitNotification();
	start_tick = HAL_GetTick();

	while((uint32_t)(HAL_GetTick() - start_tick) < timeout)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, &data) == 0)
		{
			Driver_Net_AppendReplyByte(buf, sizeof(buf), &length, data);
			if(strstr(buf, "STAIP,\"0.0.0.0\"") != 0)
			{
				return -1;
			}
			if(strstr(buf, "STAIP,\"") != 0)
			{
				return 0;
			}
		}
		else
		{
			Driver_Net_WaitForRxActivity(1);
		}
	}

	return -1;
}

static int Driver_Net_UART_Init(void)
{
	/* 鍒濆鍖?USART2锛堣繛鎺?ESP8266锛夈€?*/
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
	/* 寮€鍚?RXNE 涓柇锛岄€愬瓧鑺傛帴鏀讹紝闄嶄綆鏁版嵁涓㈠け椋庨櫓銆?*/
	__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

	return 0;
}

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart)
{
	/* 閰嶇疆 USART2 鐨?GPIO 涓?NVIC銆?*/
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(huart->Instance == USART2)
	{
		__HAL_RCC_USART2_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();

		/* USART2_TX -> PA2, F407 AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_2;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		/* USART2_RX -> PA3, F407 AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_3;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		HAL_NVIC_SetPriority(USART2_IRQn, NET_UART_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(USART2_IRQn);
	}
}

static uint8_t Driver_Net_TakeRxFault(void)
{
	uint8_t pending = 0U;

	taskENTER_CRITICAL();
	pending = s_net_rx_fault_pending;
	s_net_rx_fault_pending = 0U;
	taskEXIT_CRITICAL();

	return pending;
}

static void Driver_Net_ResetRxStream(void)
{
	taskENTER_CRITICAL();
	(void)Driver_Buffer_Clean(&NetDataBuffer);
	s_net_rx_fault_pending = 0U;
	s_net_parser_reset_requested = 1U;
	taskEXIT_CRITICAL();
}

static void Driver_Net_AppendReplyByte(char *buf,
	uint16_t capacity,
	uint16_t *length,
	uint8_t data)
{
	uint16_t keep = 0U;

	if(buf == NULL || length == NULL || capacity < 2U)
	{
		return;
	}

	if(data == 0U)
	{
		*length = 0U;
		buf[0] = '\0';
		return;
	}

	if(*length >= (uint16_t)(capacity - 1U))
	{
		keep = (uint16_t)((capacity - 1U) / 2U);
		memmove(buf, &buf[*length - keep], keep);
		*length = keep;
	}

	buf[*length] = (char)data;
	(*length)++;
	buf[*length] = '\0';
}

#if 0
static int Driver_Net_UART_Init(void)
{
	/* 鍒濆鍖?USART2锛堣繛鎺?ESP8266锛夈€?*/
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
	/* 寮€鍚?RXNE 涓柇锛岄€愬瓧鑺傛帴鏀讹紝闄嶄綆鏁版嵁涓㈠け椋庨櫓銆?*/
	__HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

	return 0;
}

static void HAL_UART2_MspInit(UART_HandleTypeDef *huart)
{
	/* 閰嶇疆 USART2 鐨?GPIO 涓?NVIC銆?*/
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(huart->Instance == USART2)
	{
		__HAL_RCC_USART2_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();

		/* USART2_TX -> PA2, F407 AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_2;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		/* USART2_RX -> PA3, F407 AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_3;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		HAL_NVIC_SetPriority(USART2_IRQn, NET_UART_IRQ_PRIORITY, 0);
		HAL_NVIC_EnableIRQ(USART2_IRQn);
	}
}

#endif
void USART2_IRQHandler(void)
{
	/* 串口接收中断：保存 AT 流 + 解析 +IPD + 通知等待任务。 */
	uint8_t rx_data = 0;
	uint32_t status = USART2->SR;
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if((status & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
	{
		/* Reading SR followed by DR clears RXNE and ORE without consuming DR twice. */
		rx_data = (uint8_t)(USART2->DR & 0xFFU);
		if((status & USART_SR_ORE) != 0U)
		{
			s_net_payload_drop_count++;
			s_net_rx_fault_pending = 1U;
			s_net_parser_reset_requested = 1U;
		}

		if((status & USART_SR_RXNE) != 0U)
		{
			/* Keep TCP payload out of the AT reply matcher. */
			if(NetDataProcess_Callback(rx_data) == 0U)
			{
				(void)Driver_Buffer_Write(&CMDRetBuffer, rx_data);
			}

		}

		if(xNetWaitTaskHandle != NULL)
		{
			vTaskNotifyGiveFromISR(xNetWaitTaskHandle, &xHigherPriorityTaskWoken);
		}
	}

	portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static int Driver_Net_TransmitCmd(const char *cmd, const char *reply, uint16_t timeout)
{
	/* 鍙戦€?AT 鎸囦护锛屽苟绛夊緟鎸囧畾搴旂瓟瀛楃涓层€?*/
	char buf[128] = {0};
	int written = 0;
	int ret = -1;

	if(cmd == NULL || reply == NULL || timeout == 0U)
	{
		return -1;
	}

	Driver_Net_RegisterCurrentTask();
	if(strstr(cmd, "\r\n") == NULL)
	{
		written = snprintf(buf, sizeof(buf), "%s\r\n", cmd);
	}
	else
	{
		written = snprintf(buf, sizeof(buf), "%s", cmd);
	}
	if(written < 0 || written >= (int)sizeof(buf))
	{
		return -1;
	}

	Driver_Buffer_Clean(&CMDRetBuffer);
	if(HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500) != HAL_OK)
	{
		return -1;
	}

	ret = Driver_Net_WaitForReply(reply, timeout);
	return ret;
}

int Driver_Net_TransmitSocket(const char *socket, int len, int timeout)
{
	/* 涓ら樁娈靛彂閫侊細鍏?CIPSEND 鑾峰彇 '>'锛屽啀鍙戝疄闄呮暟鎹瓑寰?SEND OK銆?*/
	char cmd[16] = {0};
	int written = 0;
	int ret = -1;

	if(socket == NULL || len <= 0 || len > 0xFFFF || timeout <= 0)
	{
		return -1;
	}

	Driver_Net_RegisterCurrentTask();

	/* Stage 1: request the ESP8266 transmit prompt ('>'). */
	written = snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d\r\n", len);
	if(written < 0 || written >= (int)sizeof(cmd))
	{
		return -1;
	}
	Driver_Buffer_Clean(&CMDRetBuffer);
	if(HAL_UART_Transmit(&huart2, (uint8_t *)cmd, strlen(cmd), 500) != HAL_OK)
	{
		return -1;
	}

	if(Driver_Net_WaitForReply(">", timeout) != 0)
	{
		return -1;
	}

	/* Stage 2: push the actual payload and wait for SEND OK. */
	Driver_Buffer_Clean(&CMDRetBuffer);
	if(HAL_UART_Transmit(&huart2, (uint8_t *)socket, (uint16_t)len, 500) != HAL_OK)
	{
		return -1;
	}

	ret = Driver_Net_WaitForReply("SEND OK", timeout);
	return ret;
}

int Driver_Net_RecvSocket(char *buf, int len, int timeout)
{
	/* 浠庣函鏁版嵁缂撳啿鍖烘寜鐩爣闀垮害璇诲彇锛岃秴鏃惰繑鍥炴湭瀹屾垚鐘舵€併€?*/
	uint32_t start_tick = 0U;

	if(buf == NULL || len <= 0 || len > 0xFFFF)
	{
		return -1;
	}
	if(timeout <= 0)
	{
		return 1;
	}

	Driver_Net_RegisterCurrentTask();
	Driver_Net_ClearWaitNotification();
	start_tick = HAL_GetTick();

	while((uint32_t)(HAL_GetTick() - start_tick) < (uint32_t)timeout)
	{
		if(Driver_Net_TakeRxFault() != 0U)
		{
			(void)Driver_Buffer_Clean(&NetDataBuffer);
			return -1;
		}

		if(Driver_Buffer_GetUsed(&NetDataBuffer) >= (uint16_t)len)
		{
			return (Driver_Buffer_ReadBytes(&NetDataBuffer,
				(uint8_t *)buf, (uint16_t)len) == len) ? 0 : -1;
		}

		Driver_Net_WaitForRxActivity(1);
	}

	return 1;
}

int Driver_Net_ConnectWiFi(const char *ssid, const char *pwd, int timeout)
{
	/* fix: buf 从 64 扩大到 128，SSID(最镳32B)+密码(最镳64B)+固定字符 > 64，
	 * 原来指针源头 buf[64] 会栈溢出，改用 snprintf 拼字符串。 */
	char buf[128];
	int written = 0;

	if(ssid == NULL || pwd == NULL || timeout <= 0)
	{
		return -1;
	}

	Driver_Net_RegisterCurrentTask();

	written = snprintf(buf, sizeof(buf), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
	if(written < 0 || written >= (int)sizeof(buf))
	{
		return -1;
	}

	printf("[NET] Joining WiFi: %s\r\n", ssid);
	Driver_Buffer_Clean(&CMDRetBuffer);
	if(HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500) != HAL_OK)
	{
		return -1;
	}

	if(Driver_Net_WaitForWiFiJoin(timeout) == 0)
	{
		return 0;
	}

	/* Some firmware builds report WiFi status through CIFSR after a long join.
	 * Treat it as success only if a non-zero STA IP is assigned. */
	if(Driver_Net_HasValidStaIp(3000) == 0)
	{
		return 0;
	}

	printf("[NET] CWJAP timeout or failed.\r\n");
	return -1;
}

int Driver_Net_DisconnectWiFi(void)
{
	return Driver_Net_TransmitCmd("AT+CWQAP", "OK\r\n", 500);
}

int Driver_Net_ConnectTCP(const char *ip, int port, int timeout)
{
	/* 鍏堥厤缃崟杩炴帴涓庢櫘閫氫紶杈撴ā寮忥紝鍐嶅彂璧?TCP 杩炴帴銆?*/
	char buf[128] = {0};
	int written = 0;
	int ret = -1;

	if(ip == NULL || port <= 0 || port > 65535 || timeout <= 0)
	{
		return -1;
	}

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

	written = snprintf(buf, sizeof(buf), "AT+CIPSTART=\"TCP\",\"%s\",%d\r\n", ip, port);
	if(written < 0 || written >= (int)sizeof(buf))
	{
		return -1;
	}
	Driver_Net_RegisterCurrentTask();

	Driver_Buffer_Clean(&CMDRetBuffer);
	if(HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500) != HAL_OK)
	{
		return -1;
	}
	ret = Driver_Net_WaitForTcpConnect(timeout);
	if(ret == 0)
	{
		Driver_Net_ResetRxStream();
	}
	return ret;
}

int Driver_Net_Disconnect_TCP_UDP(void)
{
	return Driver_Net_TransmitCmd("AT+CIPCLOSE", "OK\r\n", 500);
}

typedef enum AT_STATUS{
	/* 瑙ｆ瀽 +IPD 鐨勭姸鎬佹満锛氬抚澶?-> 闀垮害 -> 鏁版嵁浣撱€?*/
	INIT_STATUS,
	LEN_STATUS,
	DATA_STATUS,
	DISCARD_STATUS
}AT_STATUS;

static uint8_t g_DataBuff[512] = {0};

static uint8_t NetDataProcess_Callback(uint8_t data)
{
	/* 浠庝覆鍙ｅ瓧鑺傛祦涓彁鍙?+IPD,<len>:<data> 閲岀殑 data 閮ㄥ垎銆?*/
	static const char prefix[] = "+IPD,";
	static AT_STATUS g_status = INIT_STATUS;
	static uint8_t prefix_index = 0U;
	static uint8_t length_digits = 0U;
	static uint32_t data_index = 0U;
	static uint32_t data_len = 0U;

	if(s_net_parser_reset_requested != 0U)
	{
		g_status = INIT_STATUS;
		prefix_index = 0U;
		length_digits = 0U;
		data_index = 0U;
		data_len = 0U;
		s_net_parser_reset_requested = 0U;
	}

	switch(g_status)
	{
		case INIT_STATUS:
		{
			if(data == (uint8_t)prefix[prefix_index])
			{
				prefix_index++;
				if(prefix_index == (sizeof(prefix) - 1U))
				{
					prefix_index = 0U;
					length_digits = 0U;
					data_index = 0U;
					data_len = 0U;
					g_status = LEN_STATUS;
				}
			}
			else
			{
				prefix_index = (data == (uint8_t)prefix[0]) ? 1U : 0U;
			}
			return 0U;
		}

		case LEN_STATUS:
		{
			if(data >= (uint8_t)'0' && data <= (uint8_t)'9' && length_digits < 5U)
			{
				data_len = (data_len * 10U) + (uint32_t)(data - (uint8_t)'0');
				length_digits++;
				return 0U;
			}

			if(data == (uint8_t)':' && length_digits != 0U && data_len != 0U)
			{
				data_index = 0U;
				if(data_len <= sizeof(g_DataBuff))
				{
					g_status = DATA_STATUS;
				}
				else
				{
					g_status = DISCARD_STATUS;
					s_net_payload_drop_count++;
				}
				return 0U;
			}

			g_status = INIT_STATUS;
			prefix_index = (data == (uint8_t)prefix[0]) ? 1U : 0U;
			length_digits = 0U;
			data_index = 0U;
			data_len = 0U;
			return 0U;
		}

		case DATA_STATUS:
		{
			/* The length gate above keeps this write within g_DataBuff. */
			g_DataBuff[data_index] = data;
			data_index++;
			if(data_index == data_len)
			{
				if(Driver_Buffer_WriteBytes(&NetDataBuffer,
					g_DataBuff, (uint16_t)data_len) != (int)data_len)
				{
					s_net_payload_drop_count++;
					s_net_rx_fault_pending = 1U;
				}
				g_status = INIT_STATUS;
				length_digits = 0U;
				data_index = 0U;
				data_len = 0U;
			}
			return 1U;
		}

		case DISCARD_STATUS:
		{
			data_index++;
			if(data_index >= data_len)
			{
				g_status = INIT_STATUS;
				length_digits = 0U;
				data_index = 0U;
				data_len = 0U;
				s_net_rx_fault_pending = 1U;
			}
			return 1U;
		}

		default:
			g_status = INIT_STATUS;
			prefix_index = 0U;
			length_digits = 0U;
			data_index = 0U;
			data_len = 0U;
			return 0U;
	}
}

uint32_t Driver_Net_GetRxDropCount(void)
{
	return s_net_payload_drop_count;
}

int Driver_Net_Init(void)
{
	Driver_Net_RegisterCurrentTask();

	if(g_net_driver_inited != 0U)
	{
		return 0;
	}

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
	Driver_Net_TaskDelay(2000);

	if(Driver_Net_TransmitCmd("AT+CWMODE=1", "OK\r\n", 500) != 0)
	{
		return -1;
	}

	g_net_driver_inited = 1U;
	return 0;
}


