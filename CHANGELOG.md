# Changelog

本文件记录 RTK SDK 的所有版本变更。

版本格式遵循 [语义化版本](https://semver.org/lang/zh-CN/)：`主版本.次版本.修订版本`
- **主版本**：不兼容的 API/协议变更
- **次版本**：向后兼容的功能新增
- **修订版本**：向后兼容的问题修复

---

## [v1.1.0] - 2026-02-28

### 新增
- UDP 广播协议新增 `diff_state` 字段：差分服务状态（0未启动 1连接中 2运行中 3错误）
- UDP 广播协议新增 `diff_err_msg` 字段：差分失败原因，正常时为空字符串
- UDP 广播协议新增 `gps_state` 字段：GPS 串口状态（0串口断开 1已连接无信号 2已连接有定位）
- GPS 串口断开时每 5 秒发送心跳广播包（`gps_state=0`），接收端可感知服务运行状态
- `fix=0`（无卫星信号）时也会触发 UDP 广播，`gps_state=1`
- `rtk_context_t` 新增 `last_error_code` / `last_error_msg` 字段，记录最近一次差分错误

### 变更
- `broadcast_position()` 重写：所有 fix 值均广播，仅 fix>0 时触发回调和计数
- GPS 串口读取流程调整：先解析 GGA 获取 `fix_quality`，再决定是否上报给差分 SDK
- 移除 UDP 广播中冗余的 `src` 字段（其值与 `fix` 字段完全相同）

### 文档
- 新增 `doc/RTK_Service_Process_Guide.md`：独立进程模式使用文档
- 更新 UDP 广播协议说明，新增 5 种场景示例，新增 Python 接收示例

---

## [v1.0.0] - 2026-02-04

### 新增
- RTK SDK 初始版本
- 支持线程库模式和独立进程（`rtk_service`）两种使用方式
- 六分科技差分 SDK 集成
- GPS 串口 NMEA/GGA 解析
- UDP 广播定位数据（JSON 格式）
- 自动重连机制（断网后指数退避重试，最多 5 次）
- 配置文件支持（`rtk_sdk.conf`）
- 后台守护进程模式（`-d` 启动参数）
