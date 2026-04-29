/**
 * @file platform_io.c
 * @brief 智能家居项目 - 平台层 IO 适配实现
 *
 * @note
 * - 本文件位于 5_Platform(平台层)，作用是把 2_Device(设备抽象层) 的统一接口
 *   转发/适配到 6_ModuleDrives(底层驱动层) 的具体实现。
 * - 设备抽象层会调用 platform_io_init/platform_io_write/platform_io_read，
 *   平台层再根据设备类型(Type)或业务约定，调用对应的 Driver_XXX_* 函数。
 */

#include "platform_io.h"      /* 平台层 IO 接口声明 */

#include "driver_led_key.h"   /* LED/按键底层驱动 */
#include "driver_dbg.h"       /* 调试串口底层驱动 */

void platform_io_init(struct IODev *dev)
{
	/* 基本防护：避免空指针导致 HardFault */
	if(dev == NULL) return;

	/* 根据设备类型分发到不同的底层初始化函数 */
	switch(dev->Type)
	{
		case LED:
		{
			/* 初始化 LED 相关 GPIO */
			Driver_LED_Init();
			break;
		}
		case KEY:
		{
			/* 初始化按键 GPIO/EXTI，并准备按键事件缓冲机制 */
			Driver_Key_Init();
			break;
		}
		case DBGOUT:
		{
			/* 初始化调试输出串口(用于 printf 打印) */
			Driver_DBG_Init();
			break;
		}
		/* 其他设备类型预留扩展 */
		default:break;
	}
}

int platform_io_write(struct IODev *dev, uint8_t *buf, uint16_t len)
{

	/* 基本参数校验：写操作必须有设备、有效缓冲区和长度 */
	if(dev == NULL || buf == NULL || len == 0) return -1;
	
	/*
	 * 本工程中 IO 写操作主要用于 LED 控制：
	 * - 约定 buf[0] 表示 LED 状态（1:亮/开，0:灭/关）
	 * - 直接转发到底层驱动执行
	 */
	return Driver_LED_WriteStatus(buf[0]);
}

int platform_io_read(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	/* 基本参数校验：读操作必须有设备、有效缓冲区和长度 */
	if(dev == NULL || buf == NULL || len == 0) return -1;
	
	/*
	 * 本工程中 IO 读操作主要用于按键事件读取：
	 * - 底层驱动会从按键事件缓冲区中取出一条事件数据写入 buf
	 * - 返回值遵循底层驱动约定（上层通常以返回 0 作为读取成功）
	 */
	return Driver_Key_Read(buf, len);
}




