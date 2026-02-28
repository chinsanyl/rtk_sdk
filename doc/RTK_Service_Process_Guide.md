# RTK Service 独立进程使用指南

> **文档版本**: v1.2.0
> **更新日期**: 2026-02-28
> **适用 SDK 版本**: 1.0.0 及以上

---

## 文档版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.2.0 | 2026-02-28 | 新增 `gps_state=3`（GPS 模块静默故障检测）；串口最大重连次数 5→20 次；RTK 差分服务器新增运行时自动重连+指数退避（最长 3 小时） |
| v1.1.0 | 2026-02-28 | UDP JSON 包新增 `diff_state` / `diff_err_msg` / `gps_state` 三个状态字段；串口断开时增加定时心跳广播；fix=0 时也广播状态包；移除冗余的 `src` 字段（与 `fix` 值完全相同） |
| v1.0.0 | 2026-02-28 | 初始版本，独立进程使用说明 |

---

## 目录

1. [概述](#1-概述)
2. [架构说明](#2-架构说明)
3. [编译](#3-编译)
4. [配置文件](#4-配置文件)
5. [启动与运行](#5-启动与运行)
6. [工作模式](#6-工作模式)
7. [UDP 广播协议](#7-udp-广播协议)
8. [接收端集成](#8-接收端集成)
9. [交互命令](#9-交互命令)
10. [守护进程模式](#10-守护进程模式)
11. [日志管理](#11-日志管理)
12. [常见问题](#12-常见问题)

---

## 1. 概述

RTK SDK 提供两种使用形式：

| 使用形式 | 说明 | 适用场景 |
|---------|------|---------|
| **独立进程模式** | 编译为 `rtk_service` 可执行文件，通过 UDP 广播输出差分数据和定位结果 | 多进程架构、跨语言集成、快速部署 |
| 线程库模式 | 以 `.so`/`.a` 库链接到业务进程，通过回调函数获取数据 | 对延迟敏感、需深度定制的场景 |

**独立进程模式的优势：**

- **零侵入**：业务程序无需集成 RTK SDK，只需监听 UDP 端口
- **跨语言**：Python、Java、Node.js 等任何能接收 UDP 的程序都可使用
- **快速部署**：直接运行可执行文件，无需修改业务代码
- **进程隔离**：RTK 服务崩溃不影响业务进程
- **多客户端**：UDP 广播支持多个接收端同时消费数据

---

## 2. 架构说明

```
┌────────────────────────────────────────────────┐
│              rtk_service 进程                   │
│                                                │
│  ┌──────────┐    ┌──────────┐   ┌──────────┐  │
│  │ 配置解析  │───▶│ RTK Core │──▶│ UDP广播  │  │
│  └──────────┘    └──────────┘   └──────────┘  │
│                       ▲               │        │
│  ┌──────────┐         │               │        │
│  │GPS串口   │─────────┘         端口: 9000     │
│  │Worker   │  GGA数据+RTCM发送                 │
│  └──────────┘                         │        │
└───────────────────────────────────────┼────────┘
                                        │ UDP广播
                    ┌───────────────────┼──────────────────┐
                    ▼                   ▼                  ▼
             ┌──────────┐        ┌──────────┐       ┌──────────┐
             │ 业务进程A │        │ 业务进程B │       │ 业务进程C │
             │ (C/C++)  │        │ (Python) │       │ (Node.js)│
             └──────────┘        └──────────┘       └──────────┘
```

**数据流说明：**

1. GPS 串口读取 NMEA 数据，提取 GGA 位置语句
2. GGA 发送至六分科技云端，获取 RTCM 差分数据
3. RTCM 差分数据回写 GPS 模块，实现 RTK 厘米级定位
4. GPS 定位结果（含 RTK fix 状态）通过 UDP 广播发送给所有监听端

---

## 3. 编译

### 3.1 ARM 交叉编译（推荐，用于嵌入式设备）

```bash
cd rtk_sdk
make -f Makefile.arm
```

指定工具链：

```bash
make -f Makefile.arm CROSS_COMPILE=aarch64-linux-gnu-
```

### 3.2 x86 本地编译（用于调试）

```bash
cd rtk_sdk
make
```

### 3.3 输出文件

| 文件 | 说明 |
|------|------|
| `build-arm/rtk_service` | ARM 独立进程可执行文件 |
| `build/rtk_service` | x86 独立进程可执行文件 |
| `lib-arm/librtk_sdk.so` | ARM 动态库（库模式用） |
| `lib-arm/librtk_sdk.a` | ARM 静态库（库模式用） |

---

## 4. 配置文件

### 4.1 创建配置文件

```bash
cp rtk_sdk.conf.example rtk_sdk.conf
# 编辑配置，填入鉴权信息
vi rtk_sdk.conf
```

建议将配置文件权限设为 600：

```bash
chmod 600 rtk_sdk.conf
```

### 4.2 配置文件完整说明

```ini
# RTK SDK 配置文件
# 格式: INI

[auth]
# 鉴权 AK（必填）- 六分科技平台 Access Key
ak = your_access_key

# 鉴权 AS（必填）- 六分科技平台 Access Secret
as = your_access_secret

# 设备 ID（必填）- 建议使用设备唯一标识，如 SN 号
device_id = DEVICE_001

# 设备类型（必填）
device_type = rtk_receiver

[network]
# 网络请求超时时间（秒），范围 1-60，默认 10
timeout = 10

# 是否使用 HTTPS，0=HTTP，1=HTTPS，默认 0
use_https = 0

[broadcast]
# UDP 广播端口，默认 9000
port = 9000

# 广播地址，默认 255.255.255.255（局域网全广播）
# 如需定向发送，可指定具体 IP（如 192.168.1.255）
address = 255.255.255.255

[log]
# 日志级别: 0=关闭, 1=错误, 2=警告, 3=信息, 4=调试
level = 3

# 日志文件路径（可选）
# 不配置时仅输出到控制台；守护进程模式建议配置文件路径
# file = /var/log/rtk_sdk.log

[gps_serial]
# GPS 串口设备路径（配置后启用 GPS 自动模式）
port = /dev/ttyUSB0

# 波特率，默认 115200
baudrate = 115200

# 自动模式开关
# 1=启用（自动读取 GPS 数据、发送差分、广播结果）
# 0=禁用（需外部程序通过 API 手动输入 GGA 数据）
auto_mode = 1
```

### 4.3 必填配置项

| 配置项 | 说明 |
|-------|------|
| `auth.ak` | 六分科技平台 Access Key |
| `auth.as` | 六分科技平台 Access Secret |
| `auth.device_id` | 设备唯一标识 |
| `auth.device_type` | 设备类型标识 |

> **安全提示**：AK/AS 是敏感信息，请勿将配置文件提交到代码仓库。

---

## 5. 启动与运行

### 5.1 基本启动

```bash
./rtk_service -c rtk_sdk.conf
```

### 5.2 命令行选项

```
用法: rtk_service [选项]

选项:
  -c, --config <file>     配置文件路径（默认: ./rtk_sdk.conf）
  -p, --port <port>       UDP 广播端口（覆盖配置文件，默认: 9000）
  -a, --address <addr>    广播地址（覆盖配置文件）
  -l, --log-level <0-4>   日志级别（覆盖配置文件）
  -d, --daemon            以守护进程模式运行
  -h, --help              显示帮助信息
  -v, --version           显示版本信息
```

### 5.3 常用启动示例

```bash
# 使用默认配置文件
./rtk_service

# 指定配置文件
./rtk_service -c /etc/rtk/rtk.conf

# 临时修改广播端口
./rtk_service -c rtk_sdk.conf -p 8888

# 调试模式（最详细日志）
./rtk_service -c rtk_sdk.conf -l 4

# 守护进程模式
./rtk_service -c rtk_sdk.conf -d
```

### 5.4 启动输出示例

```
RTK定位服务启动中...
版本: 1.0.0
[Config] 已加载配置: rtk_sdk.conf
[Config] AK: your_ak (已脱敏)
[Config] 广播: 255.255.255.255:9000
[Config] GPS串口: /dev/ttyUSB0 @ 115200
[STATUS] 状态: INIT, 六分SDK状态码: 0
[STATUS] 状态: CONNECTING, 六分SDK状态码: 0
[STATUS] 状态: RUNNING, 六分SDK状态码: 0
GPS自动模式已启动
服务已启动，输入 h 查看帮助，q 退出
[RTCM] 收到差分数据: 1024 字节
```

---

## 6. 工作模式

### 6.1 GPS 自动模式（推荐）

配置文件中设置 `gps_serial.auto_mode = 1` 且 `gps_serial.port` 不为空时启用。

**特点：**
- 自动从 GPS 串口读取 NMEA 数据
- 自动提取 GGA 语句并上传至差分服务
- 收到 RTCM 差分数据后自动写回 GPS 串口
- 自动解析 GPS 定位结果并通过 UDP 广播

```
配置示例:
[gps_serial]
port = /dev/ttyUSB0
baudrate = 115200
auto_mode = 1
```

**串口自动重连：**
- 串口连续 3 次读写错误时自动触发重连
- 最大重连次数：20 次，每次间隔 2 秒
- 重连期间每 5 秒广播一次心跳包（`gps_state=0`）
- 重连成功后重置计时器，给 GPS 模块 10 秒启动缓冲

**GPS 模块静默检测（新）：**
- 串口正常打开，但 GPS 模块超过 10 秒无任何 NMEA 输出时，切换为 `gps_state=3` 心跳广播
- 触发场景：GPS 模块掉电、固件异常、波特率不匹配等
- 重连成功或收到 NMEA 数据后自动恢复正常广播

**差分服务器自动重连（新）：**
- 差分服务运行中断开时自动触发重连，无需重启进程
- 采用指数退避策略，重连等待时间逐步增长（最长 3 小时封顶）
  - 第 1 次：3 秒，第 2 次：约 4.8 秒，第 3 次：约 7.7 秒，以此类推
- kill 进程后重启，重连计数从头开始

### 6.2 手动 GGA 输入模式

配置文件中设置 `gps_serial.auto_mode = 0` 时，rtk_service 不操作串口，仅通过 UDP 广播 RTCM 数据。业务进程需要通过其他方式（如独立进程间通信）向 SDK 输入 GGA 数据。

> 此模式通常配合库模式使用，独立进程模式下较少使用。

---

## 7. UDP 广播协议

### 7.1 广播参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 协议 | UDP | 用户数据报协议 |
| 端口 | 9000 | 可通过配置或命令行修改 |
| 地址 | 255.255.255.255 | 局域网全广播 |
| 最大包大小 | 2048 字节 | 超长 RTCM 帧会分包 |

### 7.2 数据包类型

`rtk_service` 广播两类数据包，通过首字节区分：

| 首字节 | 类型 | 说明 |
|--------|------|------|
| `0xD3` | RTCM3 二进制 | 原始差分数据，直接发送给 GPS 模块 |
| `{` | JSON 文本 | 定位结果 + 差分状态，**始终广播**（见 7.3） |

### 7.3 JSON 状态与定位包

#### 广播触发机制

| 场景 | 广播行为 | gps_state |
|------|---------|-----------|
| GPS 串口有数据（任意 fix 值） | 每条 GGA 触发一次广播 | 1 或 2 |
| GPS 串口断开 / 重连中 | 每 **5 秒**广播一次心跳包 | 0 |
| GPS 串口正常但模块无 NMEA 输出超过 10 秒 | 每 **5 秒**广播一次心跳包 | 3 |

> 接收端只要持续收到 JSON 包，就说明 `rtk_service` 进程仍在运行。若超过 **60 秒**无任何包，则应视为进程异常，触发重启。

#### 完整字段说明

```json
{
    "type":         "rtk_position",
    "diff_state":   2,
    "diff_err_msg": "",
    "gps_state":    2,
    "lat":          39.12345678,
    "lon":          116.12345678,
    "alt":          45.123,
    "hdop":         0.80,
    "fix":          4,
    "sat":          12,
    "ts":           1234567890123
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定值 `"rtk_position"` |
| `diff_state` | int | 差分服务状态（见下表） |
| `diff_err_msg` | string | 差分失败原因；正常时为 `""` |
| `gps_state` | int | GPS 串口状态（见下表） |
| `lat` | double | 纬度（度，WGS-84，负值为南纬）；`gps_state=0` 时为 0.0 |
| `lon` | double | 经度（度，WGS-84，负值为西经）；`gps_state=0` 时为 0.0 |
| `alt` | double | 海拔高度（米）；`gps_state=0` 时为 0.0 |
| `hdop` | double | 水平精度因子，越小越好；`gps_state=0` 时为 0.0 |
| `fix` | int | GPS 模块原始定位质量（见下表） |
| `sat` | int | 可见卫星数 |
| `ts` | int64 | 毫秒时间戳 |

#### `fix` 定位质量

| 值 | 含义 | 精度参考 |
|----|------|---------|
| 0 | 无定位（串口断开或无卫星） | - |
| 1 | GPS 单点定位 | 米级（3~10m） |
| 2 | DGPS 差分定位 | 亚米级（0.5~1m） |
| 4 | **RTK 固定解** | **厘米级（1~3cm）** |
| 5 | RTK 浮动解 | 分米级（0.1~0.5m） |

#### `diff_state` 差分服务状态

| 值 | 状态 | 说明 | 接收端建议 |
|----|------|------|-----------|
| 0 | 未启动 | 差分服务未初始化或已停止 | 等待 |
| 1 | 连接中 | 正在连接六分科技云端 | 使用 `fix=1` 单点数据，等待 RTK |
| 2 | 运行中 | 差分服务正常运行 | 按 `fix` 值决定精度 |
| 3 | 错误 | 差分服务异常，查看 `diff_err_msg` | 显示单点数据，提示用户 |

#### `diff_err_msg` 常见内容

| 内容 | 含义 |
|------|------|
| `""` | 无错误 |
| `"鉴权失败"` | AK/AS 错误或账号过期 |
| `"连接失败"` | 网络不通，无法连接云端 |
| `"超时"` | 60 秒未收到 GGA 或未获取 RTCM |
| `"六分SDK初始化失败"` | 底层 SDK 初始化异常 |

#### `gps_state` GPS 串口状态

| 值 | 含义 | 位置数据是否可用 |
|----|------|----------------|
| 0 | GPS 串口断开或重连中（心跳包） | **不可用**，lat/lon/alt 均为 0.0 |
| 1 | GPS 串口正常，搜星中（fix=0） | **不可用**，等待卫星锁定 |
| 2 | GPS 串口正常，有定位数据（fix≥1） | **可用**，精度由 `fix` 决定 |
| 3 | GPS 串口正常但模块无 NMEA 输出（心跳包） | **不可用**，lat/lon/alt 均为 0.0 |

> `gps_state=3` 触发条件：串口处于 OPEN 状态，但连续 **10 秒**未收到任何 NMEA 语句。常见原因：GPS 模块掉电、固件崩溃、波特率配置不匹配。

#### 典型场景示例

**场景 1：差分连接中，GPS 有单点定位**
```json
{"type":"rtk_position","diff_state":1,"diff_err_msg":"","gps_state":2,"lat":39.12345678,"lon":116.12345678,"alt":45.123,"hdop":1.20,"fix":1,"sat":8,"ts":...}
```
→ 显示单点位置，提示"RTK 连接中..."

**场景 2：差分正常，RTK 固定解**
```json
{"type":"rtk_position","diff_state":2,"diff_err_msg":"","gps_state":2,"lat":39.12345678,"lon":116.12345678,"alt":45.123,"hdop":0.80,"fix":4,"sat":12,"ts":...}
```
→ 使用 RTK 厘米级数据

**场景 3：差分鉴权失败，GPS 有单点定位**
```json
{"type":"rtk_position","diff_state":3,"diff_err_msg":"鉴权失败","gps_state":2,"lat":39.12345678,"lon":116.12345678,"alt":45.123,"hdop":1.20,"fix":1,"sat":8,"ts":...}
```
→ 显示单点位置，提示"鉴权失败，请检查 AK/AS"

**场景 4：GPS 串口断开（心跳包）**
```json
{"type":"rtk_position","diff_state":1,"diff_err_msg":"","gps_state":0,"lat":0.0,"lon":0.0,"alt":0.0,"hdop":0.0,"fix":0,"sat":0,"ts":...}
```
→ 提示"GPS 串口断开"，不使用位置数据

**场景 5：GPS 已连，搜星中（fix=0）**
```json
{"type":"rtk_position","diff_state":2,"diff_err_msg":"","gps_state":1,"lat":0.0,"lon":0.0,"alt":0.0,"hdop":0.0,"fix":0,"sat":3,"ts":...}
```
→ 提示"GPS 搜星中"，等待卫星锁定

**场景 6：GPS 模块静默（串口正常但 10 秒无 NMEA 输出）**
```json
{"type":"rtk_position","diff_state":2,"diff_err_msg":"","gps_state":3,"lat":0.0,"lon":0.0,"alt":0.0,"hdop":0.0,"fix":0,"sat":0,"ts":...}
```
→ 提示"GPS 模块无响应，请检查 GPS 硬件"，不使用位置数据

---

## 8. 接收端集成

### 8.1 C/C++ 接收示例

```c
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define RTK_BROADCAST_PORT  9000
#define BUF_SIZE            2048

int main() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    /* 允许多个进程绑定同一端口 */
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(RTK_BROADCAST_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return -1;
    }

    printf("监听 UDP 端口 %d ...\n", RTK_BROADCAST_PORT);

    uint8_t buf[BUF_SIZE];
    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if (len <= 0) continue;
        buf[len] = '\0';

        if (buf[0] == 0xD3) {
            /* RTCM3 差分数据：直接透传给 GPS 模块 */
            printf("[RTCM] %d 字节\n", len);
            // send_to_gps_serial(buf, len);
        } else if (buf[0] == '{') {
            /* JSON 状态+定位包：解析新字段 */
            int gps_state = 0, diff_state = 0, fix = 0;
            double lat = 0, lon = 0;
            char diff_err_msg[128] = "";

            /* 简单 sscanf 示例，生产环境建议用 JSON 库 */
            sscanf((char*)buf,
                "{\"type\":\"rtk_position\","
                "\"diff_state\":%d,"
                "\"diff_err_msg\":\"%127[^\"]\","
                "\"gps_state\":%d,"
                "\"lat\":%lf,"
                "\"lon\":%lf,"
                "%*[^}]"
                "\"fix\":%d,",
                &diff_state, diff_err_msg, &gps_state, &lat, &lon, &fix);

            if (gps_state == 0) {
                printf("[HEARTBEAT] GPS串口断开, diff_state=%d\n", diff_state);
            } else if (gps_state == 3) {
                printf("[HEARTBEAT] GPS模块无响应(静默), diff_state=%d\n", diff_state);
            } else if (gps_state == 1) {
                printf("[GPS] 搜星中, diff_state=%d\n", diff_state);
            } else {
                /* gps_state == 2：有定位数据 */
                if (diff_state == 3) {
                    printf("[WARN] 差分失败: %s, 使用单点定位 lat=%.6f lon=%.6f\n",
                           diff_err_msg, lat, lon);
                } else if (fix == 4) {
                    printf("[RTK] 固定解 lat=%.8f lon=%.8f\n", lat, lon);
                } else {
                    printf("[GPS] fix=%d lat=%.6f lon=%.6f diff_state=%d\n",
                           fix, lat, lon, diff_state);
                }
            }
        }
    }
    return 0;
}
```

### 8.2 Python 接收示例

```python
import socket
import json

RTK_PORT = 9000

def receive_rtk():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(('', RTK_PORT))
    print(f'监听 UDP 端口 {RTK_PORT} ...')

    while True:
        data, addr = sock.recvfrom(2048)

        if data[0] == 0xD3:
            print(f'[RTCM] {len(data)} 字节差分数据')
            # send_to_gps(data)
            continue

        try:
            pkt = json.loads(data.decode('utf-8'))
        except Exception:
            continue

        if pkt.get('type') != 'rtk_position':
            continue

        gps_state   = pkt.get('gps_state',   0)
        diff_state  = pkt.get('diff_state',  0)
        diff_err    = pkt.get('diff_err_msg', '')
        fix         = pkt.get('fix', 0)
        lat         = pkt.get('lat', 0.0)
        lon         = pkt.get('lon', 0.0)

        if gps_state == 0:
            print(f'[HEARTBEAT] GPS串口断开, diff_state={diff_state}')
        elif gps_state == 3:
            print(f'[HEARTBEAT] GPS模块无响应(静默), diff_state={diff_state}')
        elif gps_state == 1:
            print(f'[GPS] 搜星中, diff_state={diff_state}')
        else:
            # gps_state == 2：有定位数据
            if diff_state == 3:
                print(f'[WARN] 差分失败({diff_err})，单点: {lat:.6f}, {lon:.6f}')
            elif fix == 4:
                print(f'[RTK] 固定解 {lat:.8f}, {lon:.8f}')
            else:
                print(f'[GPS] fix={fix} {lat:.6f},{lon:.6f} diff_state={diff_state}')

if __name__ == '__main__':
    receive_rtk()
```

### 8.3 多客户端说明

由于使用 UDP 广播，同一局域网内多个进程可以同时接收数据：

```bash
# 进程 A 和进程 B 同时绑定 9000 端口（需要设置 SO_REUSEADDR/SO_REUSEPORT）
# rtk_service 发送一次，两个进程都能收到
```

> **注意**：确保接收端程序设置了 `SO_REUSEADDR` 和 `SO_REUSEPORT`，否则多进程绑定同一端口会失败。

---

## 9. 交互命令

`rtk_service` 非守护进程模式下支持终端交互命令：

| 命令 | 说明 |
|------|------|
| `s` 或 `status` | 显示当前运行状态和广播状态 |
| `h` 或 `help` | 显示帮助信息 |
| `q` 或 `quit` 或 `exit` | 优雅退出（等同于 Ctrl+C） |

**使用示例：**

```
服务已启动，输入 h 查看帮助，q 退出
> s
当前状态: RUNNING
广播状态: 已启用
> q
正在停止服务...
服务已停止
```

---

## 10. 守护进程模式

### 10.1 启动守护进程

```bash
./rtk_service -c rtk_sdk.conf -d
```

守护进程模式下：
- 进程脱离终端运行
- 标准输入/输出关闭
- **建议配置日志文件**，否则日志丢失

```ini
[log]
level = 3
file = /var/log/rtk_sdk.log
```

### 10.2 管理守护进程

**查看进程：**
```bash
ps aux | grep rtk_service
```

**停止服务：**
```bash
kill -TERM $(pgrep rtk_service)
# 或
kill -INT $(pgrep rtk_service)
```

### 10.3 systemd 服务配置（推荐）

创建服务文件 `/etc/systemd/system/rtk-service.service`：

```ini
[Unit]
Description=RTK Positioning Service
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/rtk_service -c /etc/rtk/rtk_sdk.conf
Restart=on-failure
RestartSec=5s
User=root

[Install]
WantedBy=multi-user.target
```

注册并启动：

```bash
systemctl daemon-reload
systemctl enable rtk-service
systemctl start rtk-service
systemctl status rtk-service
```

---

## 11. 日志管理

### 11.1 日志级别

| 级别 | 值 | 说明 |
|------|----|------|
| 关闭 | 0 | 无任何日志输出 |
| 错误 | 1 | 仅输出错误信息 |
| 警告 | 2 | 错误 + 警告 |
| 信息 | 3 | 错误 + 警告 + 关键流程信息（推荐） |
| 调试 | 4 | 全量日志（排障用） |

### 11.2 日志轮转（logrotate）

```bash
# /etc/logrotate.d/rtk-service
/var/log/rtk_sdk.log {
    daily
    rotate 7
    compress
    missingok
    notifempty
    postrotate
        kill -HUP $(pgrep rtk_service) 2>/dev/null || true
    endscript
}
```

---

## 12. 常见问题

### Q1: 启动失败：加载配置文件失败

```
加载配置文件失败: ./rtk_sdk.conf (错误: -303)
```

**原因**：配置文件不存在或路径错误。

**解决**：
```bash
cp rtk_sdk.conf.example rtk_sdk.conf
# 编辑填写 AK/AS 等信息
vi rtk_sdk.conf
```

---

### Q2: 状态停留在 CONNECTING，长时间不变为 RUNNING

**原因**：鉴权失败或网络不通。

**排查步骤**：
1. 检查 `ak`/`as` 是否填写正确，注意首尾空格
2. 确认账号未过期，在平台控制台验证
3. 检查设备网络是否能访问外网：`ping 8.8.8.8`
4. 将日志级别调至 4（调试），查看详细错误：`-l 4`

---

### Q3: 收不到 UDP 广播数据

**排查步骤**：

1. 确认 `rtk_service` 日志中显示"收到差分数据"（说明已获取 RTCM）
2. 确认接收端与 `rtk_service` 在同一局域网
3. 检查防火墙是否放行 UDP 9000 端口：
   ```bash
   # Linux
   iptables -I INPUT -p udp --dport 9000 -j ACCEPT
   ```
4. 确认接收端绑定了正确端口，且设置了 `SO_REUSEADDR`
5. 使用工具抓包验证广播是否发出：
   ```bash
   tcpdump -i any udp port 9000
   ```

---

### Q4: GPS 串口无法打开

```
警告: GPS自动模式启动失败: ...
```

**排查步骤**：
1. 确认串口路径正确：`ls /dev/ttyUSB*`
2. 检查当前用户权限：`ls -l /dev/ttyUSB0`
3. 将用户加入 `dialout` 组：`sudo usermod -aG dialout $USER`
4. 确认串口未被其他程序占用：`fuser /dev/ttyUSB0`

---

### Q5: 定位 fix 值一直为 1 或 2，无法达到 RTK 固定解（fix=4）

**排因**：
- GGA 未发送或发送频率过低（建议每秒 1 次）
- 所在位置不在差分服务覆盖范围
- GPS 天线信号弱，卫星数不足（建议 ≥ 8 颗）
- 基准站距离过远（一般要求 ≤ 30km）

---

### Q6: 多个进程同时绑定 UDP 端口失败

**解决**：接收端 socket 需设置选项：

```c
int reuse = 1;
setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
```

---

## 附录：快速验证清单

部署完成后，按以下步骤逐项验证：

- [ ] `rtk_service -v` 显示版本号
- [ ] 使用 `-c` 指定配置文件，服务正常启动
- [ ] 日志中出现 `状态: RUNNING`，表明鉴权和连接成功
- [ ] 日志中出现 `收到差分数据`，表明 RTCM 获取正常
- [ ] 接收端能收到 UDP 广播数据包
- [ ] JSON 定位结果中 `fix` 字段最终稳定在 4（RTK 固定解）
