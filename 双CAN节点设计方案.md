# 双 CAN 节点设计方案

## 目标

将当前单板 `F407 + CAN loopback` 的 BCM/MQTT 原型，升级为：

```text
MQTT / 上位机
   |
   v
F407 主节点（网关 / BCM 主控）
   |
   |  CAN BUS
   |
F103 从节点（执行 / 状态反馈）
```

最终形成更像车企项目的链路：

```text
MQTT 下发 BCM 命令
 -> F407 接收并解析
 -> F407 发送 CAN BODY_CMD
 -> F103 接收 BODY_CMD
 -> F103 控制本地 LED / 输入状态
 -> F103 回发 BODY_STATUS
 -> F407 收到 BODY_STATUS
 -> MQTT 上报 /vehicle/bcm/status
```

---

## 节点角色划分

## 1. F407 主节点

职责：

- 负责 WiFi / MQTT 联网
- 订阅 `/vehicle/bcm/command`
- 解析 `bcm_ctrl/body_ctrl`
- 发送 CAN `BODY_CMD`
- 接收 F103 回发的 `BODY_STATUS`
- 向 `/vehicle/bcm/status` 上报
- 保留节点诊断状态

当前已有基础：

- [app_mqtt.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/1_App/app_mqtt.c>)
- [app_can.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/1_App/app_can.c>)
- [driver_can.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives/driver_can.c>)

---

## 2. F103 从节点

建议职责：

- 初始化 CAN 外设
- 常驻监听 `BODY_CMD`
- 解析 `lamp / hazard / fan`
- 控制本地 LED
- 读取本地按键或输入状态
- 回发 `BODY_STATUS`

建议保持尽量简单：

- 不联网
- 不上 MQTT
- 只做 CAN 执行与状态反馈

这样项目结构会更像：
- 主节点：网关 / 控制中心
- 从节点：执行节点 / 外设控制单元

---

## 建议 CAN 报文定义

## 1. BODY_CMD

- CAN ID：`0x100`
- 方向：`F407 -> F103`
- DLC：`1`

Byte0 bit 定义：

- bit0：`lamp`
- bit1：`hazard`
- bit2：`fan`

示例：

- `0x01`：lamp on
- `0x00`：all off
- `0x05`：lamp + fan

---

## 2. BODY_STATUS

- CAN ID：`0x101`
- 方向：`F103 -> F407`
- DLC：`2`

Byte0：

- 当前输出状态位图
  - bit0：lamp
  - bit1：hazard
  - bit2：fan

Byte1：

- 节点模式
  - `0`：INIT
  - `1`：NORMAL
  - `2`：DEGRADED
  - `3`：FAULT

---

## 3. NODE_HEARTBEAT

- CAN ID：`0x1F0`
- 可选
- 用于保活，不是第一优先

建议第一阶段双节点联调先不依赖它，先保证 `BODY_CMD/BODY_STATUS` 闭环跑通。

---

## 硬件连接建议

每块 MCU 不要直接连 `CANH/CANL`，都通过 CAN 收发器模块（如 `TJA1050/TJA1051`）接总线。

### F407 侧

- `CAN_TX` -> 收发器 `TXD`
- `CAN_RX` -> 收发器 `RXD`
- `GND` 共地

### F103 侧

- `CAN_TX` -> 收发器 `TXD`
- `CAN_RX` -> 收发器 `RXD`
- `GND` 共地

### 总线侧

- 收发器1 `CANH` <-> 收发器2 `CANH`
- 收发器1 `CANL` <-> 收发器2 `CANL`
- 两端加 `120Ω` 终端电阻

---

## 软件迁移步骤建议

## 第一步：保留 F407 现有逻辑，切换 CAN 为 NORMAL

当前：

- [driver_can.c](</D:/BaiduNetdiskDownload/项目资料/项目资料/项目代码/SmartHome/SmartHome_mqtt服务器用的实习这里的/6_ModuleDrives/driver_can.c>)
  使用的是 `CAN_MODE_LOOPBACK`

需要改成：

- `CAN_MODE_NORMAL`

并确认：

- 波特率统一
- F103/F407 两边都一致

---

## 第二步：为 F103 写最小从节点

建议新建一个极简版本，功能只保留：

- CAN 初始化
- 接收 `BODY_CMD`
- 控制 LED
- 回发 `BODY_STATUS`

不建议一开始把 F103 做得太复杂，否则联调时问题源会很多。

---

## 第三步：验证控制闭环

验证链路：

1. MQTT 发：
```json
{"cmd":"bcm_ctrl","lamp":true,"hazard":false,"fan":false}
```

2. F407：
- 收到 MQTT
- 发 `0x100 BODY_CMD`

3. F103：
- 收到 `0x100`
- 点亮 LED
- 发 `0x101 BODY_STATUS`

4. F407：
- 收到 `0x101`
- 向 `/vehicle/bcm/status` 上报

---

## 第四步：再考虑节点心跳和故障状态

闭环跑通后再补：

- `NODE_HEARTBEAT`
- 节点超时检测
- `DEGRADED/FAULT` 状态
- 统计计数器

这样调试顺序更稳。

---

## 当前建议的推进顺序

1. 先把当前 F407 版本保存到 GitHub
2. 从 loopback 改为 normal mode
3. 连接 F103 + TJA 模块
4. 做 `BODY_CMD/BODY_STATUS` 双节点闭环
5. 最后补心跳与诊断状态

---

## 这套设计写到简历时的表达

可以表述为：

- 基于 `STM32F407 + FreeRTOS + ESP8266` 实现 `MQTT-CAN` 轻量网关原型
- 基于 `STM32F103 + CAN` 实现从节点执行单元
- 完成主从节点间 `BODY_CMD/BODY_STATUS` 控制闭环
- 实现远程控制、总线转发、本地执行和状态反馈

