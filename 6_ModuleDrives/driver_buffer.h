/**
 * @file driver_buffer.h
 * @brief 智能家居项目 - 环形缓冲区 (RingBuffer) 核心头文件
 * @note 这是一个通用的环形队列模块，本系统内主要用于给按键采集和 ESP8266 串口收发做数据缓存。
 */

#ifndef __DRIVER_BUFFER_H
#define __DRIVER_BUFFER_H

#include "stm32f4xx_hal.h"

/**
 * @brief 环形缓冲区控制结构体
 */
typedef struct{
	uint8_t *fifo;      /* 缓冲区的动态分配内存块基地址 */
	uint16_t pw;        /* 写指针 (Pointer Write)，记录下一次可以写入数据的位置 */
	uint16_t pr;        /* 读指针 (Pointer Read)，记录下一次可以读取数据的位置 */
	uint16_t buf_size;  /* 该缓冲区的申请的最大容量 */
} RingBuffer, *ptRingBuffer;

/**
 * @brief 初始化分配一个环形缓冲区
 * @param buffer 被初始化的环形缓冲区对象
 * @param size 需要申请的缓冲字节总数
 * @return 0 成功, 非 0 失败 (如内存不足)
 */
int Driver_Buffer_Init(ptRingBuffer buffer, uint16_t size);

/**
 * @brief 往缓冲区送入单个字节
 * @param buffer 操作的目标缓冲区
 * @param data 需要写入的一个字节数据
 * @return 0 成功写入, -1 失败(如缓冲区满)
 */
int Driver_Buffer_Write(ptRingBuffer buffer, const uint8_t data);

/**
 * @brief 往缓冲区批量写入连续字节
 * @param buffer 目标缓冲区
 * @param data_stream 数据源首地址
 * @param len 将被写入的字节长度
 * @return 实际成功写入的字节数，如果缓冲区没空间了就会比要求的 len 少
 */
int Driver_Buffer_WriteBytes(ptRingBuffer buffer, const uint8_t *data_stream, uint8_t len);

/**
 * @brief 从缓冲区抓取最早的一个字节数据 (先进先出)
 * @param buffer 数据存放的园区
 * @param data 用来接收该字节数据的地址
 * @return 0 取出成功, -1 取出失败 (缓冲区是空的)
 */
int Driver_Buffer_Read(ptRingBuffer buffer, uint8_t *data);

/**
 * @brief 连续从缓冲区抓取多个字节数据
 * @param buffer 数据存放区
 * @param data_stream 接收数组的其实地址
 * @param len 打算抓取的数据量
 * @return 真实连续获取到的字节数
 */
int Driver_Buffer_ReadBytes(ptRingBuffer buffer, uint8_t *data_stream, uint8_t len);

/**
 * @brief 暴力清空重置该缓冲区
 * @param buffer 将被重置的园区指针
 * @return 0 成功清零复位
 */
int Driver_Buffer_Clean(ptRingBuffer buffer);

#endif
