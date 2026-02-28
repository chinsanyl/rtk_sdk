/**
 * @file rtk_main.c
 * @brief RTK SDK独立进程入口
 * 
 * @details 提供命令行参数解析、配置文件加载、信号处理
 *          编译为 rtk_service 可执行文件
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <sys/select.h>
#include <fcntl.h>

#include "rtk_sdk.h"

/* 配置相关声明 */
extern int rtk_config_load(rtk_config_t *config, const char *filepath);
extern int rtk_config_validate(const rtk_config_t *config);
extern void rtk_config_dump(const rtk_config_t *config);

/* GPS工作线程声明 */
extern int rtk_gps_worker_start(const char *port, int baudrate);
extern void rtk_gps_worker_stop(void);
extern int rtk_gps_worker_is_running(void);

/* ============================================================================
 * 全局变量
 * ========================================================================== */
static volatile int g_running = 1;
static int g_stdin_fd = -1;

/* ============================================================================
 * 信号处理
 * ========================================================================== */

static void signal_handler(int sig) {
    printf("\n收到信号 %d，正在退出...\n", sig);
    g_running = 0;
}

/* ============================================================================
 * 命令行帮助
 * ========================================================================== */

static void print_usage(const char *program) {
    printf("RTK定位服务 %s\n", rtk_sdk_get_version());
    printf("\n用法: %s [选项]\n", program);
    printf("\n选项:\n");
    printf("  -c, --config <file>     配置文件路径 (默认: ./rtk_sdk.conf)\n");
    printf("  -p, --port <port>       UDP广播端口 (默认: 9000)\n");
    printf("  -a, --address <addr>    广播地址 (默认: 255.255.255.255)\n");
    printf("  -l, --log-level <0-4>   日志级别 (0=关闭, 4=调试)\n");
    printf("  -d, --daemon            以守护进程运行\n");
    printf("  -h, --help              显示帮助信息\n");
    printf("  -v, --version           显示版本信息\n");
    printf("\n配置文件格式 (INI):\n");
    printf("  [auth]\n");
    printf("  ak = your_ak\n");
    printf("  as = your_as\n");
    printf("  device_id = your_device_id\n");
    printf("  device_type = your_device_type\n");
    printf("\n  [broadcast]\n");
    printf("  port = 9000\n");
    printf("  address = 255.255.255.255\n");
    printf("\n交互命令:\n");
    printf("  q/quit    退出程序\n");
    printf("  s/status  显示状态\n");
    printf("  h/help    显示帮助\n");
}

/* ============================================================================
 * 回调函数
 * ========================================================================== */

static void on_rtcm_data(const uint8_t *data, int len, void *user_data) {
    (void)user_data;
    printf("[RTCM] 收到差分数据: %d 字节\n", len);
}

static void on_status_change(rtk_state_t state, int status_code, void *user_data) {
    (void)user_data;
    const char *state_names[] = {"IDLE", "INIT", "CONNECTING", "RUNNING", "STOPPING", "ERROR"};
    printf("[STATUS] 状态: %s, 六分SDK状态码: %d\n", 
           state < sizeof(state_names)/sizeof(state_names[0]) ? state_names[state] : "UNKNOWN",
           status_code);
}

static void on_error(const rtk_error_info_t *err, void *user_data) {
    (void)user_data;
    printf("[ERROR] 错误: %d - %s (可恢复: %s)\n", 
           err->code, err->msg, err->recoverable ? "是" : "否");
}

/* ============================================================================
 * 交互命令处理
 * ========================================================================== */

static void process_command(const char *cmd) {
    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
        g_running = 0;
    } else if (strcmp(cmd, "s") == 0 || strcmp(cmd, "status") == 0) {
        rtk_state_t state = rtk_sdk_get_state();
        const char *state_names[] = {"IDLE", "INIT", "CONNECTING", "RUNNING", "STOPPING", "ERROR"};
        printf("当前状态: %s\n", state_names[state]);
        printf("广播状态: %s\n", rtk_sdk_is_broadcast_enabled() ? "已启用" : "未启用");
    } else if (strcmp(cmd, "h") == 0 || strcmp(cmd, "help") == 0) {
        printf("可用命令:\n");
        printf("  q/quit    退出程序\n");
        printf("  s/status  显示状态\n");
        printf("  h/help    显示帮助\n");
    } else if (strlen(cmd) > 0) {
        printf("未知命令: %s (输入 h 查看帮助)\n", cmd);
    }
}

/* ============================================================================
 * 主函数
 * ========================================================================== */

int main(int argc, char *argv[]) {
    int ret = 0;
    rtk_config_t config;
    const char *config_file = "./rtk_sdk.conf";
    int daemon_mode = 0;
    int override_port = 0;
    int override_log_level = -1;
    char override_addr[64] = {0};
    
    /* 命令行参数定义 */
    static struct option long_options[] = {
        {"config",    required_argument, 0, 'c'},
        {"port",      required_argument, 0, 'p'},
        {"address",   required_argument, 0, 'a'},
        {"log-level", required_argument, 0, 'l'},
        {"daemon",    no_argument,       0, 'd'},
        {"help",      no_argument,       0, 'h'},
        {"version",   no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };
    
    /* 解析命令行参数 */
    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:a:l:dhv", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'p':
                override_port = atoi(optarg);
                break;
            case 'a':
                strncpy(override_addr, optarg, sizeof(override_addr) - 1);
                break;
            case 'l':
                override_log_level = atoi(optarg);
                break;
            case 'd':
                daemon_mode = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                printf("RTK SDK 版本: %s\n", rtk_sdk_get_version());
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 守护进程模式 */
    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            perror("daemon");
            return 1;
        }
    }
    
    printf("RTK定位服务启动中...\n");
    printf("版本: %s\n", rtk_sdk_get_version());
    
    /* 加载配置文件 */
    ret = rtk_config_load(&config, config_file);
    if (ret != RTK_OK) {
        fprintf(stderr, "加载配置文件失败: %s (错误: %d)\n", config_file, ret);
        fprintf(stderr, "请创建配置文件或使用 -c 指定路径\n");
        return 1;
    }
    
    /* 命令行参数覆盖 */
    if (override_port > 0) {
        config.broadcast_port = override_port;
    }
    if (strlen(override_addr) > 0) {
        strncpy(config.broadcast_addr, override_addr, sizeof(config.broadcast_addr) - 1);
    }
    if (override_log_level >= 0) {
        config.log_level = (rtk_log_level_t)override_log_level;
    }
    
    /* 验证配置 */
    ret = rtk_config_validate(&config);
    if (ret != RTK_OK) {
        fprintf(stderr, "配置验证失败: %d\n", ret);
        return 1;
    }
    
    /* 打印配置 */
    rtk_config_dump(&config);
    
    /* 初始化SDK */
    ret = rtk_sdk_init(&config);
    if (ret != RTK_OK) {
        fprintf(stderr, "SDK初始化失败: %s (%d)\n", rtk_strerror(ret), ret);
        return 1;
    }
    
    /* 注册回调（调试用） */
    rtk_sdk_set_rtcm_callback(on_rtcm_data, NULL);
    rtk_sdk_set_status_callback(on_status_change, NULL);
    rtk_sdk_set_error_callback(on_error, NULL);
    
    /* 启用UDP广播 */
    ret = rtk_sdk_enable_broadcast(config.broadcast_port, config.broadcast_addr);
    if (ret != RTK_OK) {
        fprintf(stderr, "启用广播失败: %s (%d)\n", rtk_strerror(ret), ret);
        /* 继续运行，回调仍可用 */
    }
    
    /* 启动服务 */
    ret = rtk_sdk_start();
    if (ret != RTK_OK) {
        fprintf(stderr, "启动服务失败: %s (%d)\n", rtk_strerror(ret), ret);
        rtk_sdk_deinit();
        return 1;
    }
    
    /* 启动GPS自动模式（如果配置了） */
    if (config.gps_auto_mode && strlen(config.gps_serial_port) > 0) {
        printf("启动GPS自动模式: %s @ %d\n", 
               config.gps_serial_port,
               config.gps_serial_baudrate > 0 ? config.gps_serial_baudrate : 115200);
        ret = rtk_gps_worker_start(config.gps_serial_port, config.gps_serial_baudrate);
        if (ret != RTK_OK) {
            fprintf(stderr, "警告: GPS自动模式启动失败: %s (%d)\n", rtk_strerror(ret), ret);
            /* 继续运行，差分服务仍可用 */
        } else {
            printf("GPS自动模式已启动\n");
        }
    }
    
    printf("服务已启动，输入 h 查看帮助，q 退出\n");
    
    /* 设置stdin非阻塞 */
    if (!daemon_mode) {
        g_stdin_fd = STDIN_FILENO;
        int flags = fcntl(g_stdin_fd, F_GETFL, 0);
        fcntl(g_stdin_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    /* 主循环 */
    char cmd_buf[256];
    while (g_running) {
        /* 使用select监听stdin */
        if (!daemon_mode) {
            fd_set rfds;
            struct timeval tv;
            
            FD_ZERO(&rfds);
            FD_SET(g_stdin_fd, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ready = select(g_stdin_fd + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET(g_stdin_fd, &rfds)) {
                if (fgets(cmd_buf, sizeof(cmd_buf), stdin) != NULL) {
                    /* 去除换行符 */
                    cmd_buf[strcspn(cmd_buf, "\r\n")] = '\0';
                    process_command(cmd_buf);
                }
            }
        } else {
            sleep(1);
        }
    }
    
    /* 清理 */
    printf("正在停止服务...\n");
    
    /* 停止GPS自动模式 */
    if (rtk_gps_worker_is_running()) {
        rtk_gps_worker_stop();
    }
    
    rtk_sdk_stop();
    rtk_sdk_deinit();
    
    printf("服务已停止\n");
    return 0;
}
