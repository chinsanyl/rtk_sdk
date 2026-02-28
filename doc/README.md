# RTK SDK 使用说明

## 简介

RTK SDK是基于六分科技差分SDK封装的高精度定位库，支持获取RTCM差分数据实现厘米级RTK定位。

## 目录结构

```
rtk_sdk/
├── include/
│   └── rtk_sdk.h           # 公开API头文件
├── src/
│   ├── rtk_core.c          # 核心引擎
│   ├── rtk_log.c           # 日志模块
│   ├── rtk_gga.c           # GGA处理
│   ├── rtk_broadcast.c     # UDP广播
│   ├── rtk_config.c        # 配置解析
│   └── rtk_internal.h      # 内部头文件
├── main/
│   └── rtk_main.c          # 进程入口
├── doc/
│   ├── API.md              # API文档
│   └── README.md           # 本文件
├── Makefile                # 构建脚本
├── Makefile.arm            # ARM交叉编译
└── rtk_sdk.conf.example    # 配置示例
```

## 编译

### 本地编译（x86测试）
```bash
cd rtk_sdk
make
```

### ARM交叉编译
```bash
make -f Makefile.arm
# 或指定工具链
make -f Makefile.arm CROSS_COMPILE=aarch64-linux-gnu-
```

### 输出文件
- `lib/librtk_sdk.so` - 动态库
- `lib/librtk_sdk.a` - 静态库
- `build/rtk_service` - 独立进程

## 运行（进程模式）

### 1. 准备配置文件
```bash
cp rtk_sdk.conf.example rtk_sdk.conf
# 编辑配置文件，填入AK/AS等信息
```

### 2. 启动服务
```bash
./build/rtk_service -c rtk_sdk.conf
```

### 3. 命令行选项
```
-c, --config <file>     配置文件路径
-p, --port <port>       UDP广播端口
-l, --log-level <0-4>   日志级别
-d, --daemon            守护进程模式
-h, --help              帮助
```

## 集成（库模式）

### 1. 链接库
```makefile
LDFLAGS += -L/path/to/rtk_sdk/lib -lrtk_sdk -lsixents-core-sdk -lpthread
```

### 2. 代码示例
```c
#include "rtk_sdk.h"

void on_rtcm(const uint8_t *data, int len, void *user) {
    // 收到差分数据，发送给GPS模块
    send_to_gps(data, len);
}

int main() {
    rtk_config_t cfg = {0};
    strcpy(cfg.ak, "your_ak");
    strcpy(cfg.as, "your_as");
    strcpy(cfg.device_id, "dev001");
    strcpy(cfg.device_type, "test");
    
    rtk_sdk_init(&cfg);
    rtk_sdk_set_rtcm_callback(on_rtcm, NULL);
    rtk_sdk_start();
    
    // 主循环：定期输入GGA
    while (1) {
        rtk_sdk_input_gga(gga_str, strlen(gga_str));
        sleep(1);
    }
    
    rtk_sdk_stop();
    rtk_sdk_deinit();
    return 0;
}
```

## UDP广播协议

进程模式下，RTK SDK通过UDP广播发送RTCM差分数据：

- **协议**：UDP广播
- **默认端口**：9000
- **默认地址**：255.255.255.255
- **数据格式**：原始RTCM3二进制数据

### 接收端示例
```c
int sock = socket(AF_INET, SOCK_DGRAM, 0);
struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port = htons(9000),
    .sin_addr.s_addr = INADDR_ANY
};
bind(sock, (struct sockaddr*)&addr, sizeof(addr));

uint8_t buf[2048];
while (1) {
    int len = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
    if (len > 0) {
        // 收到RTCM数据
        send_to_gps_module(buf, len);
    }
}
```

## 常见问题

### 1. 鉴权失败
- 检查AK/AS是否正确
- 确认账号未过期
- 检查网络连接

### 2. 无法获取差分数据
- 确保定期发送有效GGA数据
- 检查GGA位置是否在服务范围内
- 查看日志排查问题

### 3. 编译链接错误
- 确认六分SDK路径正确
- 检查库文件是否存在
