# RTK SDK 架构分析：工作流与状态机

> **文档版本**: v1.1.0
> **更新日期**: 2026-02-28
> **适用 SDK 版本**: 1.2.0

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
  └─ 外层重连循环（无限，直到 worker_running=0）
       │
       ├─ [reconnect_count > 0] → calc_backoff_ms() 计算等待时间，sleep_interruptible()
       │      退避公式: delay = BASE(3s) × FACTOR(1.6)^(n-1)，上限 3 小时
       │
       ├─ A. 状态 → CONNECTING
       │
       ├─ B. sixents_sdkInit()
       │      失败 → dispatch_error()，reconnect_count++，continue 外层循环
       │
       ├─ C. sixents_sdkStart()
       │      失败 → sixents_cleanup()，dispatch_error()，reconnect_count++，continue
       │      成功 → reconnect_count=0，状态 → RUNNING，清除 last_error_msg
       │
       └─ D. 内层主循环（每 200ms Tick 一次）
              ├─ 每 200ms: sixents_sdkTick()   驱动六分SDK内部状态机
              │      失败计数 tick_errors++，达到 RTK_TICK_ERROR_THRESHOLD(10) →
              │            need_reconnect=1，跳出内层循环
              └─ 每 1000ms: sixents_sdkSendGGAStr()  上报 GGA 到云端

              ※ 六分SDK通过回调推送数据：
                - rtk_sixents_diff_callback()  ← 收到 RTCM 差分数据
                - rtk_sixents_status_callback() ← 收到状态码

              内层循环退出 → sixents_cleanup()，reconnect_count++，继续外层循环
```

### 2.3 GPS 工作线程（gps_worker_thread）流程

```
gps_worker_thread()
  │
  └─ 主循环:
       │
       ├─ [串口状态 = ERROR]
       │    ├─ 若距上次广播 ≥ 5s → broadcast_heartbeat(0) (gps_state=0)
       │    └─ rtk_serial_reconnect()
       │         成功 → 重置 last_nmea_time（给模块10s启动缓冲），继续
       │         失败 → reconnect_count++，sleep(2s)，继续
       │
       ├─ [串口状态 ≠ OPEN]
       │    ├─ 若距上次广播 ≥ 5s → broadcast_heartbeat(0) (gps_state=0)
       │    └─ sleep(100ms) → 继续
       │
       └─ [串口状态 = OPEN]
            ├─ [GPS模块静默检测] 若 now-last_nmea_time ≥ 10s 且距上次广播 ≥ 5s
            │    └─ broadcast_heartbeat(3)  (gps_state=3)
            │
            ├─ rtk_serial_read()       读取串口原始数据（100ms 超时）
            ├─ extract_nmea_sentence() 从缓冲区提取完整 NMEA 语句
            └─ [检测到 GGA 语句]
                 ├─ rtk_serial_parse_nmea()  解析 → 获取 fix, lat, lon 等
                 ├─ [fix > 0] → rtk_sdk_input_gga() 上报给差分SDK
                 ├─ broadcast_position()  → UDP 广播（无论 fix 是否为 0）
                 └─ last_nmea_time = now  （更新GPS模块活跃时间戳）
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

| 场景 | 广播内容 | 首字节 | gps_state |
|------|---------|--------|-----------|
| 收到 RTCM | 原始 RTCM 二进制 | `0xD3` | - |
| GPS 有数据（任意 fix） | JSON 定位包 | `{` | 1 或 2 |
| GPS 串口断开/重连中（5s 一次）| JSON 心跳包 | `{` | 0 |
| GPS 串口正常但模块静默 >10s（5s 一次）| JSON 心跳包 | `{` | 3 |

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
| CONNECTING | Init/Start 失败 | CONNECTING（退避后重试，无上限） |
| RUNNING | `sixents_sdkTick()` 连续失败 ≥10 次 | CONNECTING（触发重连） |
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
| `RTK_SERIAL_MAX_RECONNECT_COUNT` | **20 次** | 最大重连次数（v1.2.0 由 5 增至 20） |
| `RTK_SERIAL_ERROR_THRESHOLD` | 3 次 | 触发错误状态的连续错误阈值 |
| `RTK_GPS_NO_DATA_TIMEOUT_MS` | 10000ms | GPS 模块静默检测阈值（gps_state=3）|

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

### 6.1 GPS 串口重连（✅ 有自动重连 + GPS 模块静默检测）

由 `gps_worker_thread` 驱动：

```
检测到 RTK_SERIAL_STATE_ERROR
    │
    ├─ 广播心跳（gps_state=0，每 5s）
    └─ rtk_serial_reconnect()
         ├─ 关闭旧 fd
         ├─ 等待 2000ms
         ├─ serial_open_internal() 重新打开
         ├─ 成功 → OPEN，重置 last_nmea_time，继续
         └─ 失败 → 继续 ERROR，sleep 后重试（最多 20 次）

串口 OPEN 但 GPS 模块超过 10 秒无 NMEA 输出
    │
    └─ 广播心跳（gps_state=3，每 5s）
         触发场景：模块掉电、固件崩溃、波特率不匹配
         收到 NMEA 后自动恢复正常广播
```

**GPS 串口重连参数**：

| 参数 | 值 | 说明 |
|------|-----|------|
| `RTK_SERIAL_ERROR_THRESHOLD` | 3 次 | 连续读写错误触发 ERROR |
| `RTK_SERIAL_RECONNECT_DELAY_MS` | 2000ms | 重连等待时间（固定） |
| `RTK_SERIAL_MAX_RECONNECT_COUNT` | 20 次 | 最大重连次数 |
| `RTK_GPS_NO_DATA_TIMEOUT_MS` | 10000ms | 模块静默触发 gps_state=3 |
| `RTK_GPS_HEARTBEAT_INTERVAL_MS` | 5000ms | 心跳广播间隔 |

### 6.2 差分服务器重连（✅ 有自动重连 + 指数退避，v1.2.0 新增）

**统一外层重连循环**（`rtk_worker_thread`）：

```
外层循环（无限重试，除非 worker_running=0）
  │
  ├─ [reconnect_count > 0] 先等待退避时间
  │      delay = RTK_RECONNECT_BASE_MS × (RTK_RECONNECT_FACTOR/100)^(n-1)
  │             = 3000 × 1.6^(n-1)，上限 10800000ms（3 小时）
  │      等待期间每 200ms 检查一次是否需要退出
  │
  ├─ sixents_sdkInit() 失败
  │      → dispatch_error()，reconnect_count++，退避后重试
  │
  ├─ sixents_sdkStart() 失败
  │      → sixents_cleanup()，dispatch_error()，reconnect_count++，退避后重试
  │
  ├─ 成功启动 → reconnect_count=0，状态 RUNNING，清除 last_error_msg
  │
  └─ 内层 Tick 循环
       ├─ sixents_sdkTick() 连续失败 ≥ 10 次（RTK_TICK_ERROR_THRESHOLD）
       │      → need_reconnect=1，跳出内层循环
       └─ 内层退出 → sixents_cleanup()，reconnect_count++，回到外层等待
```

**退避时间参考**（BASE=3s，FACTOR=×1.6）：

| 重连次数 | 等待时间 |
|---------|---------|
| 第 1 次 | 0s（立即） |
| 第 2 次 | 3s |
| 第 3 次 | ≈4.8s |
| 第 4 次 | ≈7.7s |
| 第 5 次 | ≈12.3s |
| 第 10 次 | ≈77s |
| 第 15 次 | ≈39min |
| 第 20 次+ | 3 小时（封顶） |

> kill 进程后重启，重连计数从 0 开始，无历史累积。

**差分重连参数**（宏定义，修改后重新编译生效）：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `RTK_RECONNECT_BASE_MS` | 3000ms | 首次重连等待时间 |
| `RTK_RECONNECT_FACTOR` | 160 | 退避倍率（/100 = ×1.6） |
| `RTK_RECONNECT_MAX_MS` | 10800000ms | 最长等待时间（3 小时） |
| `RTK_TICK_ERROR_THRESHOLD` | 10 次 | 触发重连的连续 Tick 失败阈值 |
