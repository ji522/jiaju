/**
 * @file driver_buffer.c
 * @brief 智能家居项目 - 环形缓冲区 (RingBuffer) 底层功能实现
 * @note 提供高效的 FIFO 队列，用于隔离数据产生者(如中断)和数据消费者(如主循环)
 */

#include "driver_buffer.h"
#include "stdlib.h"
#include "stdio.h"

/**
 * @brief 初始化环形缓冲区
 * @param buffer 目标缓冲对象
 * @param size 期望分配的字节数量
 * @return 0 成功, -1 失败
 */
int Driver_Buffer_Init(ptRingBuffer buffer, uint16_t size)
{
	/* 基本参数合法性校验 */
	if(buffer == NULL || size == 0) return -1;
	
	/* 如果内部堆空间还未分配，则进行动态内存分配 */
	if(buffer->fifo == NULL)
	{
		buffer->fifo = (uint8_t*)malloc(size);
		/* 检查分配是否成功 */
		if(buffer->fifo == NULL)
		{
			/* 若系统堆空间不足导致分配失败，打印诊断信息 */
			printf("Malloc %d bytes failed.\r\n", size);
			return -1;
		}
	}
	/* 读写指针双双归零，并记录容量上限 */
	buffer->pw = buffer->pr = 0;
	buffer->buf_size = size;
	
	return 0;
}

/**
 * @brief 向环形缓冲区写入 1 字节
 * @param buffer 目标缓冲对象
 * @param data 待写入的数据
 * @return 0 写入成功, -1 缓冲区已满
 */
int Driver_Buffer_Write(ptRingBuffer buffer, const uint8_t data)
{
	/* 空指针防御 */
	if(buffer == NULL || buffer->fifo == NULL) return -1;
	
	/* 计算“下一个待写位置 i”，用取模运算实现首尾相连的环形 */
	int i = (buffer->pw + 1) % buffer->buf_size;
	/* 如果 下一写位置 还没有追尾 读位置，说明还有空间，允许写入 */
	if(i != buffer->pr)
	{
		buffer->fifo[buffer->pw] = data; /* 数据入队 */
		buffer->pw = i;                  /* 指针后移 */
		
		return 0;
	}
	
	/* 如果队满，此处静默引发丢包 */
	return -1;
}

/**
 * @brief 向环形缓冲区批量写入多字节流
 * @param buffer 目标缓冲区
 * @param data_stream 数据源地址
 * @param len 被写总数据量
 * @return 真实成功写入缓冲区的字节数
 */
int Driver_Buffer_WriteBytes(ptRingBuffer buffer, const uint8_t *data_stream, uint16_t len)
{
	uint16_t i = 0;
	uint16_t write_pos = 0U;
	/* 防御性判空 */
	if(buffer == NULL || buffer->fifo == NULL)	return -1;
	if(data_stream == NULL)	return -1;
	if(len == 0)	return -1;
	if(Driver_Buffer_GetFree(buffer) < len)	return 0;

	write_pos = buffer->pw;
	/* Publish the new write pointer only after the complete packet is copied. */
	for(i = 0; i <len; i++)
	{
		buffer->fifo[write_pos] = data_stream[i];
		write_pos = (uint16_t)((write_pos + 1U) % buffer->buf_size);
	}
	buffer->pw = write_pos;
	
	return (int)i;
}

/**
 * @brief 从环形缓冲区提取 1 个最早写入的字节
 * @param buffer 目标缓冲对象
 * @param data 提货地址
 * @return 0 成功读出, -1 缓冲区空无数据
 */
int Driver_Buffer_Read(ptRingBuffer buffer, uint8_t *data)
{
	/* 空指针及合法性校验 */
	if(buffer == NULL || buffer->fifo == NULL) return -1;
	
	/* 如果读、写指针重合，代表当前队列里没有新鲜数据 */
	if(buffer->pr == buffer->pw)	return -1;
	
	/* 从当前读指针处提走数据，并滑动读指针 */
	*data = buffer->fifo[buffer->pr];
	buffer->pr = (buffer->pr + 1) % buffer->buf_size;
	
	return 0;
}

/**
 * @brief 连贯地从环形缓冲区提取指定长度的字节流
 * @param buffer 目标缓冲
 * @param data_stream 数据卸货地
 * @param len 想取走的数据长度
 * @return 实际提取到了多少字节
 */
int Driver_Buffer_ReadBytes(ptRingBuffer buffer, uint8_t *data_stream, uint16_t len)
{
	uint16_t i = 0;
	uint16_t read_pos = 0U;
	/* 判空防御 */
	if(buffer == NULL || buffer->fifo == NULL)	return -1;
	if(data_stream == NULL)	return -1;
	if(len == 0)	return -1;
	if(Driver_Buffer_GetUsed(buffer) < len)	return 0;
	
	read_pos = buffer->pr;
	/* Release the consumed space only after the complete packet is copied. */
	for(i = 0; i <len; i++)
	{
		data_stream[i] = buffer->fifo[read_pos];
		read_pos = (uint16_t)((read_pos + 1U) % buffer->buf_size);
	}
	buffer->pr = read_pos;
	
	return (int)i;
}

uint16_t Driver_Buffer_GetUsed(const ptRingBuffer buffer)
{
	uint16_t read_pos = 0U;
	uint16_t write_pos = 0U;

	if(buffer == NULL || buffer->fifo == NULL || buffer->buf_size == 0U)
	{
		return 0U;
	}

	read_pos = buffer->pr;
	write_pos = buffer->pw;
	if(write_pos >= read_pos)
	{
		return (uint16_t)(write_pos - read_pos);
	}

	return (uint16_t)(buffer->buf_size - read_pos + write_pos);
}

uint16_t Driver_Buffer_GetFree(const ptRingBuffer buffer)
{
	if(buffer == NULL || buffer->fifo == NULL || buffer->buf_size == 0U)
	{
		return 0U;
	}

	return (uint16_t)(buffer->buf_size - Driver_Buffer_GetUsed(buffer) - 1U);
}

/**
 * @brief 一键清空环形缓冲区所有遗留的内容
 * @param buffer 目标缓冲
 * @return 0 成功
 */
int Driver_Buffer_Clean(ptRingBuffer buffer)
{
	if(buffer == NULL || buffer->fifo == NULL)	return -1;
	/* Advancing only the consumer pointer is race-safe for the ISR/task SPSC
	 * buffers. Bytes arriving after this snapshot remain available. */
	buffer->pr = buffer->pw;
	return 0;
}
