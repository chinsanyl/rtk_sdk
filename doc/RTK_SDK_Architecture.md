# RTK SDK 架构分析：工作流与状态机

> **文档版本**: v1.0.0
> **更新日期**: 2026-02-28
> **适用 SDK 版本**: 1.1.0

---

## 目录

1. [整体架构](#1-整体架构)
2. [完整工作流](#2-完整工作流)
3. [数据流分析](#3-数据流分析)
4. [状态机分析](#4-状态机分析)
5. [线程模型](#5-线程模型)
6. [重连机制分析](#6-重连机制分析)

---

## 1. 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        rtk_service 进程                      │
│                                                             │
│  ┌──────────┐    ┌──────────────┐    ┌───────────────────┐  │
│  │ rtk_main │    │   rtk_core   │    │  rtk_gps_worker   │  │
│  │  (入口)  │───▶│  (差分引擎)  │◀──▶│  (GPS串口线程)    │  │
│  └──────────┘    └──────┬───────┘    └────────┬──────────┘  │
│                         │                     │             │
│                    ┌────▼────┐          ┌──────▼──────┐     │
│                    │ 六分SDK  │          │  rtk_serial  │     │
│                    │(差分服务)│          │  (串口驱动)  │     │
│                    └────┬────┘          └──────┬──────┘     │
│                         │                     │             │
│                  ┌──────▼──────┐        ┌─────▼──────┐     │
│                  │  rtk_broad  │        │  GPS 模块  │     │
│                  │ cast(广播)  │        │  (串口设备) │     │
│                  └──────┬──────┘        └────────────┘     │
└─────────────────────────┼───────────────────────────────────┘
                          │ UDP 广播 (9000端口)
                    ┌─────▼─────┐
                    │  接收端    │
                    │ (客户程序) │
                    └───────────┘

        六分科技云服务器
              ▲  │
     GGA上报  │  │ RTCM差分数据
              │  ▼
         ┌────────────┐
         │  六分 SDK   │
         │ (网络连接)  │
         └────────────┘
```

---

## 2. 完整工作流

### 2.1 启动流程

```
main()
  │
  ├─ 1. 解析命令行参数
  │
  ├─ 2. rtk_config_load()     读取 rtk_sdk.conf（AK/AS/串口/端口等）
  │
  ├─ 3. rtk_sdk_init()        初始化全局上下文，分配互斥锁
  │      └── 状态: IDLE → INIT
  │
  ├─ 4. rtk_sdk_enable_broadcast()  创建 UDP socket，绑定广播地址
  │
  ├─ 5. rtk_sdk_start()       创建工作线程 rtk_worker_thread
  │      └── 状态: INIT → CONNECTING
  │
  └─ 6. rtk_gps_worker_start()  打开 GPS 串口，创建 GPS 工作线程
```

### 2.2 工作线程（rtk_worker_thread）流程

```
rtk_worker_thread()
  │
  ├─ A. 初始化六分SDK（最多重试 5 次，间隔 3s）
  │      sixents_sdkInit()
  │      失败 → 重试 → 达到上限 → 线程退出
  │
  ├─ B. 启动六分SDK（最多重试 5 次，间隔 3s）
  │      sixents_sdkStart()
  │      成功 → 状态: CONNECTING → RUNNING，清除错误信息
  │      失败 → 重试 → 达到上限 → 线程退出
  │
  └─ C. 主循环（每 200ms Tick 一次）
         ├─ 每 200ms: sixents_sdkTick()   驱动六分SDK内部状态机
         └─ 每 1000ms: sixents_sdkSendGGAStr()  上报 GGA 到云端

         ※ 六分SDK通过回调推送数据：
           - rtk_sixents_diff_callback()  ← 收到 RTCM 差分数据
           - rtk_sixents_status_callback() ← 收到状态码
```

### 2.3 GPS 工作线程（gps_worker_thread）流程

```
gps_worker_thread()
  │
  └─ 主循环:
       │
       ├─ [串口状态 = ERROR]
       │    ├─ 若距上次广播 ≥ 5s → broadcast_heartbeat() (gps_state=0)
       │    └─ rtk_serial_reconnect() → 重连成功继续，失败 sleep 后重试
       │
       ├─ [串口状态 ≠ OPEN]
       │    ├─ 若距上次广播 ≥ 5s → broadcast_heartbeat() (gps_state=0)
       │    └─ sleep(100ms) → 继续
       │
       └─ [串口状态 = OPEN]
            ├─ rtk_serial_read()       读取串口原始数据（100ms 超时）
            ├─ extract_nmea_sentence() 从缓冲区提取完整 NMEA 语句
            └─ [检测到 GGA 语句]
                 ├─ rtk_serial_parse_nmea()  解析 → 获取 fix, lat, lon 等
                 ├─ [fix > 0] → rtk_sdk_input_gga() 上报给差分SDK
                 └─ broadcast_position()  → UDP 广播（无论 fix 是否为 0）
```

### 2.4 RTCM 数据回路

```
六分SDK云端收到 GGA
    │
    ▼ 推送 RTCM
rtk_sixents_diff_callback()
    ├─ rtk_broadcast_send()           UDP 广播原始 RTCM（首字节 0xD3）
    └─ rtk_gps_on_rtcm_data()
         └─ rtk_serial_send_rtcm()    将 RTCM 写入 GPS 串口
              └─ GPS 模块处理 RTCM → fix 质量提升为 RTK 固定解 (fix=4)
```

---

## 3. 数据流分析

### 3.1 正常 RTK 定位数据流

```
GPS 模块 (NMEA)
    │ /dev/ttyUSBx
    ▼
gps_worker_thread
    │ 提取 GGA 语句
    ▼
rtk_sdk_input_gga()
    │ 写入 gga_buffer（加锁）
    ▼
rtk_worker_thread（每1s）
    │ sixents_sdkSendGGAStr()
    ▼
六分科技云端
    │ 返回 RTCM 差分数据
    ▼
rtk_sixents_diff_callback()
    ├──────────────────────────────────┐
    │                                  │
    ▼                                  ▼
rtk_broadcast_send()          rtk_gps_on_rtcm_data()
（UDP 广播 RTCM，0xD3 开头）   rtk_serial_send_rtcm()
                               （写入 GPS 串口）
                                    │
                                    ▼
                              GPS 模块处理 RTCM
                                    │
                               fix=4 RTK固定解
                                    │
                              gps_worker_thread
                                    │
                              broadcast_position()
                                    │
                               UDP 广播 JSON
```

### 3.2 UDP 广播内容

| 场景 | 广播内容 | 首字节 |
|------|---------|--------|
| 收到 RTCM | 原始 RTCM 二进制 | `0xD3` |
| GPS 有数据（任意 fix） | JSON 定位包 | `{` |
| GPS 串口断开（5s 一次）| JSON 心跳包（gps_state=0） | `{` |

---

## 4. 状态机分析

### 4.1 差分服务状态机（rtk_state_t）

```
                    rtk_sdk_init()
    ┌─────┐  ──────────────────────▶  ┌─────┐
    │IDLE │                           │INIT │
    └─────┘                           └──┬──┘
      ▲                                  │ rtk_sdk_start()
      │ rtk_sdk_deinit()                 ▼
      │                           ┌────────────┐
      │                           │ CONNECTING  │ ◀── 创建工作线程
      │                           └─────┬──────┘
      │                                 │ sixents_sdkStart() 成功
      │                                 ▼
      │                           ┌──────────┐
      │      rtk_sdk_stop()       │ RUNNING  │ ◀── 正常工作状态
      │    ◀──────────────────    └─────┬────┘
      │                                 │
      │                          ┌──────▼──────┐
      │                          │  STOPPING   │ ◀── 等待线程退出
      │                          └──────┬──────┘
      └──────────────────────────────── ┘

 RTK_STATE_ERROR（当前未使用，pthread_create 失败时设置）
```

**状态转换表**：

| 当前状态 | 触发条件 | 目标状态 |
|---------|---------|---------|
| IDLE | `rtk_sdk_init()` 成功 | INIT |
| INIT | `rtk_sdk_start()` 调用 | CONNECTING |
| CONNECTING | `sixents_sdkStart()` 成功 | RUNNING |
| CONNECTING | 重试 5 次失败 | 线程退出（状态未变） |
| RUNNING | `rtk_sdk_stop()` 调用 | STOPPING |
| STOPPING | 线程退出完成 | INIT |
| INIT | `rtk_sdk_deinit()` | IDLE |

### 4.2 GPS 串口状态机（rtk_serial_state_t）

```
                    rtk_serial_open()
    ┌────────┐  ──────────────────────▶  ┌──────┐
    │ CLOSED │                           │ OPEN │ ◀─┐
    └────────┘                           └──┬───┘   │
         ▲                                  │        │ rtk_serial_reconnect() 成功
         │ rtk_serial_close()               │ 连续 3 次读写错误
         │                                  ▼
         │                           ┌──────────┐
         │      rtk_serial_close()   │  ERROR   │
         │    ◀──────────────────    └────┬─────┘
         │                               │ rtk_serial_reconnect() 触发
         │                               ▼
         │                       ┌──────────────┐
         │                       │ RECONNECTING  │
         │                       └──────┬────────┘
         │                              │ 重连成功 → OPEN
         └──────────────────────────────┘ 重连失败（≥5次）→ 停留 ERROR
```

**错误触发逻辑**（`rtk_serial.c`）：
- `select()` 失败 → `error_count++`
- `read()` 失败（非 EAGAIN）→ `error_count++`
- `error_count >= 3`（`RTK_SERIAL_ERROR_THRESHOLD`）→ 状态转为 ERROR

**重连参数**：

| 参数 | 值 | 说明 |
|------|-----|------|
| `RTK_SERIAL_RECONNECT_DELAY_MS` | 2000ms | 重连前等待时间 |
| `RTK_SERIAL_MAX_RECONNECT_COUNT` | 5 次 | 最大重连次数 |
| `RTK_SERIAL_ERROR_THRESHOLD` | 3 次 | 触发错误状态的连续错误阈值 |

### 4.3 UDP 广播状态（diff_state 字段）

`diff_state` 字段是由 `get_diff_state()` 从 `rtk_state_t` 映射而来，供接收端判断差分服务状态：

| rtk_state_t | diff_state | 含义 |
|-------------|-----------|------|
| IDLE / INIT / STOPPING | 0 | 未启动 |
| CONNECTING | 1 | 连接中 |
| RUNNING | 2 | 运行正常 |
| ERROR | 3 | 错误 |

---

## 5. 线程模型

```
进程
  │
  ├─ 主线程（main）
  │    └─ 监听 stdin 命令（每 1s select）
  │
  ├─ 工作线程（rtk_worker_thread）
  │    └─ 驱动六分SDK（每 200ms Tick）
  │       └─ 每 1s 发送 GGA
  │
  └─ GPS 工作线程（gps_worker_thread）
       └─ 读取 GPS 串口 NMEA 数据
          └─ 解析 → 广播 → 上报给差分SDK
```

**互斥锁保护**：

| 互斥锁 | 保护的资源 |
|--------|---------|
| `state_mutex` | `rtk_state_t state` 状态字段 |
| `callback_mutex` | 所有回调函数指针 + `last_error_msg` |
| `gga_mutex` | `gga_buffer`、`gga_len`、`gga_ready` |
| `broadcast_mutex` | UDP socket 发送操作 |
| `serial.mutex` | 串口 fd、串口状态、位置缓存 |

---

## 6. 重连机制分析

### 6.1 GPS 串口重连（✅ 有自动重连）

由 `gps_worker_thread` 驱动：

```
检测到 RTK_SERIAL_STATE_ERROR
    │
    ├─ 广播心跳（gps_state=0）
    └─ rtk_serial_reconnect()
         ├─ 关闭旧 fd
         ├─ 等待 2000ms
         ├─ serial_open_internal() 重新打开
         └─ 成功 → OPEN，失败 → 继续 ERROR，sleep 后重试
```

### 6.2 差分服务器重连（⚠️ 仅初始连接有重试，运行中无重连）

**初始连接**（`rtk_worker_thread` 启动阶段）：
- `sixents_sdkInit()`: 失败重试，最多 5 次，间隔 3s
- `sixents_sdkStart()`: 失败重试，最多 5 次，间隔 3s
- 5 次失败后：线程退出，进程继续运行但差分服务停止

**运行中断线**（主循环阶段）：
- `sixents_sdkTick()` 失败 → 仅打印警告，**无重连触发**
- 六分SDK 内部可能有自己的保活机制，但 RTK SDK 层面**不感知也不处理**

**影响与建议**：

| 场景 | 当前行为 | 建议处理 |
|------|---------|---------|
| 初始连接失败 | 重试 5 次后线程退出，进程继续 | 外部 watchdog 检测超时后重启进程 |
| 运行中断网 | 等待六分SDK超时回调（60s）后报错 | 外部 watchdog 检测 60s 无 RTCM 后重启 |
| 断网恢复 | 无法自动恢复 | kill + restart rtk_service |

**外部 watchdog 建议逻辑**：

```bash
#!/bin/bash
while true; do
    ./rtk_service -c rtk_sdk.conf &
    PID=$!

    # 监控 UDP 心跳，超过 60s 无数据则重启
    LAST_PKT=$(date +%s)
    while kill -0 $PID 2>/dev/null; do
        NOW=$(date +%s)
        if [ $((NOW - LAST_PKT)) -gt 60 ]; then
            echo "超时无数据，重启服务"
            kill $PID
            break
        fi
        sleep 5
    done

    sleep 3  # 重启前等待
done
```
