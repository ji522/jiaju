/**
 * @file platform_net.c
 * @brief 智能家居项目 - 平台层网络适配控制实现
 *
 * @note
 * - 本文件位于 5_Platform(平台层)，主要做“适配/分发”：
 *   把 2_Device(设备抽象层) 传下来的统一网络操作，映射到 6_ModuleDrives(驱动层)
 *   的 ESP8266 AT 指令驱动实现。
 * - 设备层调用链：App -> dev_net(NetDev) -> platform_net -> driver_net。
 */

#include "platform_net.h"
#include <driver_net.h>

/**
 * @brief 平台层统一网络初始化
 * @param net 网络设备对象句柄
 * @return 0: 成功, <0: 失败
 */
int platform_net_init(struct NetDev *net)
{
	/* 基本防护：避免空指针 */
	if(net == NULL) return -1;

	/* 调用底层的 ESP8266 硬件 UART 初始化和 AT 复位操作 */
	return Driver_Net_Init();
}

/**
 * @brief 平台层统一网络连接（兼容连 WiFi 和连 TCP）
 * @param net 网络设备句柄
 * @param arg 包含连接信息的结构体指针
 * @param timeout 操作超时限制
 * @return 0: 成功, -1: 失败
 */
int platform_net_connect(struct NetDev *net, const char *arg, int timeout)
{
	int ret = -1;
	
	/* 防护判断：确保被传入的指针不为空 */
	if(net == NULL || arg == NULL) return -1;
	
	/*
	 * 约定：arg 指向一个“连接参数结构体”，其首字节为 ConnectID：
	 * - WiFi_ID：表示 arg 实际类型为 WiFiInfo
	 * - TCP_ID ：表示 arg 实际类型为 TCP_UDP_Info
	 * 平台层据此做一次强转，再调用对应的 Driver 函数。
	 */
	switch(arg[0])
	{
		case WiFi_ID:
		{
			/* 强制类型转换为热点信息结构体 */
			WiFiInfo *p = (WiFiInfo *)arg;
			/* 调用底层驱动连接路由器 */
			ret = Driver_Net_ConnectWiFi(p->ssid, p->pwd, timeout);
			break;
		}
		case TCP_ID:
		{
			/* 强制类型转换为 Socket 服务器信息结构体 */
			TCP_UDP_Info *p = (TCP_UDP_Info *)arg;
			/* 调用底层驱动发 AT+CIPSTART 连接 TCP 服务器（本工程通常是 MQTT Broker） */
			ret = Driver_Net_ConnectTCP(p->IP, p->RemotePort, timeout);
			break; /* 修复：此处漏了一个 break 防止由于 switch 穿透引发的不可预知错误 */
		}
		default: 
			break;
	}
	return ret;
}
/**
 * @brief 平台层统一网络断开接口
 * @param net 网络设备句柄
 * @param arg 包含连接信息的结构体指针
 * @param timeout 操作超时限制
 * @return 0: 断开成功, -1: 失败
 */
int platform_net_disconnect(struct NetDev *net, const char *arg, int timeout)
{
	int ret = -1;
	
	/* 防护判断：确保被传入的指针不为空 */
	if(net == NULL || arg == NULL) return -1;
	
	/* 根据 arg[0] 分发：断开 WiFi 或断开 TCP */
	switch(arg[0])
	{
		case WiFi_ID:
		{
			/* 调用底层驱动：断开路由器连接 (对应 AT+CWQAP) */
			ret = Driver_Net_DisconnectWiFi();
			break;
		}
		case TCP_ID:
		{
			/* 调用底层驱动：断开 MQTT 服务器云连接 (对应 AT+CIPCLOSE) */
			ret = Driver_Net_Disconnect_TCP_UDP();
			break; /* 并在此处补上缺失的 break 防止隐患 */
		}
		default: 
			break;
	}
	return ret;
}

/**
 * @brief 平台层统一网络发送接口 (给应用层调用的统一入口)
 * @param net 网络设备上下文
 * @param buf 要发送的内容缓冲区地址
 * @param len 要发送的字节数
 * @param timeout 操作超时限制
 * @return 0: 发送成功, -1: 发送失败
 */
int platform_net_write(struct NetDev *net, char *buf, uint16_t len, int timeout)
{
	/* 基本参数合法性校验 */
	if(net == NULL || buf == NULL || len == 0) return -1;
	
	/* 透传调用底层网络驱动向 Socket 端口内发送数据 (对应 AT+CIPSEND 操作) */
	return Driver_Net_TransmitSocket(buf, len, timeout);
}

/**
 * @brief 平台层统一网络接收接口 (MQTT Yield 任务会在这里提取云端消息)
 * @param net 网络设备上下文
 * @param buf 获取读取数据的存储地址
 * @param len 期待抓取的数据长度
 * @param timeout 读取超时时间
 * @return 0: 抓取成功, 非0: 缓冲区为空或抓取超时
 */
int platform_net_read(struct NetDev *net, char *buf, uint16_t len, int timeout)
{
	/* 基本参数合法性校验 */
	if(net == NULL || buf == NULL || len == 0) return -1;
	
	/* 透传调用底层驱动从内部环形缓冲区 (RingBuffer) 读取已解析出的网络包数据 */
	return Driver_Net_RecvSocket(buf, len, timeout);
}




