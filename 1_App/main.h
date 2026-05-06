/**
 * @file main.h
 * @brief 智能家居项目 - 全局主头文件
 * @note 包含了底层 HAL 库的引用，通常也用来定义全局宏
 */

/* 引入宏定义防护，防止头文件被重复包含导致编译报错 */
#ifndef __MAIN_H
#define __MAIN_H

/* 引入 STM32F1 系列的 HAL (硬件抽象层) 核心库头文件，几乎所有的 .c 都会间接包含它 */
#include "stm32f4xx_hal.h"

#endif /* __MAIN_H */
