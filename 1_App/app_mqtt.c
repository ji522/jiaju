/**
 * @file app_mqtt.c
 * @brief 智能家居项目 - MQTT 网络通信应用层逻辑
 * @note 负责连接 EMQX 云服务器、订阅控制下发指令(LED)、发布状态上报信息(KEY)
 */

/* Standard includes. (标准C库头文件) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FreeRTOS includes. (FreeRTOS系统核心组件) */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* 引入 Eclipse Paho MQTT C 客户端库 */
#include "MQTTClient.h"
/* 引入本地设备抽象层，主要是为了获取硬件状态或事件 */
#include "dev_io.h"

/* 外部声明：LED 控制任务的句柄（用于接收 MQTT 任务发过去的通知） */
extern TaskHandle_t ledTaskHandle;
/* 外部声明：按键扫描任务创建的消息队列（网络任务从此队列中拿取要发送的数据） */
extern QueueHandle_t xKeyQueue;

/* ========== EMQX 公共服务器配置 ========== */
/* 客户端ID，接入云服务器时的唯一身份标识，可随意修改但必须唯一 */
const static char clientID[] = "STM32_SmartHome";   
/* EMQX 公共免费测试服通常不需要用户名，留空 */
const static char username[] = "";                  
/* EMQX 公共免费测试服通常不需要密码，留空 */
const static char password[] = "";                  

/* 订阅/发布的 Topic (主题)，可根据需要自定义 */
/* 订阅此主题：手机/云下发给设备的控制命令（如 led on）会发到这里 */
const static char LedTopic[] = "/smarthome/led/cmd";    
/* 发布到此主题：设备扫描到按键后上报的状态信息发到这里 */
const static char KeyTopic[] = "/smarthome/key/info";   

/**
 * @brief MQTT 消息接收回调函数
 * @param data 包含了云端下发的主题和负载(Payload)的结构体指针
 * @note 当有订阅的主题收到消息时，底层的 MQTTYield() 会自动调用此回调
 */
void messageArrived(MessageData* data)
{
	/* 将收到的 Topic 名称和具体的 Payload 消息内容打印到串口，方便调试 */
	printf("Message arrived on topic %.*s: %.*s\n", data->topicName->lenstring.len, data->topicName->lenstring.data,
		data->message->payloadlen, data->message->payload);
	
	/* 检查收到的消息 Topic 中是否包含我们订阅的 LED 控制主题 */
	if(strstr(data->topicName->lenstring.data, LedTopic) != 0)
	{
		/* 如果 Payload 负载字符串包含 "led on" */
		if(strstr(data->message->payload, "led on") != 0)
		{
			/* 
			 * 通过 FreeRTOS 的任务通知机制，向 LED 任务发送数值 '1'
			 * eSetValueWithOverwrite 意味着如果之前有通知还没被处理，直接覆盖旧通知
			 */
			xTaskNotify(ledTaskHandle, 1, eSetValueWithOverwrite);
		}
		/* 如果 Payload 负载字符串包含 "led off" */
		else if(strstr(data->message->payload, "led off") != 0)
		{
			/* 向 LED 任务发送数值 '0'，通知其熄灭灯光 */
			xTaskNotify(ledTaskHandle, 0, eSetValueWithOverwrite);
		}
	}
}

/**
 * @brief MQTT 底层网络通信与协议处理主任务
 * @param pvParameters 任务启动时传入的参数（本例未使用）
 */
static void prvMQTTEchoTask(void *pvParameters)
{
	/* 定义一个按键结构体，用于接收队列里拿出来的事件 */
	KeyEvent key = {0};
	/* MQTT 客户端核心对象结构体 */
	MQTTClient client;
	/* 网络底层接口结构体 (内部封装了 Connect, Read, Write 等网络动作) */
	Network network;
	/* MQTT 发送数据的全局缓冲区 (打包指令用) */
	unsigned char sendbuf[256];
	/* MQTT 接收数据的全局缓冲区 (解析应答用) */
	unsigned char readbuf[256];
	/* 定义返回值变量，用于记录每一步函数调用的结果供 printf 调试 */
	int rc = 0;
	/* 初始化用于配置 MQTT 连接属性的结构体 (协议版本、KeepAlive等默认值) */
	MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;

	pvParameters = 0; /* 消除未使用参数的编译警告 */
	
	/* 初始化 Network 结构体，内部会把底层的 dev_net.c 驱动接口和驱动读写函数挂载上来 */
	NetworkInit(&network);
	
	/* 初始化 MQTTClient 对象，绑定网络接口，设定超时为 30000ms，并绑定自建的收发大数组 */
	MQTTClientInit(&client, &network, 30000, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));

	/* 定义 实习地方的服务器地址 */
	char* address = "www.yanzmain.com.cn";
	/* 调用底层的 TCP 建立连接（实质上是让 ESP8266 发送 AT+CIPSTART 命令连上了服务器） */
	if ((rc = NetworkConnect(&network, address, 1883)) != 0)
		printf("Return code from network connect is %d\n", rc); /* 连接失败打印 */

/* 若定义了独立的 MQTT 处理任务则开启（这里没有定义，也就是走主任务轮询处理） */
#if defined(MQTT_TASK)
	if ((rc = MQTTStartTask(&client)) != pdPASS)
		printf("Return code from start tasks is %d\n", rc);
#endif

	/* 填充 MQTT 协议报文头 (CONNECT) 的各个要素 */
	connectData.MQTTVersion = 3;                            /* 使用 MQTT V3.1 或 V3.1.1 协议版本 */
	connectData.clientID.cstring = (char*)clientID;         /* 指明本机客户端 ID */
	connectData.username.cstring = (char*)username;         /* 指明用户名 */
	connectData.password.cstring = (char*)password;         /* 指明鉴权密码 */
	
	/* 向服务器发起 MQTT Connect 握手请求 */
	if ((rc = MQTTConnect(&client, &connectData)) != 0)
		printf("Return code from MQTT connect is %d\n", rc); /* 失败打印错误码 */
	else
		printf("MQTT Connected\n");                         /* 连接成功 */

	/* 握手成功后，立即向服务器发去 Subscribe(订阅) 指令，主题为 led 控制，且绑定接收回调处理函数 */
	if ((rc = MQTTSubscribe(&client, LedTopic, 0, messageArrived)) != 0)
		printf("Return code from MQTT subscribe is %d\n", rc);

	/* -------- 进入核心业务大循环 -------- */
	while (1)
	{
		/* 
		 * 从系统中读取 xKeyQueue 队列。
		 * 等待最多 10 个 Tick (通常为10ms)。如果这段时间内 app_key 扫描任务扔来了最新的按键按下事件，xQueueReceive 会立刻返回 pdPASS
		 */
		if(xKeyQueue != NULL && xQueueReceive(xKeyQueue, (uint8_t*)&key, 10) == pdPASS)
		{
			/* 声明一个 MQTT 消息体结构，准备发送数据 */
			MQTTMessage message;
			/* 定义用来装我们所要发布 payload 的短数组 */
			char payload[64];

			message.qos = 0;              /* 服务质量设定为 QoS 0 (最多发送一次，不管云端是否确认收到) */
			message.retained = 0;         /* 是否让 Broker 保留该消息。0代表不保留 */
			message.payload = payload;    /* 指定消息体指针指向我们在栈上定义的字符数组 */
			/* 格式化字符串，将拿到的 按键编号和按压时长 拼接出来 */
			sprintf(payload, "key number %d, Press time:%d ms", key.num, key.time);
			message.payloadlen = strlen(payload); /* 填入实际报文内容长度 */
			
			/* 在本地串口打一句调试信息 */
			printf("%s\r\n", payload);
			
			/* 将刚才组装好的信息发布 (Publish) 到指定的 KeyTopic 主题中 */
			if ((rc = MQTTPublish(&client, KeyTopic, &message)) != 0)
				printf("Return code from MQTT publish is %d\n", rc);
					
		}
		
		/*
		 * 如果在上面的宏中没有启动独立的 MQTT 数据包接收守护任务，我们在这里执行轮询接收
		 * MQTTYield(100) 意思是交出 100ms 时间给 MQTT 底层去从串口 Buffer 吸取 ESP8266 发来的数据，并处理心跳维持(Ping)
		 */
#if !defined(MQTT_TASK)
		if ((rc = MQTTYield(&client, 100)) != 0)
			printf("Return code from yield is %d\n", rc);
#endif
		/* FreeRTOS 防止死循环独霸 CPU，强制延时 1 Tick，把同等或更低优先级任务呼叫起来做事 */
		vTaskDelay(1);
	}

	/* do not return (理论上任务死循环是不允许退出返回的，如果返回了会导致系统崩溃) */
}

/**
 * @brief 对外提供：创建 MQTT 网络通信层任务
 * @param usTaskStackSize 分配给任务的栈大小
 * @param uxTaskPriority 分配给任务的系统优先级
 */
void vStartMQTTTasks(uint16_t usTaskStackSize, UBaseType_t uxTaskPriority)
{
	BaseType_t x = 0L; /* 任务参数 */

	/* 调用系统的 xTaskCreate 创建任务实体为 prvMQTTEchoTask */
	if(xTaskCreate(prvMQTTEchoTask,	    /* The function that implements the task. (任务实体入口函数) */
			"MQTTEcho0",			    /* Just a text name for the task to aid debugging. (任务命名名) */
			usTaskStackSize,	        /* The stack size is defined in FreeRTOSIPConfig.h. (任务栈尺寸) */
			(void *)x,		            /* The task parameter, not used in this case. (任务参数) */
			uxTaskPriority,		        /* The priority assigned to the task. (优先级，数字越大级别越高) */
			NULL) == pdPASS)			/* The task handle is not used. (这里不需要保留句柄后续不用控制此任务生命周期) */
			{
				/* 提示分配任务及内卷成功 */
				printf("Create MQTT Task success.\r\n");
			}
	else
	{
		/* 内存不足分配失败 */
		printf("Create MQTT Task failed.\r\n");
	}
}
