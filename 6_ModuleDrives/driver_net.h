/**
 * @file driver_net.h
 * @brief 智能家居项目 - ESP8266 网络底层驱动模块头文件
 * @note 暴露了网络模组初始化、TCP/UDP连接、WiFi接入、断开以及数据收发的核心 API
 */

#ifndef __DRIVER_NET_H
#define	__DRIVER_NET_H

#include "stm32f1xx_hal.h"

/**
 * @brief 初始化网络模组 (复位 ESP8266，配置 UART2 外设及中断，初始化数据接收环形缓冲区)
 * @return 0 成功
 */
int Driver_Net_Init(void);

/**
 * @brief 向 TCP 透明传输服务器静默发送一段数据流
 * @param socket 待发送的数据首地址
 * @param len 待发送的字节长度
 * @param timeout 本动作限时，0 为死等
 * @return 0 发送成功, 其他失败
 */
int Driver_Net_TransmitSocket(const char *socket, int len, int timeout);

/**
 * @brief 从网络环形缓冲区主动读取已经接收剥离好的纯 TCP 有效数据 (Payload)
 * @param buf 存放提取之后数据的首地址
 * @param len 想要尝试读取的最大长度
 * @param timeout 读不到时的阻塞试探时间(毫秒)
 * @return 0 提取成功，数据放置于 buf 处
 */
int Driver_Net_RecvSocket(char *buf, int len, int timeout);

/**
 * @brief 控制 ESP8266 发送 AT 指令去连接指定的 WiFi 热点
 * @param ssid 热点名称
 * @param pwd 热点密码
 * @param timeout 指令执行超时保护时间
 * @return 0 连接成功
 */
int Driver_Net_ConnectWiFi(const char *ssid, const char *pwd, int timeout);

/**
 * @brief 控制 ESP8266 主动断开当前 WiFi 连接
 * @return 0 断开成功
 */
int Driver_Net_DisconnectWiFi(void);

/**
 * @brief 控制 ESP8266 发出 AT 指令去建立一个 TCP 连接
 * @param ip 远端服务器的 IP 地址或域名 (如 broker.emqx.io)
 * @param port 远端服务器的端口 (如 1883)
 * @param timeout 指令允许的最大超时执行时间
 * @return 0 建立成功
 */
int Driver_Net_ConnectTCP(const char *ip, int port, int timeout);

/**
 * @brief 断开正在维持的 TCP/UDP 会话连接
 * @return 0 成功
 */
int Driver_Net_Disconnect_TCP_UDP(void);

#endif
