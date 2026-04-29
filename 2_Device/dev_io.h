/**
 * @file dev_io.h
 * @brief 智能家居项目 - 设备层 IO 抽象接口定义
 * @note 采用面向对象(结构体+函数指针)思想，剥离业务层和具体驱动层
 */

#ifndef __DEV_IO_H
#define __DEV_IO_H

/* 引入平台头文件，通常提供基础数据类型和基础操作 */
#include "platform.h"

/**
 * @brief 定义支持的 IO 设备类型枚举
 */
typedef enum{
	LED = (0),      /* LED 指示灯设备，索引为0 */
	KEY = (1),      /* 独立按键设备，索引为1 */
	DBGOUT = (2),   /* 调试输出设备，索引为2 */
}IODevType;

/**
 * @brief 定义按键事件的数据结构 (与应用层的队列通信强相关)
 */
typedef struct{
	uint16_t num;   /* 按下的按键编号 (如果系统有多个按键则用于区分) */
	uint16_t time;  /* 按键被按压持续的时长 (单位: ms)，用于防抖和长短按判断 */
}KeyEvent;

/**
 * @brief IO 设备的面向对象抽象基类 (结构体)
 */
typedef struct IODev{
	IODevType Type; /* 设备类型标志符 */
	
	/* 设备初始化方法指针：传入对象自身指针以操作上下文 */
	void (*Init)(struct IODev *dev);
	
	/* 设备的写操作方法指针：常用于控制输出状态，返回值为执行结果 */
	int (*write)(struct IODev *dev, uint8_t *buf, uint16_t len);
	
	/* 设备的读操作方法指针：常用于读取传感器或按键状态，返回值为执行结果 */
	int (*Read)(struct IODev *dev, uint8_t *buf, uint16_t len);
}IODev, *ptIODev;

/**
 * @brief 对外提供获取特定设备实例对象指针的方法
 * @param type 指定要获取的设备类型 (LED, KEY, 或 DBGOUT)
 * @return 对应类型设备的接口操作指针 ptIODev
 */
ptIODev IODev_GetDev(IODevType type);

#endif
