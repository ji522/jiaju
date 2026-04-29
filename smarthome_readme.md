# SmartHome 项目一完整复盘

本文记录 **项目一：基于 STM32F103 与 FreeRTOS 的物联网智能终端（支持 MQTT 云端通信）** 的核心实现、参数、链路和面试要点，方便后续统一复盘。

## 1. 项目概述

项目目标是做一个运行在 `STM32F103` 上的轻量级物联网终端，具备以下能力：

- 本地按键采集
- 本地 LED 控制
- 通过 `ESP8266(AT)` 接入 WiFi
- 通过 TCP 连接 MQTT Broker
- 订阅云端控制指令
- 将本地按键事件上报到云端
- 运行在 `FreeRTOS` 多任务环境下

简单说，就是：

- 手机或云端下发 `led on / led off`
- 设备收到后控制本地 LED
- 本地按键按下后，把按键编号和按压时长发布到 MQTT Topic

## 2. 技术栈

- MCU：`STM32F103`
- RTOS：`FreeRTOS`
- 网络模组：`ESP8266(AT)`
- 通信接口：`UART`
- 上层协议：`MQTT`
- 关键设计：`分层架构`、`RingBuffer`、`有限状态机 FSM`、`中断驱动`

## 3. 软件架构

项目一最重要的特点之一是分层。

调用链路如下：

```text
App
 -> Device
   -> Platform
     -> Driver
       -> HAL
```

各层职责：

- `1_App`
  - 业务逻辑层
  - 包括按键任务、LED 任务、MQTT 任务
- `2_Device`
  - 设备抽象层
  - 用 `struct + 函数指针` 统一封装设备能力
- `5_Platform`
  - 平台适配层
  - 负责把统一接口分发给具体驱动
- `6_ModuleDrives`
  - 驱动层
  - 直接控制 LED、按键、串口、ESP8266

这种设计的意义：

- 上层不直接依赖底层寄存器或具体模块
- 便于换硬件、换网络模块、做功能扩展
- 代码职责更清晰，联调更容易

## 4. 核心任务划分

系统在 `main.c` 中创建了 3 个主要任务：

- MQTT 任务
  - 优先级最高
  - 负责联网、连接 Broker、订阅和发布消息
- Key 任务
  - 负责读取按键事件并送入队列
- LED 任务
  - 负责等待通知并执行 LED 亮灭

这样做的目的：

- 网络阻塞时不影响按键采集
- LED 控制不依赖轮询
- 各任务关注点单一，便于调试

## 5. 本地设备链路

### 5.1 LED 控制链路

云端消息到 LED 的路径：

```text
MQTT Broker
 -> messageArrived 回调
 -> xTaskNotify(ledTaskHandle, 1/0)
 -> LED 任务唤醒
 -> ledDev->write(...)
 -> Driver_LED_WriteStatus(...)
```

关键点：

- 下行 Topic 是 `/smarthome/led/cmd`
- payload 关键字是 `led on` 和 `led off`
- 任务间通信用的是 `Task Notify`
- 对这种单值控制场景，比队列更轻量

### 5.2 按键上报链路

本地按键到云端的路径：

```text
EXTI 中断触发
 -> 记录防抖时间戳
 -> KeyShakeProcess_Callback 判稳
 -> 生成 KeyEvent
 -> 写入 KeyBuffer
 -> KeyTask 读取后发到 xKeyQueue
 -> MQTT 任务从队列取出
 -> MQTTPublish 发布到云端
```

上报内容格式：

```text
key number %d, Press time:%d ms
```

## 6. 按键防抖实现

项目一不是简单延时防抖，而是“中断触发 + 延时判稳”的方式。

流程是：

1. 外部中断检测到电平跳变
2. 不立刻判断按下/松开
3. 只记录一个未来时间：`HAL_GetTick() + 50`
4. 到达该时间点后再读取引脚状态
5. 确认是稳定按下还是稳定松开
6. 同时拿到 `press_time` 和 `release_time` 才计算一次完整事件

优点：

- 避开机械抖动
- 不在中断里做复杂逻辑
- 不容易产生误触发

这里还做了一个很关键的安全处理：

- 只有 `press_time != 0` 且 `release_time != 0` 才计算时长
- 避免出现无符号数减法下溢，产生几万毫秒的异常值

## 7. 串口与 ESP8266 数据链路

项目一的联网依赖 `ESP8266 AT` 固件，STM32 通过 UART2 和 ESP8266 通信。

串口基本配置：

- 串口：`USART2`
- 波特率：`115200`
- 数据格式：`8N1`

### 7.1 AT 指令链路

典型初始化流程：

1. `AT+RST`
2. `AT+CWMODE=1`
3. `AT+CWJAP="ssid","pwd"`
4. `AT+CIPMUX=0`
5. `AT+CIPMODE=0`
6. `AT+CIPSTART="TCP","broker.emqx.io",1883`

### 7.2 为什么要用中断 + 双缓冲

ESP8266 返回的数据是混杂的：

- 有 AT 指令回显
- 有 `OK`
- 有 `SEND OK`
- 也有 `+IPD,...:` 这种网络业务负载

如果只用一个缓冲区，容易混在一起，不好解析。

所以项目里做了两个 RingBuffer：

- `CMDRetBuffer`
  - 存指令回显和状态字符串
  - 用于等待 `OK`、`SEND OK`
- `NetDataBuffer`
  - 存纯净业务 payload
  - 给 MQTT 上层读取

### 7.3 +IPD 数据解析 FSM

项目里对 `+IPD` 做了一个三态 FSM：

- `INIT_STATUS`
  - 找 `+IPD,`
- `LEN_STATUS`
  - 解析长度字段
- `DATA_STATUS`
  - 收集 payload

例如：

```text
+IPD,23:hello world payload...
```

状态机只把 `hello world payload...` 放进 `NetDataBuffer`，上层就不需要关心烦人的 `+IPD` 头。

## 8. MQTT 连接参数

### 8.1 连接参数

| 参数 | 当前值 | 说明 |
|---|---|---|
| Broker | `broker.emqx.io` | 公网 MQTT Broker |
| Port | `1883` | 明文 TCP 端口 |
| Client ID | `STM32_SmartHome` | 客户端标识 |
| Username | `""` | 空用户名 |
| Password | `""` | 空密码 |
| MQTT Version | `3` | MQTT 3.1 |
| Command Timeout | `30000 ms` | MQTT 命令等待超时 |
| Send Buffer | `256 bytes` | MQTT 发送缓冲 |
| Read Buffer | `256 bytes` | MQTT 接收缓冲 |

### 8.2 Topic 与消息参数

| 项目 | 当前值 | 说明 |
|---|---|---|
| 订阅 Topic | `/smarthome/led/cmd` | 云端下发 LED 控制 |
| 订阅 QoS | `0` | 至多一次 |
| 发布 Topic | `/smarthome/key/info` | 按键事件上报 |
| 发布 QoS | `0` | 至多一次 |
| Retained | `0` | 不保留 |
| 下行 payload | `led on` / `led off` | LED 控制 |
| 上行 payload 格式 | `key number %d, Press time:%d ms` | 按键编号 + 时长 |

### 8.3 运行时节奏参数

| 参数 | 当前值 | 说明 |
|---|---|---|
| `MQTTYield` | `100 ms` | MQTT 收包/心跳轮询 |
| `xQueueReceive` | `10 ticks` | 等待按键消息 |
| `vTaskDelay` | `1 tick` | 主循环让出 CPU |

### 8.4 默认值来源

`connectData` 使用 `MQTTPacket_connectData_initializer` 初始化，默认带有：

- `keepAliveInterval = 60`
- `cleansession = 1`

本项目里只显式修改了：

- `MQTTVersion`
- `clientID`
- `username`
- `password`

因此 `keepAliveInterval` 和 `cleansession` 继续使用库默认值。

## 9. WiFi 名称和密码 与 MQTT 用户名密码的区别

这两个不是一回事。

### 9.1 WiFi 名称和密码

- 用途：让 `ESP8266` 先接入热点或路由器
- 所属层级：网络接入层
- 结果：设备先能上网、拿到 IP
- 对应指令：`AT+CWJAP="SSID","PASSWORD"`

### 9.2 MQTT 用户名和密码

- 用途：设备联网后，再登录 MQTT Broker
- 所属层级：应用层协议鉴权
- 结果：决定能否连上消息服务器、订阅和发布主题

### 9.3 一句话理解

- WiFi 账号密码解决“设备能不能上网”
- MQTT 用户名密码解决“设备上网后能不能登录 Broker”

### 9.4 本项目里的实际情况

- WiFi 侧通常需要真实热点名称和密码
- MQTT 侧因为连接的是 `broker.emqx.io` 公网测试 Broker，所以：
  - `username = ""`
  - `password = ""`

## 10. 项目一完整联网链路

可以按下面这个顺序理解整个系统：

```text
STM32 上电
 -> 初始化 HAL / 时钟 / FreeRTOS 任务
 -> 初始化调试串口
 -> 初始化 ESP8266 串口驱动
 -> AT+RST
 -> AT+CWMODE=1
 -> AT+CWJAP 连 WiFi
 -> AT+CIPSTART 连 broker.emqx.io:1883
 -> 发送 MQTT CONNECT
 -> 订阅 /smarthome/led/cmd
 -> 轮询 MQTTYield 处理心跳和下行消息
 -> 本地按键事件通过 /smarthome/key/info 上报
 -> 云端指令通过 Task Notify 控 LED
```

## 11. 关键源码位置

### 11.1 业务层

- `1_App/main.c`
  - 系统入口、任务创建
- `1_App/app_mqtt.c`
  - MQTT 连接、订阅、发布
- `1_App/app_key.c`
  - 按键任务、队列发送
- `1_App/app_led.c`
  - LED 任务、通知接收

### 11.2 抽象层

- `2_Device/dev_io.h`
- `2_Device/dev_io.c`
- `2_Device/dev_net.h`
- `2_Device/dev_net.c`

### 11.3 平台层

- `5_Platform/platform_io.c`
- `5_Platform/platform_net.c`

### 11.4 驱动层

- `6_ModuleDrives/driver_led_key.c`
  - LED、按键、防抖
- `6_ModuleDrives/driver_net.c`
  - UART、AT、+IPD 解析
- `6_ModuleDrives/driver_buffer.c`
  - RingBuffer 实现
- `6_ModuleDrives/driver_dbg.c`
  - printf 串口重定向

## 12. 项目亮点

这个项目最值得讲的亮点有 4 个：

1. 分层架构
   - 用 C 语言也做了较清晰的抽象和解耦
2. 中断 + 双缓冲 + FSM
   - 把 AT 回显和网络 payload 分开处理
3. FreeRTOS 任务解耦
   - 网络、按键、LED 各司其职
4. 防抖与异常值规避
   - 处理了实际联调里常见的按键抖动和时间异常

## 13. 面试高频问题口径

### 13.1 为什么要分层？

答：

“因为业务逻辑不应该直接依赖具体硬件驱动。我这里通过 Device 和 Platform 层把底层能力统一封装起来，这样上层 MQTT 或按键业务不需要关心底层到底是哪个模块实现的，后续可维护性更好。”

### 13.2 为什么要用 UART 中断而不是轮询？

答：

“ESP8266 回来的数据是异步的，而且 AT 回显和业务 payload 混在一起。如果轮询容易阻塞，也容易丢数据。我这里开了 RXNE 中断，每来一个字节就立即接收，然后用缓冲区和状态机分流处理。”

### 13.3 为什么要两个 RingBuffer？

答：

“一个专门存 AT 指令回显，比如 `OK`、`SEND OK`，另一个专门存从 `+IPD` 剥离出的纯业务 payload。这样上层 MQTT 不会被 AT 噪声干扰。”

### 13.4 Task Notify 和 Queue 分别为什么这么用？

答：

“LED 控制是单值事件，适合 Task Notify，开销更小；按键上报是结构化事件，要传编号和时长，适合 Queue。”

### 13.5 WiFi 密码和 MQTT 密码有什么区别？

答：

“WiFi 的账号密码是让 ESP8266 接入热点，解决设备能不能上网；MQTT 的用户名密码是应用层接入 Broker 的鉴权，解决设备上网后能不能登录消息服务器。”

## 14. 项目一面试一句话总结

可以直接说：

“这个项目是一个基于 STM32F103、FreeRTOS 和 ESP8266 的轻量级物联网终端。我做了分层架构，把业务和驱动解耦；底层通过 UART 中断、双 RingBuffer 和 FSM 解析 ESP8266 的 `+IPD` 数据；上层通过 MQTT 订阅云端 LED 指令、发布本地按键事件，同时用 FreeRTOS 的 Queue 和 Task Notify 做任务间通信，保证了系统的实时性和可维护性。”

## 15. 后续可继续补充的方向

如果后面你还想继续完善这个文档，可以再补：

- 具体 WiFi SSID/密码配置来源
- MQTT 抓包或串口日志样例
- EMQX 连接成功完整日志
- 项目一常见 Bug 排查记录
- 简历版 1 分钟 / 3 分钟口播稿

## 16. 面试口语稿

这一部分尽量写成口语化表达，方便直接练习。

### 16.1 30 秒版本

“这个项目是我做的一个基于 STM32F103、FreeRTOS 和 ESP8266 的物联网终端。它可以通过 MQTT 和云端通信，实现手机下发 LED 控制，同时把本地按键事件上传到云端。这个项目里我主要做了三件事：一是分层架构设计，用 C 语言的结构体和函数指针做设备抽象；二是串口中断加双 RingBuffer 和状态机解析 ESP8266 的 `+IPD` 数据；三是用 FreeRTOS 的 Queue 和 Task Notify 做任务间通信，提高系统实时性和解耦性。” 

### 16.2 1 分钟版本

“这个项目本质上是一个基于 STM32F103 的轻量级物联网控制终端，运行在 FreeRTOS 上，通过 ESP8266 接入 WiFi，再通过 MQTT 和云端 Broker 通信。系统功能上主要有两个方向，一个是云端下发 LED 控制命令，比如 `led on` 和 `led off`，设备接收后控制本地 LED；另一个是本地按键事件上报，上传按键编号和按压时长。

在实现上，我没有把业务直接写到底层驱动里，而是做了 App、Device、Platform、Driver 四层结构，用 C 语言的 `struct + 函数指针` 做设备抽象。这样上层业务不需要关心底层到底是 LED、按键还是 ESP8266。

另一个重点是 ESP8266 串口收发。因为 AT 回显和业务数据会混在一起，所以我在 UART 接收中断里做了双缓冲设计，一个缓冲区存 AT 指令回显，一个缓冲区存从 `+IPD` 中剥离出的纯 payload，再配合三态 FSM 做无阻塞解析。最后在任务通信上，我用 Queue 传按键事件，用 Task Notify 做 LED 控制，这样网络任务阻塞时不会影响本地响应。” 

### 16.3 3 分钟版本

“这个项目是一个基于 STM32F103、FreeRTOS 和 ESP8266 的物联网智能终端，我当时的目标是做一个比较完整的小型嵌入式联网系统，而不是只把某个外设点亮。它能完成两类业务：一类是云端下发控制，本地执行，比如手机通过 MQTT 主题下发 LED 开关指令；另一类是本地事件上报，比如按键按下之后，把按键编号和按压时长发回云端。

整个系统是跑在 FreeRTOS 上的，我把它拆成了 3 个主要任务。第一个是 MQTT 任务，优先级最高，负责连接 ESP8266、连接 Broker、订阅主题和处理发布。第二个是按键任务，负责从底层驱动读取已经处理好的按键事件，再送到队列里。第三个是 LED 任务，负责等待通知并执行亮灭操作。这样做的好处是网络、输入和输出三条链路是解耦的，网络阻塞时不会把整个系统拖死。

代码结构上，我没有让业务代码直接调用底层寄存器，而是做了 App、Device、Platform、Driver 四层架构。Device 层和 Platform 层主要是抽象接口，核心思路是用 C 语言的结构体加函数指针去模拟面向对象。比如上层调用 `Init/Read/Write` 的时候，并不需要知道底层到底是按键、LED 还是网络模块。这个设计在嵌入式里挺实用，因为后面换模块或者重构的时候改动范围会小很多。

项目里最有技术含量的一块是 ESP8266 的串口数据处理。因为 ESP8266 返回的内容不是单一格式，它会同时出现 AT 指令回显、`OK`、`SEND OK` 以及 `+IPD` 这种网络业务数据。如果只用轮询去收串口，主循环会阻塞，而且很容易把这些内容混在一起。我这里是完全开了 UART 的 RXNE 中断，每来一个字节就立即接收，然后放进两个 RingBuffer：一个专门存指令回显，一个专门存网络 payload。与此同时我写了一个三态有限状态机，专门识别 `+IPD,长度:数据` 这种格式，把真正的 payload 从杂乱数据流里剥离出来，放到专用缓冲区。这样上层 MQTT 任务读到的就是干净的数据，不需要再关心 AT 噪声。

按键这一块我也做了比较工程化的处理。不是简单延时防抖，而是中断只记录触发时间，真正的状态判断延后 50ms 去做。这样可以避开机械抖动，而且不会在中断里堆太多逻辑。后来我联调时还遇到过按压时长偶尔变成几万毫秒的问题，最后定位到是极端抖动下 `press_time` 和 `release_time` 不完整导致的无符号减法异常。所以我增加了完整周期校验，只有同时拿到稳定按下和稳定松开，才会生成一次按键事件。

如果总结这个项目的收获，我觉得最主要是三点：第一是学会了怎么把一个嵌入式项目做成有结构的系统，而不是功能堆砌；第二是把中断、缓冲区和状态机组合起来，处理异步串口数据流；第三是把 FreeRTOS 任务间通信用到具体场景里，理解了 Queue 和 Task Notify 在不同场景下的取舍。” 

### 16.4 问“你在里面具体负责什么”时的回答

“这个项目是我自己主导实现和联调的，我主要负责三块内容。第一块是整体软件结构设计，包括 App、Device、Platform、Driver 的分层和接口抽象。第二块是 ESP8266 联网链路，包括 AT 指令收发、RingBuffer 缓冲和 `+IPD` 状态机解析。第三块是 FreeRTOS 任务通信和按键防抖逻辑，包括 Queue、Task Notify 和按键时长异常问题的排查处理。” 

### 16.5 问“项目难点是什么”时的回答

“我觉得最大难点不是单独把 MQTT 跑通，而是 ESP8266 返回的数据流本身是混杂的。AT 回显、状态字符串和业务数据都在一条串口上，如果处理不好，上层会非常乱。所以我后面是通过 UART 中断、双 RingBuffer 和 FSM 去做分流和剥离，这块是项目里最核心的难点。另外还有一个难点是按键时长偶发异常，最后发现不是简单防抖能解决的，而是需要加完整周期校验，避免时间戳状态不完整时直接做减法。” 

### 16.6 问“为什么用 Queue，不直接在按键中断里发 MQTT”时的回答

“因为中断里应该尽量只做最小工作，不能在里面跑网络协议栈或者做阻塞操作。我的做法是中断只记录时序信息，防抖确认后生成按键事件，再通过 Queue 交给 MQTT 任务。这样中断执行时间短，系统更稳定，而且网络逻辑和输入逻辑也解耦了。” 

### 16.7 问“为什么 LED 控制不用 Queue，而用 Task Notify”时的回答

“因为 LED 控制本质上是一个单值事件，只需要告诉 LED 任务当前是开还是关，不需要像按键事件那样传结构化数据。Task Notify 更轻量，开销比 Queue 更小，也更适合这种简单控制场景。” 

### 16.8 问“你这个项目如果继续优化，会怎么做”时的回答

“如果继续优化，我会做三件事。第一，把现在部分基于固定等待的 AT 交互进一步状态机化，减少 `HAL_Delay` 依赖。第二，加入更完整的重连和异常恢复机制，比如 WiFi 掉线、Broker 掉线后的自动重建。第三，把日志和错误码体系再规范化，这样排查问题会更快，也更接近工程项目的风格。” 

### 16.9 一个更自然的结尾说法

“所以这个项目对我来说最大的收获，不只是把 MQTT 跑通，而是我真正做了一次比较完整的嵌入式系统搭建：从底层驱动、任务调度，到协议链路和问题排查，基本都串起来了。” 
