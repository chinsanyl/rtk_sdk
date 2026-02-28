# RTK SDK 项目文件说明

> **文档版本**: v1.0.0
> **更新日期**: 2026-02-28

---

## 项目目录结构

```
rtk_sdk/
├── include/
│   └── rtk_sdk.h                   # 公开 API 头文件（对外接口）
├── src/
│   ├── rtk_internal.h              # 内部头文件（模块间共享）
│   ├── rtk_core.c                  # 核心引擎（SDK 生命周期 + 差分SDK封装）
│   ├── rtk_gps_worker.c            # GPS 串口自动模式工作线程
│   ├── rtk_serial.c                # GPS 串口驱动（读写 + NMEA 解析）
│   ├── rtk_serial.h                # 串口模块头文件
│   ├── rtk_broadcast.c             # UDP 广播模块
│   ├── rtk_config.c                # INI 配置文件解析
│   ├── rtk_gga.c                   # GGA 语句构造与校验
│   └── rtk_log.c                   # 日志模块
├── main/
│   └── rtk_main.c                  # 独立进程入口（编译为 rtk_service）
├── thirdparty/
│   └── sixents/
│       ├── inc/
│       │   ├── sixents_sdk.h       # 六分科技 SDK 头文件
│       │   └── sixents_types.h     # 六分科技 SDK 类型定义
│       ├── lib/
│       │   ├── libsixents-core-sdk.a   # 六分SDK静态库
│       │   └── libsixents-core-sdk.so  # 六分SDK动态库
│       └── cacert/
│           └── root.crt            # HTTPS 根证书
├── doc/
│   ├── README.md                   # 项目总览
│   ├── API.md                      # API 参考手册
│   ├── RTK_Service_Process_Guide.md  # 独立进程使用指南
│   ├── RTK_SDK_Architecture.md     # 架构工作流与状态机分析
│   ├── RTK_SDK_FileMap.md          # 本文件：项目文件说明
│   └── RTK_SDK_API_Manual.docx     # API 手册（Word 版）
├── Makefile                        # x86/Linux 编译脚本
├── Makefile.arm                    # ARM 交叉编译脚本
├── rtk_sdk.conf.example            # 配置文件示例（不含真实凭证）
├── CHANGELOG.md                    # 版本变更记录
└── .gitignore                      # Git 忽略规则
```

---

## 文件详细说明

### 公开接口

#### [include/rtk_sdk.h](../include/rtk_sdk.h)
**对外公开的唯一头文件**，集成方只需包含此文件。

包含：
- 版本宏定义（`RTK_SDK_VERSION_STR`）
- 错误码枚举（`rtk_error_t`）
- 状态枚举（`rtk_state_t`）
- 配置结构体（`rtk_config_t`）
- 定位结果结构体（`rtk_position_result_t`）
- 所有公开 API 函数声明（init/start/stop/GGA输入/回调注册/广播控制）

---

### 核心源码（src/）

#### [src/rtk_internal.h](../src/rtk_internal.h)
**内部模块共享头文件**，不对外暴露。

包含：
- 全局上下文结构体 `rtk_context_t`（包含状态、配置、线程句柄、互斥锁、GGA缓冲区、错误信息等所有运行时状态）
- 内部常量（Tick间隔200ms、GGA发送间隔1000ms、重连延迟3000ms、最大重试5次、心跳间隔5000ms）
- 内部函数声明（各模块内部函数，跨文件调用）
- 日志宏（`RTK_LOGE/W/I/D`）

#### [src/rtk_core.c](../src/rtk_core.c)
**SDK 核心引擎**，是整个 SDK 的中枢。

职责：
- `rtk_sdk_init/deinit/start/stop`：SDK 生命周期管理
- `rtk_worker_thread`：工作线程，驱动六分SDK（`sixents_sdkTick`）、定时发送 GGA
- 六分SDK回调接收：`rtk_sixents_diff_callback`（RTCM数据）、`rtk_sixents_status_callback`（状态码）
- 回调分发：`rtk_dispatch_rtcm/status/error/position`（分发给上层注册的回调）
- 错误管理：`rtk_dispatch_error` 保存最后一次错误到 `last_error_msg`，供广播使用
- 工具函数：`rtk_get_time_ms`、`rtk_sleep_ms`、`rtk_safe_strcpy`、`rtk_strerror`

关键逻辑：
- 初始连接阶段：`sixents_sdkInit` + `sixents_sdkStart` 各最多重试 5 次
- 状态管理：INIT → CONNECTING → RUNNING（六分SDK启动成功时切换）
- 连接恢复时自动清除 `last_error_msg`

#### [src/rtk_gps_worker.c](../src/rtk_gps_worker.c)
**GPS 串口自动模式工作线程**，独立进程模式的核心数据链路。

职责：
- `gps_worker_thread`：主循环，负责串口数据读取、NMEA 解析、广播
- `broadcast_position`：构造含 `diff_state/diff_err_msg/gps_state` 的 JSON 包并 UDP 广播
- `broadcast_heartbeat`：串口断开时定时广播心跳包（`gps_state=0`，每5秒）
- `rtk_gps_on_rtcm_data`：收到 RTCM 后写入 GPS 串口（RTCM 回路）
- `get_diff_state`：将 `rtk_state_t` 映射为 `diff_state` 字段值（0/1/2/3）

关键逻辑：
- 串口 ERROR 状态：发心跳 + 自动重连
- GGA 解析后：`fix>0` 才向差分SDK上报，但任意 fix 值都触发广播
- 首次启动 `last_broadcast_time=0` 使心跳立即触发

#### [src/rtk_serial.c](../src/rtk_serial.c) / [src/rtk_serial.h](../src/rtk_serial.h)
**GPS 串口驱动模块**，封装所有串口操作。

职责：
- 串口打开/关闭/状态查询（`rtk_serial_open/close/is_open/get_state`）
- 自动重连（`rtk_serial_reconnect`）：最多5次，每次等待2秒
- 线程安全读写：使用 `dup(fd)` 复制文件描述符，避免竞态
- NMEA 解析（`rtk_serial_parse_nmea`）：支持 GP/GN/GB/GL/GA 所有 Talker ID
- GGA 精细解析（`parse_gga`）：解析纬度/经度/高度/HDOP/卫星数/fix质量
- 错误检测：连续 3 次读写错误（`RTK_SERIAL_ERROR_THRESHOLD`）自动进入 ERROR 状态

状态机：CLOSED → OPEN → ERROR → RECONNECTING → OPEN

#### [src/rtk_broadcast.c](../src/rtk_broadcast.c)
**UDP 广播模块**，负责将数据发送到局域网。

职责：
- `rtk_broadcast_init`：创建 UDP socket，设置广播选项（`SO_BROADCAST`），初始化目标地址
- `rtk_broadcast_deinit`：关闭 socket，释放资源
- `rtk_broadcast_send`：线程安全发送（`broadcast_mutex` 保护），支持 RTCM 二进制和 JSON 字符串

配置：默认端口 9000，默认广播地址 255.255.255.255

#### [src/rtk_config.c](../src/rtk_config.c)
**INI 格式配置文件解析模块**。

职责：
- `rtk_config_load`：解析 INI 文件，支持 `[auth]`、`[network]`、`[broadcast]`、`[gps]`、`[log]` 等节
- `rtk_config_validate`：校验必填字段（AK/AS/device_id/device_type）
- `rtk_config_dump`：将配置内容打印到日志（AK/AS 不明文输出）

#### [src/rtk_gga.c](../src/rtk_gga.c)
**GGA 语句处理工具模块**。

职责：
- `rtk_gga_validate`：验证 GGA 语句格式和 NMEA 校验和
- `rtk_gga_build`：根据经纬高度构造标准 NMEA GGA 语句（供 `rtk_sdk_input_position` 使用）

#### [src/rtk_log.c](../src/rtk_log.c)
**日志模块**，统一日志输出。

职责：
- 支持 ERROR/WARN/INFO/DEBUG 四个级别
- 支持控制台输出、文件输出、回调输出三个渠道
- 线程安全，带时间戳前缀
- 通过 `rtk_log_set_*_enabled()` 独立控制各渠道开关

---

### 进程入口（main/）

#### [main/rtk_main.c](../main/rtk_main.c)
**独立进程模式入口**，编译目标为 `rtk_service` 可执行文件。

职责：
- 命令行参数解析（`-c` 配置文件、`-p` 端口、`-a` 地址、`-l` 日志级别、`-d` 守护进程）
- 信号处理（SIGINT/SIGTERM → 优雅退出）
- 守护进程模式（`daemon(0,0)` 调用）
- 回调注册（调试回调：打印 RTCM 收到、状态变化、错误信息）
- 主循环：`select` 监听 stdin，支持交互命令（`s`/`q`/`h`）
- 优雅停止：按顺序停止 GPS 工作线程 → 差分SDK → 注销SDK

---

### 第三方库（thirdparty/sixents/）

#### [thirdparty/sixents/inc/sixents_sdk.h](../thirdparty/sixents/inc/sixents_sdk.h)
六分科技差分SDK头文件。定义 SDK 初始化参数结构体 `sixents_sdkConf`、回调函数类型、API 函数声明。

关键函数：
- `sixents_sdkInit`：初始化（传入AK/AS/回调）
- `sixents_sdkStart`：建立到云端连接
- `sixents_sdkSendGGAStr`：上报 GGA
- `sixents_sdkTick`：驱动SDK内部状态机（需周期调用）
- `sixents_sdkStop/sdkFinal`：停止和注销

#### [thirdparty/sixents/inc/sixents_types.h](../thirdparty/sixents/inc/sixents_types.h)
六分科技SDK类型定义：状态码常量（如 1201=鉴权成功、1206=鉴权失败、1404=GGA超时）、错误码、枚举等。

#### [thirdparty/sixents/lib/](../thirdparty/sixents/lib/)
六分科技差分SDK预编译库：
- `libsixents-core-sdk.a`：静态库（ARM 交叉编译使用）
- `libsixents-core-sdk.so`：动态库

#### [thirdparty/sixents/cacert/root.crt](../thirdparty/sixents/cacert/root.crt)
HTTPS 连接六分科技云端所需的根证书。

---

### 配置与构建文件

#### [rtk_sdk.conf.example](../rtk_sdk.conf.example)
配置文件示例（INI格式）。包含所有可配置项和注释说明，不含真实 AK/AS 凭证，可安全提交到版本控制。实际使用时复制为 `rtk_sdk.conf` 并填入真实凭证。

#### [Makefile](../Makefile)
x86/Linux 本地编译脚本。编译目标：`rtk_service`（可执行文件）、`librtk_sdk.so`（动态库）、`librtk_sdk.a`（静态库）。

#### [Makefile.arm](../Makefile.arm)
ARM 交叉编译脚本。使用 `arm-linux-gnueabihf-gcc` 编译器，链接六分SDK静态库，生成适用于 ARM 嵌入式 Linux 的二进制。

#### [CHANGELOG.md](../CHANGELOG.md)
版本变更记录文件，记录每个版本的新增、变更、修复内容。

#### [.gitignore](../.gitignore)
Git 忽略规则。排除编译产物（`*.o`、`build/`、`rtk_service`）和含凭证的配置文件（`rtk_sdk.conf`）。

---

### 文档（doc/）

| 文件 | 说明 |
|------|------|
| [README.md](README.md) | 项目总览：功能介绍、快速上手 |
| [API.md](API.md) | 完整 API 参考手册（线程库模式） |
| [RTK_Service_Process_Guide.md](RTK_Service_Process_Guide.md) | 独立进程模式使用指南（推荐阅读） |
| [RTK_SDK_Architecture.md](RTK_SDK_Architecture.md) | 架构分析：工作流、状态机、线程模型、重连机制 |
| [RTK_SDK_FileMap.md](RTK_SDK_FileMap.md) | 本文件：所有文件的作用说明 |
| [RTK_SDK_API_Manual.docx](RTK_SDK_API_Manual.docx) | API 手册 Word 版 |
