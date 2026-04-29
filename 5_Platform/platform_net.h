/**
 * @file platform_net.h
 * @brief 智能家居项目 - 平台层网络适配接口原型定义
 *
 * @note
 * - 本文件位于 5_Platform(平台层)，对应实现见 `platform_net.c`。
 * - 设备层 `2_Device/dev_net.c` 会调用本文件声明的接口，把统一的 `NetDev` 操作
 *   (Init/Connect/Disconnect/Read/Write) 转发到驱动层 `6_ModuleDrives/driver_net.c`。
 * - 本工程底层网络模块主要是 ESP8266，应用层(MQTT)并不直接碰 AT 指令，
 *   而是通过：App -> Device(NetDev) -> Platform -> Driver 的链路完成网络收发。
 */

#ifndef __PLATFORM_NET_H
#define __PLATFORM_NET_H

#include "platform.h"
#include "dev_net.h"

/**
 * @brief 平台层网络初始化函数
 * @param net 网络上下文指针，用于确定哪个设备将被初始化
 * @return 0 成功, 非 0 即为失败
 * @note 本工程会初始化 ESP8266 的 UART、中断接收缓冲区等。
 */
int platform_net_init(struct NetDev *net);

/**
 * @brief 平台层网络建立连接函数 (支持 WiFi 热点或 TCP Server 等连接形式)
 * @param net 网络上下文指针
 * @param arg 网络参数配置结构体(以首字节 id 区分类型，如指向 WiFiInfo/TCP_UDP_Info 的指针)
 * @param timeout 操作超时时长
 * @return 0 成功, -1 为失败
 * @note `platform_net_connect()` 内部会根据 `arg[0]`(WiFi_ID/TCP_ID) 分发。
 */
int platform_net_connect(struct NetDev *net, const char *arg, int timeout);

/**
 * @brief 平台层断开网络连接函数
 * @param net 网络设备句柄
 * @param arg 断开所需附带的指令或参数，默认或当前暂未使用
 * @param timeout 断开超时执行时间
 * @return 0 表示断开操作成功送达底层, -1 为参数空
 */
int platform_net_disconnect(struct NetDev *net, const char *arg, int timeout);

/**
 * @brief 平台层网络透传写接口
 * @param net 网络上下文
 * @param buf 被发送信息的存储地址
 * @param len 期待发送的数据量字节数
 * @param timeout 预留或用于底层传输堵塞退出的时间
 * @return >=0 实际传输的长度或0表示成功; <0 发送报错
 * @note 目前底层通过 ESP8266 AT 指令发送 TCP 数据。
 */
int platform_net_write(struct NetDev *net, char *buf, uint16_t len, int timeout);

/**
 * @brief 平台层网络接收查询接口
 * @param net 网络应用句柄
 * @param buf 读取到的报文的暂存处
 * @param len 将要读取的最大限定长度或是精准长度要求
 * @param timeout 指无数据时将在这此时间之内不断阻塞重拾探测; 超时若未能满足长度需求即撤退
 * @return 0 操作成功，指定长度被读取完毕; !=0 操作超时失败或报错
 * @note 底层驱动通常从内部 RingBuffer 中提取 `+IPD` 网络包内容。
 */
int platform_net_read(struct NetDev *net, char *buf, uint16_t len, int timeout);

#endif
