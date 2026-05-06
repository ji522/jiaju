/**
 * @file driver_dbg.h
 * @brief 智能家居项目 - 串口信息打印/调试 (Debug) 底层驱动头文件
 */

#ifndef __DRIVER_DBG_H
#define	__DRIVER_DBG_H

#include "stm32f4xx_hal.h"

/**
 * @brief 初始化供 printf 使用的底层硬件串口(USART1)
 * @return 0 成功, -1 失败
 */
int Driver_DBG_Init(void);

#endif
