/**
 * @file driver_dbg.c
 * @brief 智能家居项目 - 串口调试打印的底层硬件驱动实现（F407 适配版）
 * @note 负责初始化 USART1 以及通过重定义 fputc 实现对标准库 printf 的对接。
 */

#include "driver_dbg.h"
#include "stdio.h"

void HAL_UART1_MspInit(UART_HandleTypeDef *huart);

/* 对应 USART1 的全局操作句柄 */
static UART_HandleTypeDef huart1;

int Driver_DBG_Init(void)
{
	huart1.Instance = USART1;
	huart1.Init.BaudRate = 115200;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_TX_RX;
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;

	HAL_UART1_MspInit(&huart1);

	if(HAL_UART_Init(&huart1) != HAL_OK)
	{
		return -1;
	}

	return 0;
}

void HAL_UART1_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(huart->Instance == USART1)
	{
		__HAL_RCC_USART1_CLK_ENABLE();
		__HAL_RCC_GPIOA_CLK_ENABLE();

		/* PA9: USART1_TX, AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_9;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

		/* PA10: USART1_RX, AF7 */
		GPIO_InitStruct.Pin = GPIO_PIN_10;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
	}
}

struct __FILE
{
	int handle;
};

FILE __stdout;

int fputc(int ch, FILE *f)
{
	HAL_UART_Transmit(&huart1, (uint8_t*)&ch, 1, 0xffff);
	return ch;
}
