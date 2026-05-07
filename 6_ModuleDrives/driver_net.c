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
/* 閫愬瓧鑺傝В鏋?ESP8266 鍥炲寘涓殑 +IPD 鏁版嵁甯с€?*/
void NetDataProcess_Callback(uint8_t data);

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
				/* 鎵惧埌鐩爣搴旂瓟銆?*/
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

static int Driver_Net_WaitForAnyReply(const char *const replies[], uint8_t reply_count, uint16_t timeout)
{
	uint8_t i = 0;
	uint8_t r = 0;
	char buf[128] = {0};

	if(replies == NULL || reply_count == 0 || timeout == 0)
	{
		return -1;
	}

	Driver_Net_ClearWaitNotification();

	while(timeout != 0)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, (uint8_t*)&buf[i]) == 0)
		{
			i = (i + 1) % sizeof(buf);
			for(r = 0; r < reply_count; r++)
			{
				if(replies[r] != NULL && strstr(buf, replies[r]) != 0)
				{
					return 0;
				}
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
	uint8_t i = 0;
	uint8_t r = 0;
	char buf[192] = {0};

	Driver_Net_ClearWaitNotification();

	while(timeout != 0)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, (uint8_t*)&buf[i]) == 0)
		{
			i = (i + 1) % sizeof(buf);

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
			timeout--;
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
	uint8_t i = 0;
	uint8_t r = 0;
	char buf[192] = {0};

	Driver_Net_ClearWaitNotification();

	while(timeout != 0)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, (uint8_t*)&buf[i]) == 0)
		{
			i = (i + 1) % sizeof(buf);

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
			timeout--;
			Driver_Net_WaitForRxActivity(1);
		}
	}

	printf("[NET] CWJAP raw timeout.\r\n");
	return -1;
}

static int Driver_Net_HasValidStaIp(uint16_t timeout)
{
	char buf[192] = {0};
	uint8_t i = 0;

	Driver_Net_RegisterCurrentTask();
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)"AT+CIFSR\r\n", strlen("AT+CIFSR\r\n"), 500);
	Driver_Net_ClearWaitNotification();

	while(timeout != 0)
	{
		if(Driver_Buffer_Read(&CMDRetBuffer, (uint8_t *)&buf[i]) == 0)
		{
			i = (i + 1) % sizeof(buf);
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
			timeout--;
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

void USART2_IRQHandler(void)
{
	/* 涓插彛鎺ユ敹涓柇锛氫繚瀛?AT 娴?+ 瑙ｆ瀽 +IPD + 閫氱煡绛夊緟浠诲姟銆?*/
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
	/* 鍙戦€?AT 鎸囦护锛屽苟绛夊緟鎸囧畾搴旂瓟瀛楃涓层€?*/
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
	/* 涓ら樁娈靛彂閫侊細鍏?CIPSEND 鑾峰彇 '>'锛屽啀鍙戝疄闄呮暟鎹瓑寰?SEND OK銆?*/
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
	/* 浠庣函鏁版嵁缂撳啿鍖烘寜鐩爣闀垮害璇诲彇锛岃秴鏃惰繑鍥炴湭瀹屾垚鐘舵€併€?*/
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
	/* 缁勫寘骞跺彂閫佽繛鎺?WiFi 鎸囦护銆?*/
	char buf[64] = "AT+CWJAP=\"";

	strcat(buf, ssid);
	strcat(buf, "\",\"");
	strcat(buf, pwd);
	strcat(buf, "\"");

	Driver_Net_RegisterCurrentTask();

	if(strstr(buf, "\r\n") == NULL)
	{
		strcat(buf, "\r\n");
	}

	printf("[NET] Joining WiFi: %s\r\n", ssid);
	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500);

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
	Driver_Net_RegisterCurrentTask();

	if(strstr(buf, "\r\n") == NULL)
	{
		strcat(buf, "\r\n");
	}

	Driver_Buffer_Clean(&CMDRetBuffer);
	HAL_UART_Transmit(&huart2, (uint8_t *)buf, strlen(buf), 500);
	return Driver_Net_WaitForTcpConnect(timeout);
}

int Driver_Net_Disconnect_TCP_UDP(void)
{
	return Driver_Net_TransmitCmd("AT+CIPCLOSE", "OK\r\n", 500);
}

typedef enum AT_STATUS{
	/* 瑙ｆ瀽 +IPD 鐨勭姸鎬佹満锛氬抚澶?-> 闀垮害 -> 鏁版嵁浣撱€?*/
	INIT_STATUS,
	LEN_STATUS,
	DATA_STATUS
}AT_STATUS;

static uint8_t g_DataBuff[256] = {0};

void NetDataProcess_Callback(uint8_t data)
{
	/* 浠庝覆鍙ｅ瓧鑺傛祦涓彁鍙?+IPD,<len>:<data> 閲岀殑 data 閮ㄥ垎銆?*/
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


