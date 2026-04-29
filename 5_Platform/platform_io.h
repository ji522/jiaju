/**
 * @file platform_io.h
 * @brief 智能家居项目 - 平台层 IO 适配接口原型定义
 *
 * @note
 * - 本文件位于 5_Platform(平台层)，对应实现见 `platform_io.c`。
 * - 设备层 `2_Device/dev_io.c` 会调用本文件声明的接口，把统一的 `IODev` 操作
 *   (Init/Read/Write) 转发到驱动层 `6_ModuleDrives` 的具体驱动函数。
 * - 本工程约定：
 *   - `platform_io_init()` 根据 `dev->Type` 分发初始化（LED/KEY/DBGOUT）。
 *   - `platform_io_write()` 目前主要用于 LED 控制，约定 `buf[0]` 表示开关状态。
 *   - `platform_io_read()` 目前主要用于读取按键事件，上层通常传入 `KeyEvent` 缓冲区。
 */

#ifndef __PLATFORM_IO_H
#define __PLATFORM_IO_H

#include "platform.h"
#include "dev_io.h"

/**
 * @brief 平台层 IO 设备的统一初始化入口
 * @param dev 指向被初始化的设备对象
 * @note 内部会根据 `dev->Type` 调用不同的 Driver 初始化函数。
 */
void platform_io_init(struct IODev *dev);

/**
 * @brief 平台层统一写操作接口
 * @param dev 操作的目标 IO 设备指针
 * @param buf 要写入的数据首地址
 * @param len 要写入的数据长度
 * @return >=0 表示执行成功，<0 表示传参错误或失败
 * @note 本工程写操作目前用于 LED：`buf[0]` 为 1/0 表示亮/灭。
 */
int platform_io_write(struct IODev *dev, uint8_t *buf, uint16_t len);

/**
 * @brief 平台层统一读操作接口
 * @param dev 读取目标对象指针
 * @param buf 存储读回结果的缓冲区地址
 * @param len 期望读取的长度
 * @return 0 表示成功读到数据，<0 表示底层暂无数据
 * @note 本工程读操作主要用于按键：上层常传入 `KeyEvent` 结构体缓冲区。
 */
int platform_io_read(struct IODev *dev, uint8_t *buf, uint16_t len);

#endif
