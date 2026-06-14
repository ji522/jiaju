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

	switch(dev->Type)
	{
		case LED:
		{
			/* 当前写操作主要适配 LED 输出。 */
			return Driver_LED_WriteStatus(buf[0]);
		}

		case KEY:
		case DBGOUT:
		default:
		{
			/* 当前平台层未定义这些 IO 设备的通用写语义。 */
			return -1;
		}
	}
}

int platform_io_read(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	/* 基本参数校验：读操作必须有设备、有效缓冲区和长度 */
	if(dev == NULL || buf == NULL || len == 0) return -1;

	switch(dev->Type)
	{
		case KEY:
		{
			/* 当前读操作主要适配按键事件读取。 */
			return Driver_Key_Read(buf, len);
		}

		case LED:
		case DBGOUT:
		default:
		{
			/* 当前平台层未定义这些 IO 设备的通用读语义。 */
			return -1;
		}
	}
}




