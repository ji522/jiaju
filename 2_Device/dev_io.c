/**
 * @file dev_io.c
 * @brief 智能家居项目 - 设备层 IO 抽象接口的具体实现
 * @note 它是 1_App 层调用到底层平台的一个过渡桥梁
 */

#include "dev_io.h"         /* 引入设备抽象层头文件 */
#include <platform_io.h>    /* 引入更下级的平台 IO 接口头文件 */

/**
 * @brief 封装后的统一初始化函数
 * @param dev 需要被初始化的 IO 设备指针
 */
static void IODev_Init(struct IODev *dev)
{
	/* 直接透传调用平台层的初始化方法 (Platform 里面根据不同硬件执行最终的 HAL_Init 操作) */
	platform_io_init(dev);
}

/**
 * @brief 封装后的统一写数据函数
 * @param dev 操作的目标 IO 设备指针
 * @param buf 要写入的数据首地址
 * @param len 要写入的数据长度
 * @return 写入操作返回状态码 (0表示成功)
 */
static int IODev_Write(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	/* 透传调用底层平台写接口 */
	return platform_io_write(dev, buf, len);
}

/**
 * @brief 封装后的统一读数据函数
 * @param dev 操作的目标 IO 设备指针
 * @param buf 用来接收读取数据的缓冲首地址
 * @param len 期望读取的长度
 * @return 读取操作返回状态码 (0表示成功提取到数据)
 */
static int IODev_Read(struct IODev *dev, uint8_t *buf, uint16_t len)
{
	/* 透传调用底层平台读接口 */
	return platform_io_read(dev, buf, len);
}

/**
 * @brief 实例化全局的 3 个外设控制操作结构体
 * 按照 {类型, Init指针, Write指针, Read指针} 的顺序装载
 * 这是 C 语言实现多态/接口映射的典型手法
 */
static IODev g_tIODevs[3] = {{LED, IODev_Init, IODev_Write, IODev_Read},\
							{KEY, IODev_Init, IODev_Write, IODev_Read},\
							{DBGOUT, IODev_Init, IODev_Write, IODev_Read}};

/**
 * @brief 给应用层的 API：获取指定外设的操作句柄
 * @param type 指定外设枚举名（0:LED，1:KEY，2:DBGOUT）
 * @return 被请求外设的设备抽象对象的地址
 */
ptIODev IODev_GetDev(IODevType type)
{
	/* 通过枚举作为索引，直接返回装载好的局部全局变量数组中的元素地址 */
	return &g_tIODevs[type];
}
