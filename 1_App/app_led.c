/**
 * @file app_led.c
 * @brief 智能家居 LED 控制任务
 */

#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#include "dev_io.h"

/* LED 任务句柄，供其他任务通过任务通知方式控制灯的状态。 */
TaskHandle_t ledTaskHandle;

void LedTask(void *parameter)
{
	/* 保存任务通知携带的控制值，例如 0 表示关灯，1 表示开灯。 */
	uint32_t notify_value = 0;
	/* 从设备层获取 LED 设备对象，通过统一接口完成初始化和写操作。 */
	ptIODev ledDev = IODev_GetDev(LED);

	(void)parameter;

	/* 若没有找到 LED 设备，说明底层设备未注册成功，挂起任务等待排查。 */
	if(ledDev == NULL)
	{
		printf("LED Device not found.\r\n");
		for(;;)
		{
			vTaskSuspend(NULL);
		}
	}

	/* 初始化 LED 硬件，通常会配置 GPIO 输出模式等底层资源。 */
	ledDev->Init(ledDev);

	while(1)
	{
		/* 阻塞等待其他任务发送通知，收到后根据通知值更新 LED 状态。 */
		if(xTaskNotifyWait(0, 0xFFFFFFFF, &notify_value, portMAX_DELAY) == pdTRUE)
		{
			printf("LED Task notify value is %d\r\n", notify_value);
			/* 将通知值写入设备层，由驱动完成具体的亮灭控制。 */
			ledDev->write(ledDev, (uint8_t*)&notify_value, 1);
		}
	}
}

void vStartLEDTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	/* 创建 LED 控制任务，使其常驻等待来自网络层或其他业务层的灯控命令。 */
	if(xTaskCreate(LedTask,
			"LED",
			usTaskStackSize,
			(void *)x,
			uxTaskPriority,
			&ledTaskHandle) == pdPASS)
	{
		printf("Create LED Task success.\r\n");
	}
	else
	{
		printf("Create LED Task failed.\r\n");
	}
}
