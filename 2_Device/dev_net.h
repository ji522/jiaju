/**
 * @file dev_net.h
 * @brief 智能家居项目 - 设备层网络抽象接口定义
 * @note 屏蔽底层具体网络通讯模组（如 ESP8266、NB-IoT 或 4G），向应用层(特别是 MQTT)提供统一操作规范
 */

#ifndef __DEV_NET_H
#define __DEV_NET_H

/**
 * @brief 网络设备类型枚举
 * 用于兼容不同的通信模块
 */
typedef enum{
	ESP8266 = (1<<0),    /* WiFi ESP8266 模块枚举值 */
	OTHERS = 0xFFFF,     /* 预留其他模块扩展 */
}NetDevType;

/**
 * @brief 网络连接通道/协议类型
 */
typedef enum{
	WiFi_ID = (1<<0),    /* AP 接入点(热点) ID */
	UDP_ID  = (1<<1),    /* UDP 无连接通道 ID (目前未使用) */
	TCP_ID  = (1<<2)     /* TCP 连接通道 ID */
}ConnectID;

/**
 * @brief 暂未使用：WIFI 接入配置信息结构体
 */
typedef struct{
	ConnectID id;
	char *ssid;          /* WiFi 热点名 */
	char *pwd;           /* WiFi 密码 */
}WiFiInfo;

/**
 * @brief 暂未使用：TCP/UDP 网络套接字信息连结结构体
 */
typedef struct{
	ConnectID id;
	char *IP;            /* 服务器的 IP 地址 */
	unsigned short LocalPort;  /* 本地绑定端口 */
	unsigned short RemotePort; /* 远程服务器目标端口 */
}TCP_UDP_Info;

/**
 * @brief 核心：网络设备的面向对象抽象基类 (结构体)
 */
typedef struct NetDev{
	unsigned char Type;  /* 保存设备类型 */
	
	/* 网络硬件复位和初始化方法指针 */
	int (*Init)(struct NetDev *net);
	
	/* 连接函数，用于连接 WiFi 或 TCP Server，arg 通常是带有特定语法的连接指令字符串 */
	int (*Connect)(struct NetDev *net, const char *arg, int timeout);
	
	/* 断开连接方法 */
	int (*Disconnect)(struct NetDev *net, const char *arg, int timeout);
	
	/* 底层网络发送函数，把 buf 指向的数据用 len 长度抛给网络模组 */
	int (*Write)(struct NetDev *net, char *buf, unsigned short len, int timeout);
	
	/* 底层网络接收函数，被用在 MQTT 协议栈心跳等待的延时接收之中 */
	int (*Read)(struct NetDev *net, char *buf, unsigned short len, int timeout);
}NetDev, *ptNetDev;

/**
 * @brief 对外提供获取特定类型网络设备实例指针的函数
 * @param type 需要获取的网络模块类型枚举 (本案为 ESP8266)
 * @return 获取到的网络模块对象基地址
 */
ptNetDev NetDev_GetDev(NetDevType type);

#endif
