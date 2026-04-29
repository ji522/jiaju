/**
 * @file dev_net.c
 * @brief 智能家居项目 - 设备层网络抽象接口的具体实现
 * @note 用于实例化 ESP8266 设备，封装并向下透传调用 `platform_net` 提供的方法
 */

#include "dev_net.h"         /* 引入网络设备抽象类头文件 */
#include <platform_net.h>    /* 引入平台层网络接口定义 */

/**
 * @brief 封装后的统一网络初始化方法
 * @param net 网络设备上下文指针
 * @return 0: 成功, <0: 失败
 */
static int NetDev_Init(struct NetDev *net)
{
	/* 透传调用平台层的初始化方法 (底层会初始化串口，重启ESP8266并开启STA模式) */
	return platform_net_init(net);
}

/**
 * @brief 封装后的网络连接方法
 * @param net 网络设备上下文指针
 * @param arg 目标参数 (对于TCP通常是 "IP,PORT" 这样的组合字串，或者是 WiFi热点 密码 等)
 * @param timeout 等待操作响应的超时时间 (单位因底层而异，一般为底层循环Tick)
 * @return 0: 成功, 其他: 失败超时
 */
static int NetDev_Connect(struct NetDev *net, const char *arg, int timeout)
{
	/* 透传给下级平台层连接逻辑 */
	return platform_net_connect(net, arg, timeout);
}

/**
 * @brief 封装后的断开网络连接方法
 * @param net 网络设备上下文指针
 * @param arg 附加参数指令 (未使用或预留给不同协议使用)
 * @param timeout 超时时间
 */
static int NetDev_Disconnect(struct NetDev *net, const char *arg, int timeout)
{
	/* 发送 AT+CIPCLOSE 或者 AT+CWQAP 断开命令的透传 */
	return platform_net_disconnect(net, arg, timeout);
}

/**
 * @brief 封装后的底层流发送方法
 * @param net 网络上下文
 * @param buf 要发送的字节数据流首地址 (比如拼接好的 MQTT 包)
 * @param len 打算发送的长度
 * @param timeout 超时时间
 * @return 0表示发送成功，其余表示底层堵塞或失败
 */
static int NetDev_Write(struct NetDev *net, char *buf, unsigned short len, int timeout)
{
	/* 调用平台层写入，相当于给 ESP8266 发送 AT+CIPSEND=len 继而发送 payload */
	return platform_net_write(net, buf, len, timeout);
}

/**
 * @brief 封装后的底层流接收方法
 * @param net 网络上下文
 * @param buf 存储获取数据的接受容器地址
 * @param len 期望接收多少个字节的数据
 * @param timeout 阻塞等待的最长时间
 * @return 0代表如期提取到了 len 长度数据，非0表示超时或者提取到了不完整数据
 */
static int NetDev_Read(struct NetDev *net, char *buf, unsigned short len, int timeout)
{
	/* 调用平台层读取，本质上是从中断维护的长数组缓冲(RingBuffer)里面提取定长内容 */
	return platform_net_read(net, buf, len, timeout);
}

/**
 * @brief 实例化全局的 1 个 ESP8266 网络控制操作结构体
 * 按照结构体声明顺序，装载类型标志 (ESP8266) 及 5 个标准的函数接口
 */
static NetDev g_tNetDev = {ESP8266, NetDev_Init, NetDev_Connect, NetDev_Disconnect, NetDev_Write, NetDev_Read};

/**
 * @brief 给应用层的 API：获取指定类别网络模块的操作句柄
 * @param type 需要提取的外设枚举 (本工程中业务逻辑调的是 ESP8266)
 * @return 获取到的网络模块对象基地址
 */
ptNetDev NetDev_GetDev(NetDevType type)
{
	/* 判断请求的类型，并将上文静态实例化的对应网络设备句柄交给应用层使用 */
	if(ESP8266 == type)
		return &g_tNetDev;
	
	/* 其他未实现的类型返回空指针，防止段错误 */
	return NULL;
}
