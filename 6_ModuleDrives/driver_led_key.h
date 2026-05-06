/**
 * @file driver_led_key.h
 * @brief 智能家居项目 - LED 及独立按键底层驱动功能头文件
 */

#ifndef __DRIVER_LED_KEY_H
#define __DRIVER_LED_KEY_H

#include "stm32f4xx_hal.h"
#include "dev_io.h"

/* ==================== LED 硬件宏定义 ==================== */
/* LED 连接的 GPIO 端口（F407 底板使用 GPIOF Pin 9） */
#define LED_PORT	GPIOF
/* LED 连接的具体引脚编号 */
#define LED_PIN		GPIO_PIN_9

/**
 * @brief 驱动 LED 亮灭状态 (低电平驱动 / 共阳极)
 * @param STATUS 传入 1 为亮(引脚拉低)，传入 0 为灭(引脚拉高)
 */
#define LED(STATUS)	HAL_GPIO_WritePin(LED_PORT, LED_PIN, STATUS?GPIO_PIN_RESET:GPIO_PIN_SET)

/**
 * @brief 翻转 LED 当前状态闪烁一次
 */
#define LED_SHINE()	HAL_GPIO_TogglePin(LED_PORT, LED_PIN)

/* ==================== KEY 硬件宏定义 ==================== */
/* 按键连接的 GPIO 端口（F407 使用 PA0 + EXTI0） */
#define KEY_PORT	GPIOA
/* 按键连接的具体引脚编号 */
#define KEY_PIN		GPIO_PIN_0

/**
 * @brief 获取当前按键引脚电平状态
 * @return 0 为按键被按下(引脚拉低)，1 为按键松开(引脚拉高)
 */
#define KEY_STATUE()	HAL_GPIO_ReadPin(KEY_PORT, KEY_PIN)

/* ==================== 初始化与功能接口 ==================== */

/**
 * @brief 初始化 LED 对应的 GPIO 为推挽输出
 * @return 0 表示成功
 */
int Driver_LED_Init(void);

/**
 * @brief LED 通用写控制接口
 * @param status 1 为亮，0 为灭
 * @return 恒为 0
 */
int Driver_LED_WriteStatus(uint8_t status);

/**
 * @brief 初始化按键对应的 GPIO、上下拉及外部中断 (EXTI)
 * @return 0 表示成功, -1 表示环形缓冲区分配失败
 */
int Driver_Key_Init(void);

/**
 * @brief 从按键事件缓冲区提取一条按键信息 (事件驱动)
 * @param buf 指向用来存放已解析成功的按键事件数据的指针
 * @param len 期待抓取的缓冲长度量
 * @return 返回实际读到的数据量，1 表示成功取出一个按键包，<=0 表示无按键发生
 */
int Driver_Key_Read(uint8_t *buf, uint16_t len);

#endif
