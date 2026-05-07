/**
 * @file app_key.c
 * @brief 智能家居按键事件任务
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "dev_io.h"
#include <stdio.h>

#define QUEUE_LENGTH 5
#define QUEUE_ITEM_SIZE sizeof(KeyEvent)

/* 按键扫描任务向其他任务转发事件时使用的消息队列。 */
QueueHandle_t xKeyQueue;
/* 按键任务句柄，便于其他模块通过任务通知唤醒本任务。 */
TaskHandle_t keyTaskHandle;

void KeyTask(void *parameter)
{
	KeyEvent key = {0};
	/* 从设备层获取按键设备对象，后续通过统一接口完成初始化和读取。 */
	ptIODev keyDev = IODev_GetDev(KEY);

	(void)parameter;
	printf("[KEY] Task started\r\n");

	/* 创建按键事件队列，供网络任务等上层业务读取按键数据。 */
	xKeyQueue = xQueueCreate(QUEUE_LENGTH, QUEUE_ITEM_SIZE);
	if(xKeyQueue == NULL)
	{
		printf("Create Key Queue failed.\r\n");
		for(;;)
		{
			vTaskSuspend(NULL);
		}
	}

	/* 若设备未注册成功，则挂起当前任务并等待排查。 */
	if(keyDev == NULL)
	{
		printf("Key Device not found.\r\n");
		for(;;)
		{
			vTaskSuspend(NULL);
		}
	}

	/* 初始化按键硬件，通常会完成 GPIO、外部中断或定时扫描相关配置。 */
	keyDev->Init(keyDev);
	printf("[KEY] Device init OK\r\n");

	while(1)
	{
		/* 等待消抖逻辑通知有新的按键事件到来。 */
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		printf("[KEY] Notify received\r\n");

		/* 将驱动缓冲区中已经生成的按键事件全部取出，再统一发送到消息队列。 */
		while(keyDev->Read(keyDev, (uint8_t*)&key, sizeof(KeyEvent)) == 0)
		{
			printf("[KEY] Event: num=%u press_ms=%u\r\n",
				(unsigned)key.num, (unsigned)key.time);
			if(xQueueSendToBack(xKeyQueue, (uint8_t*)&key, pdMS_TO_TICKS(10)) != pdPASS)
			{
				printf("Key Queue Send full.\r\n");
			}
			else
			{
				printf("[KEY] Event queued for MQTT\r\n");
			}
		}
	}
}

void vStartKeyTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L;

	/* 创建按键任务，使其负责接收底层按键事件并转发给应用层。 */
	if(xTaskCreate(KeyTask,
			"Key",
			usTaskStackSize,
			(void *)x,
			uxTaskPriority,
			&keyTaskHandle) == pdPASS)
	{
		printf("Create Key Task success.\r\n");
	}
	else
	{
		printf("Create Key Task failed.\r\n");
	}
}
