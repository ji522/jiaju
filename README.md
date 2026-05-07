# MQTT-CAN BCM Node Prototype

基于 `STM32F407 + FreeRTOS + ESP8266` 与 `STM32F103 + CAN` 的双节点车载通信原型。  
当前版本实现了：

- `F407` 作为主控/网关节点
- `F103` 作为执行从节点
- MQTT 远程控制
- CAN `BODY_CMD -> BODY_STATUS` 控制闭环
- 本地按键输入事件上报

---

## 1. 项目定位

这个项目最初来源于一个 `STM32F103 + FreeRTOS + ESP8266(MQTT)` 的智能家居项目，后续迁移到 `STM32F407`，并进一步收口为一个更偏车载方向的：

- `BCM（Body Control Module）` 通信节点原型
- `MQTT-CAN` 轻量级网关原型

目标不是做量产 ECU，而是做一个：

- 可跑通
- 可联调
- 可展示
- 可写简历

的最小系统原型。

---

## 2. 当前系统结构

```text
MQTTX / 云端
   |
   v
STM32F407 主节点
  - FreeRTOS
  - MQTT client
  - BCM command parser
  - CAN master / gateway
   |
   |  CAN BUS
   |
STM32F103 从节点
  - CAN slave
  - BODY_CMD receiver
  - local LED actuator
  - BODY_STATUS feedback
```

主链路：

```text
/vehicle/bcm/command
 -> F407 解析 JSON
 -> 发送 CAN BODY_CMD (0x100)
 -> F103 收到命令并执行输出
 -> F103 回发 BODY_STATUS (0x101)
 -> F407 收到状态并上报 /vehicle/bcm/status
```

---

## 3. 节点职责

## 3.1 F407 主节点

工程目录：

- [1_App](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/1_App>)
- [6_ModuleDrives](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives>)

主要职责：

- 连接 WiFi
- 建立 MQTT 通道
- 订阅控制命令
- 解析 `bcm_ctrl/body_ctrl`
- 发送 CAN `BODY_CMD`
- 接收 `BODY_STATUS`
- 上报 `/vehicle/bcm/status`
- 上报本地输入 `/vehicle/bcm/input`

关键文件：

- [app_mqtt.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/1_App/app_mqtt.c>)
- [app_can.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/1_App/app_can.c>)
- [driver_can.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives/driver_can.c>)
- [driver_net.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives/driver_net.c>)

## 3.2 F103 从节点

工作副本目录：

- [F103_BCM_Slave_Node](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/F103_BCM_Slave_Node>)

主要职责：

- 初始化 CAN
- 监听 `BODY_CMD`
- 控制本地 LED
- 回发 `BODY_STATUS`

关键文件：

- [User/main.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/F103_BCM_Slave_Node/User/main.c>)
- [Hardware/MyCAN.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/F103_BCM_Slave_Node/Hardware/MyCAN.c>)
- [Hardware/BcmProtocol.h](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/F103_BCM_Slave_Node/Hardware/BcmProtocol.h>)

---

## 4. MQTT Topic 设计

## 主控制 Topic

```text
/vehicle/bcm/command
```

推荐命令：

```json
{"cmd":"bcm_ctrl","lamp":true,"hazard":false,"fan":true}
```

兼容旧命令：

```text
/vehicle/body/cmd
```

```json
{"cmd":"body_ctrl","lamp":true,"hazard":false,"fan":true}
```

## 主状态 Topic

```text
/vehicle/bcm/status
```

状态分两类：

### 1. node_status

```json
{
  "src":"bcm",
  "frame":"node_status",
  "node_mode":1,
  "output_mask":1,
  "lamp":true,
  "hazard":false,
  "fan":false,
  "can_tx_cnt":12,
  "can_rx_cnt":10,
  "last_rx_id":256,
  "mqtt_state":4,
  "mqtt_reconn_cnt":0
}
```

### 2. body_status

```json
{
  "src":"bcm",
  "frame":"body_status",
  "can_id":257,
  "lamp":true,
  "hazard":false,
  "fan":false,
  "node_mode":1
}
```

## 输入事件 Topic

```text
/vehicle/bcm/input
```

示例：

```json
{"src":"input","device":"key","key_id":1,"press_ms":202}
```

兼容保留：

```text
/smarthome/key/info
```

## 兼容老演示 Topic

```text
/smarthome/led/cmd
```

示例：

```json
{"cmd":"led","action":"on"}
```

说明：

- 这是旧版 LED 演示入口
- 当前仍然保留，但不再是主推荐控制入口

---

## 5. CAN 报文定义

共享协议头：

- [can_bcm_protocol.h](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives/can_bcm_protocol.h>)
- [F103_BCM_Slave_Node/Hardware/BcmProtocol.h](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/F103_BCM_Slave_Node/Hardware/BcmProtocol.h>)

## 5.1 BODY_CMD

- CAN ID：`0x100`
- 方向：`F407 -> F103`
- DLC：`1`

Byte0 bit 定义：

- bit0：`lamp`
- bit1：`hazard`
- bit2：`fan`

## 5.2 BODY_STATUS

- CAN ID：`0x101`
- 方向：`F103 -> F407`
- DLC：`2`

Byte0：

- 当前输出位图

Byte1：

- 节点模式
  - `0 = INIT`
  - `1 = NORMAL`
  - `2 = DEGRADED`
  - `3 = FAULT`

## 5.3 NODE_HEARTBEAT

- CAN ID：`0x1F0`
- 当前主要用于节点保活和调试计数

---

## 6. 硬件连接

## 6.1 F407 主节点

当前 CAN 引脚：

- `PB9 = CAN1_TX`
- `PB8 = CAN1_RX`

接到 CAN 收发器模块：

- `PB9 -> TXD`
- `PB8 -> RXD`
- `GND -> GND`

## 6.2 F103 从节点

当前 CAN 引脚：

- `PA12 = CAN_TX`
- `PA11 = CAN_RX`

接到 CAN 收发器模块：

- `PA12 -> TXD`
- `PA11 -> RXD`
- `GND -> GND`

## 6.3 CAN 总线连接

- `CANH <-> CANH`
- `CANL <-> CANL`
- `GND <-> GND`

建议总线两端各保留一个 `120Ω` 终端电阻。

## 6.4 F103 本地输出

当前 F103 从节点用的是教程里的 LED 驱动：

- `LED1 = PA1`
- `LED2 = PA2`

映射关系：

- `lamp -> LED1`
- `hazard -> LED2`
- `fan` 当前只做状态位

LED 为低电平点亮，接法：

- 长脚（正极）-> `3.3V`，串限流电阻
- 短脚（负极）-> `PA1/PA2`

---

## 7. 当前测试方法

## 7.1 MQTTX 订阅

订阅：

```text
/vehicle/bcm/status
/vehicle/bcm/input
/smarthome/key/info
```

## 7.2 主控制命令测试

发布：

```text
/vehicle/bcm/command
```

示例：

```json
{"cmd":"bcm_ctrl","lamp":true,"hazard":false,"fan":false}
```

预期：

- F407 收到 MQTT 命令
- F407 发送 `BODY_CMD`
- F103 `LED1` 亮
- F103 回 `BODY_STATUS`
- MQTTX 收到 `/vehicle/bcm/status`

再发：

```json
{"cmd":"bcm_ctrl","lamp":false,"hazard":true,"fan":false}
```

预期：

- F103 `LED1` 灭
- F103 `LED2` 亮

## 7.3 老兼容灯控测试

```text
/smarthome/led/cmd
```

```json
{"cmd":"led","action":"on"}
```

预期：

- F407 本地 LED 亮

## 7.4 本地按键测试

按下 F407 本地按键，预期：

- `/vehicle/bcm/input` 收到按键事件
- `/smarthome/key/info` 兼容收到按键事件

---

## 8. 当前边界

当前版本已经可以作为简历项目和 GitHub 展示原型，但要明确：

- F407 主节点已经具备双节点联调结构
- F103 从节点已实现最小执行闭环
- 当前 F103 从节点未接入本地按键
- `hazard/fan` 中只有 `hazard` 映射到了第二颗 LED
- 还不是完整量产 ECU
- 未使用 AUTOSAR / UDS / Bootloader / Functional Safety

---

## 9. 当前已解决的关键问题

详见：

- [问题记录与修复日志.md](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/问题记录与修复日志.md>)

已处理的主要问题包括：

- F103 -> F407 迁移后 MQTT 不通
- ESP8266 初始化时序问题
- FreeRTOS 任务不运行
- CAN 路径收发逻辑混乱
- BCM 命令响应过慢
- LED 状态与调试闪灯冲突

---

## 10. 后续计划

建议的后续优先级：

1. 固化当前双节点版本并整理简历描述
2. 为 F103 从节点增加最小串口调试输出
3. 为 F103 增加本地输入状态并通过 `BODY_STATUS` 反馈
4. 进一步规范 `BCM` 状态语义
5. 如果有必要，再考虑更深的诊断/故障管理功能

