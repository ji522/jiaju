/**
 * @file driver_dbg.c
 * @brief 智能家居项目 - 串口调试打印的底层硬件驱动实现
 * @note 负责初始化 USART1 以及通过重定义 fputc 实现对标准库 printf 的对接。
 */

#include "driver_dbg.h"
#include "stdio.h"

/* 前置声明：由于 HAL 库回调特点，MSP 的配置要在 Init 中被间接调用 */
void HAL_UART1_MspInit(UART_HandleTypeDef *huart);

/* 对应 USART1 的全局操作句柄 */
static UART_HandleTypeDef huart1;

/**
 * @brief 配置并初始化串口1作为控制台日志输出口
 * @return 0 表示无报错
 */
int Driver_DBG_Init(void)
{
	/* 选择串口 1（挂载在 APB2 上） */
	huart1.Instance = USART1;
	/* 配置通讯标准：115200 8位数据 不足偶校验 1个停止位 */
	huart1.Init.BaudRate = 115200;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_TX_RX;          /* 开启收和发模式 */
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE; /* 关硬件流控 */
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;
	
	/* 调用引脚和时钟初始化函数 */
	HAL_UART1_MspInit(&huart1);
	
	/* 将配置真正应用到单片机底层寄存器中 */
	if(HAL_UART_Init(&huart1) != HAL_OK)
	{
		return -1;
	}
	
	return 0;
}

/**
 * @brief STM32 HAL库专用：底层引脚及时钟回调初始化配置
 * @param huart 触发此回调的当前串口句柄
 */
void HAL_UART1_MspInit(UART_HandleTypeDef *huart)
{
	GPIO_InitTypeDef GPIO_InitStruct = {0};
	
	if(huart->Instance == USART1)
	{
		/* 1. 使能 USART1 外设时钟 */
		__HAL_RCC_USART1_CLK_ENABLE();
		
		/* 2. 使能复用引脚所在的 GPIOA 时钟 */
		__HAL_RCC_GPIOA_CLK_ENABLE();
		
		/* 3. 引脚映射：TX 为 PA9, RX 为 PA10 */
		
		/* 配置 PA9 为复用推挽输出 (TX 发送线) */
		GPIO_InitStruct.Pin = GPIO_PIN_9;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
		GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
		
		/* 配置 PA10 为悬浮或上拉输入 (RX 接收线) */
		GPIO_InitStruct.Pin = GPIO_PIN_10;
		GPIO_InitStruct.Mode = GPIO_MODE_AF_INPUT;
		GPIO_InitStruct.Pull = GPIO_PULLUP;
		HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
	}
}

/* --------------- 以下为重写 C 库底层进行 printf 挂载 --------------- */

struct __FILE
{
	int handle;
};

FILE __stdout;

/**
 * @brief 接管标准库 C 的底层单字符输出源
 * @note 当应用层代码调用 printf("%s", str) 时，库函数会解析成逐个字符，在这里用单片机串口喷发出去
 */
int fputc(int ch, FILE *f)
{
	/* 调用 HAL 发送一个字节，阻塞时限 0xffff 足够长 */
	HAL_UART_Transmit(&huart1, (uint8_t*)&ch, 1, 0xffff);
	return ch;
}
