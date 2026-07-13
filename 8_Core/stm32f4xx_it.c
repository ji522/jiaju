/**
  * @file    stm32f4xx_it.c
  * @brief   Interrupt Service Routines (SmartHome F407)
  * @note    Peripheral ISRs are in driver files. Only Cortex-M4 exception handlers here.
  */
#include "main.h"
#include "stm32f4xx_it.h"
#include "driver_can.h"

void NMI_Handler(void) {}
void HardFault_Handler(void) { while(1) {} }
void MemManage_Handler(void) { while(1) {} }
void BusFault_Handler(void) { while(1) {} }
void UsageFault_Handler(void) { while(1) {} }
void DebugMon_Handler(void) {}

void CAN1_RX0_IRQHandler(void)
{
	Driver_CAN_IRQHandler();
}

void CAN1_SCE_IRQHandler(void)
{
	Driver_CAN_IRQHandler();
}
