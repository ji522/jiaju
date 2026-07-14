/**
 * @file driver_led_key.c
 * @brief LED 与按键底层驱动
 */

#include "driver_led_key.h"
#include "driver_buffer.h"
#include "FreeRTOS.h"
#include "task.h"

/* 按键任务句柄（由应用层定义），用于在中断里通知按键任务处理事件。 */
extern TaskHandle_t keyTaskHandle;

/* 按键事件环形缓冲区，用于在中断与任务之间传递按键数据。 */
static RingBuffer KeyBuffer;
/* 按键触发后的目标时间戳（用于消抖判断）。 */
volatile static uint32_t KeyTrigerTime = 0;

int Driver_LED_Init(void)
{
	/* 配置 LED GPIO 为推挽输出。 */
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	__HAL_RCC_GPIOF_CLK_ENABLE();

	GPIO_InitStruct.Pin = LED_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;

	HAL_GPIO_Init(LED_PORT, &GPIO_InitStruct);
	return 0;
}

int Driver_LED_WriteStatus(uint8_t status)
{
	/* 通过宏控制 LED 亮灭状态。 */
	LED(status);
	return 0;
}

int Driver_Key_Init(void)
{
	/* 初始化按键缓冲区与 GPIO 外部中断。 */
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	if(Driver_Buffer_Init(&KeyBuffer, sizeof(KeyEvent) << 4) != 0)
	{
		return -1;
	}

	__HAL_RCC_GPIOA_CLK_ENABLE();

	GPIO_InitStruct.Pin = KEY_PIN;
	GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
	GPIO_InitStruct.Pull = GPIO_PULLUP;

	HAL_GPIO_Init(KEY_PORT, &GPIO_InitStruct);

	/* fix: 优先级从 0 改为 5，确保不超出 FreeRTOS configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY。
	 * 在回调里调用了 vTaskNotifyGiveFromISR()，优先级 0 会触发 FreeRTOS 断言。 */
	HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(EXTI0_IRQn);

	return 0;
}

void EXTI0_IRQHandler(void)
{
	/* 按键 EXTI0 中断入口，交给 HAL 分发到回调。 */
	HAL_GPIO_EXTI_IRQHandler(KEY_PIN);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	(void)GPIO_Pin;
	/* 记录 50ms 后的时间点，交由周期任务做消抖确认。 */
	KeyTrigerTime = HAL_GetTick() + 50;
}

int Driver_Key_Read(uint8_t *buf, uint16_t len)
{
	/* 参数合法性校验：长度需按 KeyEvent 对齐。 */
	if(len == 0 || len < sizeof(KeyEvent) || (len % sizeof(KeyEvent) != 0))
	{
		return -1;
	}
	if(buf == NULL)
	{
		return -1;
	}

	if(Driver_Buffer_ReadBytes(&KeyBuffer, buf, len) == (int)len)
	{
		/* 读取成功。 */
		return 0;
	}

	return -1;
}

void KeyShakeProcess_Callback(void)
{
	/* 按键消抖与按压时长统计逻辑。 */
	KeyEvent nKeyEvent = {0};
	static uint32_t press_time = 0;
	static uint32_t release_time = 0;
	uint32_t tick = HAL_GetTick();

	/* fix: 改为 >= 并加 !=0 守卫，避免精确相等只有1ms 时间窗口导致按键丢失。 */
	if(KeyTrigerTime != 0 && tick >= KeyTrigerTime)
	{
		if(KEY_STATUE() == 0)
		{
			/* 按下沿稳定后记录按下时刻。 */
			press_time = tick;
		}
		else
		{
			/* 松开沿稳定后记录松开时刻。 */
			release_time = tick;
		}

		if(press_time != 0 && release_time != 0)
		{
			BaseType_t xHigherPriorityTaskWoken = pdFALSE;

			/* 生成一次按键事件：按键号 + 按压时长。 */
			nKeyEvent.num = 1;
			nKeyEvent.time = release_time - press_time;

			release_time = 0;
			press_time = 0;

			if(Driver_Buffer_WriteBytes(&KeyBuffer, (uint8_t*)&nKeyEvent, sizeof(KeyEvent)) == sizeof(KeyEvent) &&
				keyTaskHandle != NULL)
			{
				/* 在中断上下文通知按键任务读取事件。 */
				vTaskNotifyGiveFromISR(keyTaskHandle, &xHigherPriorityTaskWoken);
				portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
			}
		}
	}
}
