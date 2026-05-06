/**
 * @file driver_hal_timebase.c
 * @brief 使用 TIM2 重载 HAL 系统节拍，并在 1ms 周期内执行按键消抖处理
 */

#include "stm32f4xx_hal.h"
/* 按键消抖处理回调，在每个 Tick 中调用一次。 */
extern void KeyShakeProcess_Callback(void);
/* HAL Tick 使用的基础定时器句柄。 */
static TIM_HandleTypeDef htim2;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	/* 初始化 TIM2 为 1MHz 计数后 1ms 更新中断（1kHz Tick）。 */
	uint32_t uwTimclock = 0;
	uint32_t uwPrescalerValue = 0;
	
	__HAL_RCC_TIM2_CLK_ENABLE();
	
	HAL_NVIC_SetPriority(TIM2_IRQn, TickPriority, 0);
	HAL_NVIC_EnableIRQ(TIM2_IRQn);
	
	uwTimclock = HAL_RCC_GetPCLK1Freq() * 2;
	uwPrescalerValue = uwTimclock / 1000000;
	
	htim2.Instance = TIM2;
	htim2.Init.Prescaler = uwPrescalerValue - 1;
	htim2.Init.Period = 1000 - 1;
	htim2.Init.ClockDivision = 0;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	if(HAL_TIM_Base_Init(&htim2) == HAL_OK)
	{
		/* 启动更新中断，HAL_IncTick 将在回调里执行。 */
		return HAL_TIM_Base_Start_IT(&htim2);
	}
	return HAL_ERROR;
}

void HAL_SuspendTick(void)
{
	/* 关闭 TIM2 更新中断，暂停系统 Tick。 */
	__HAL_TIM_DISABLE_IT(&htim2, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
	/* 恢复 TIM2 更新中断，继续系统 Tick。 */
	__HAL_TIM_ENABLE_IT(&htim2, TIM_IT_UPDATE);
}

void TIM2_IRQHandler(void)
{
	/* TIM2 中断入口，交给 HAL 统一处理。 */
	HAL_TIM_IRQHandler(&htim2);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	(void)htim;
	/* 每 1ms 递增 HAL 全局 Tick。 */
	HAL_IncTick();
	/* 复用 Tick 节拍执行按键消抖状态机。 */
	KeyShakeProcess_Callback();
}
