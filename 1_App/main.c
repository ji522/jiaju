/**
 * @file main.c
 * @brief 智能家居应用程序入口文件
 */

#include "main.h"

#include "dev_io.h"

#include "FreeRTOS.h"
#include "task.h"

void SystemClock_Config(void);

/* 记录 FreeRTOS 异常原因，便于调试器或串口定位故障。 */
volatile const char *g_pcFreeRTOSFaultReason = NULL;
/* 当发生栈溢出时，记录出问题的任务名称。 */
volatile const char *g_pcFreeRTOSFaultTaskName = NULL;

/* 外部任务启动接口，分别负责网络通信、LED 控制与按键处理。 */
extern void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartLEDTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);
extern void vStartKeyTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority);

int main(void)
{
	/* 调试串口设备对象，用于系统启动后的日志输出。 */
	ptIODev dbgoutDev = NULL;

	/* 初始化 HAL 库和系统时钟，为后续外设与 RTOS 运行做准备。 */
	HAL_Init();
	SystemClock_Config();

	/* 初始化调试输出设备，便于后续通过串口查看运行日志。 */
	dbgoutDev = IODev_GetDev(DBGOUT);
	if(dbgoutDev != NULL)
	{
		dbgoutDev->Init(dbgoutDev);
	}

	/* 按优先级创建各应用任务。 */
	vStartMQTTTasks(512, 10);
	vStartLEDTasks(128, 1);
	vStartKeyTasks(128, 2);

	/* 启动 FreeRTOS 调度器，之后系统将交由各任务协同运行。 */
	vTaskStartScheduler();

	while(1)
	{
		/* 正常情况下不会执行到这里，除非调度器启动失败。 */
	}
}

void vApplicationMallocFailedHook(void)
{
	/* 动态内存申请失败时进入该钩子函数，记录原因并停机等待排查。 */
	g_pcFreeRTOSFaultReason = "malloc failed";
	taskDISABLE_INTERRUPTS();
	for(;;)
	{
	}
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	(void)xTask;
	/* 任务栈溢出时保存故障信息，便于调试具体是哪个任务出了问题。 */
	g_pcFreeRTOSFaultReason = "stack overflow";
	g_pcFreeRTOSFaultTaskName = pcTaskName;
	taskDISABLE_INTERRUPTS();
	for(;;)
	{
	}
}

void SystemClock_Config(void)
{
	/* 配置 F407 时钟树：HSE 8MHz → PLL → 168MHz SYSCLK */
	RCC_OscInitTypeDef RCC_OscInitStruct = {0};
	RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

	__HAL_RCC_PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	/* 使能 HSE，配置 PLL：PLLM=8, PLLN=336, PLLP=2 → 8/8*336/2=168MHz */
	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLM = 8;
	RCC_OscInitStruct.PLL.PLLN = 336;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 4;
	HAL_RCC_OscConfig(&RCC_OscInitStruct);

	/* HCLK=168MHz, PCLK1=42MHz(APB1 max), PCLK2=84MHz(APB2 max) */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
		RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

	HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}
