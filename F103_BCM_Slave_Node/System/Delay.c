#include "stm32f10x.h"
#include "Delay.h"

static volatile uint32_t s_tick_ms = 0U;

#define DWT_CTRL_REG     (*(volatile uint32_t *)0xE0001000UL)
#define DWT_CYCCNT_REG   (*(volatile uint32_t *)0xE0001004UL)

void Delay_Init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT_CYCCNT_REG = 0U;
	DWT_CTRL_REG |= 1UL;
	(void)SysTick_Config(SystemCoreClock / 1000U);
}

void Delay_TickISR(void)
{
	s_tick_ms++;
}

uint32_t Delay_GetTickMs(void)
{
	return s_tick_ms;
}

/**
  * @brief  微秒级延时
  * @param  xus 延时时长，范围：0~233015
  * @retval 无
  */
void Delay_us(uint32_t xus)
{
	uint32_t start_cycle = DWT_CYCCNT_REG;
	uint32_t wait_cycles = (SystemCoreClock / 1000000U) * xus;

	while((uint32_t)(DWT_CYCCNT_REG - start_cycle) < wait_cycles)
	{
	}
}

/**
  * @brief  毫秒级延时
  * @param  xms 延时时长，范围：0~4294967295
  * @retval 无
  */
void Delay_ms(uint32_t xms)
{
	uint32_t start_tick = Delay_GetTickMs();

	while((uint32_t)(Delay_GetTickMs() - start_tick) < xms)
	{
	}
}

/**
  * @brief  秒级延时
  * @param  xs 延时时长，范围：0~4294967295
  * @retval 无
  */
void Delay_s(uint32_t xs)
{
	while(xs--)
	{
		Delay_ms(1000);
	}
} 
