/**
 * @file rtk_internal.h
 * @brief RTK SDK内部头文件
 */

#ifndef RTK_INTERNAL_H
#define RTK_INTERNAL_H

#include "rtk_sdk.h"
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ============================================================================
 * 内部常量定义
 * ========================================================================== */
#define RTK_TICK_INTERVAL_MS            200     /* SDK Tick间隔（毫秒） */
#define RTK_GGA_INTERVAL_MS             1000    /* GGA发送间隔（毫秒） */
#define RTK_GPS_HEARTBEAT_INTERVAL_MS   5000    /* GPS串口断开/无数据时心跳广播间隔（毫秒） */
#define RTK_GPS_NO_DATA_TIMEOUT_MS      10000   /* GPS串口正常但模块无数据超时（毫秒），触发gps_state=3 */

/* 差分服务器重连退避参数（delay = BASE * FACTOR^(n-1)，上限 MAX） */
#define RTK_RECONNECT_BASE_MS           3000    /* 首次重连等待时间（毫秒） */
#define RTK_RECONNECT_FACTOR            160     /* 每次增长倍率（160 = ×1.6，整数计算） */
#define RTK_RECONNECT_MAX_MS            10800000/* 最长重连等待时间（毫秒，3小时） */
#define RTK_TICK_ERROR_THRESHOLD        10      /* 连续Tick失败次数阈值，触发重连 */

/* ============================================================================
 * 内部数据结构
 * ========================================================================== */

/**
 * @brief RTK SDK全局上下文
 */
typedef struct {
    /* 状态 */
    rtk_state_t state;                  /* 当前状态 */
    int initialized;                    /* 初始化标志 */
    int running;                        /* 运行标志 */
    
    /* 配置 */
    rtk_config_t config;                /* 配置参数副本 */
    
    /* 线程 */
    pthread_t worker_thread;            /* 工作线程 */
    int worker_running;                 /* 工作线程运行标志 */
    
    /* 互斥锁 */
    pthread_mutex_t state_mutex;        /* 状态锁 */
    pthread_mutex_t callback_mutex;     /* 回调锁 */
    pthread_mutex_t gga_mutex;          /* GGA锁 */
    pthread_mutex_t broadcast_mutex;    /* 广播锁 */
    
    /* GGA缓冲区 */
    char gga_buffer[RTK_GGA_BUF_SIZE];  /* GGA数据缓冲 */
    int gga_len;                        /* GGA数据长度 */
    int gga_ready;                      /* GGA数据就绪标志 */
    
    /* 回调函数（线程库模式） */
    rtk_rtcm_callback_t rtcm_cb;        /* RTCM回调 */
    void *rtcm_cb_data;                 /* RTCM回调用户数据 */
    rtk_status_callback_t status_cb;    /* 状态回调 */
    void *status_cb_data;               /* 状态回调用户数据 */
    rtk_error_callback_t error_cb;      /* 错误回调 */
    void *error_cb_data;                /* 错误回调用户数据 */
    rtk_log_callback_t log_cb;          /* 日志回调 */
    void *log_cb_data;                  /* 日志回调用户数据 */
    
    /* UDP广播（进程模式） */
    int broadcast_enabled;              /* 广播启用标志 */
    int broadcast_socket;               /* 广播socket */
    struct sockaddr_in broadcast_addr;  /* 广播地址 */
    
    /* 六分SDK状态 */
    int sixents_init;                   /* 六分SDK初始化标志 */
    int sixents_started;                /* 六分SDK启动标志 */
    
    /* 统计信息 */
    uint64_t rtcm_recv_count;           /* 接收RTCM计数 */
    uint64_t rtcm_recv_bytes;           /* 接收RTCM字节数 */
    uint64_t gga_send_count;            /* 发送GGA计数 */

    /* 最近一次差分错误信息（用于UDP广播状态字段） */
    int      last_error_code;           /* 最近一次错误码，0表示无错误 */
    char     last_error_msg[128];       /* 最近一次错误描述，连接恢复后自动清零 */
} rtk_context_t;

/* ============================================================================
 * 全局上下文
 * ========================================================================== */
extern rtk_context_t g_rtk_ctx;

/* ============================================================================
 * 内部函数声明
 * ========================================================================== */

/* 日志模块 */
void rtk_log(rtk_log_level_t level, const char *fmt, ...);
#define RTK_LOGE(fmt, ...) rtk_log(RTK_LOG_ERROR, fmt, ##__VA_ARGS__)
#define RTK_LOGW(fmt, ...) rtk_log(RTK_LOG_WARN,  fmt, ##__VA_ARGS__)
#define RTK_LOGI(fmt, ...) rtk_log(RTK_LOG_INFO,  fmt, ##__VA_ARGS__)
#define RTK_LOGD(fmt, ...) rtk_log(RTK_LOG_DEBUG, fmt, ##__VA_ARGS__)

/* 广播模块 */
int rtk_broadcast_init(int port, const char *addr);
void rtk_broadcast_deinit(void);
int rtk_broadcast_send(const uint8_t *data, int len);

/* GGA处理模块 */
int rtk_gga_validate(const char *gga_str, int len);
int rtk_gga_build(char *buf, int buf_size, double lat, double lon, double alt);

/* 回调分发模块 */
void rtk_dispatch_rtcm(const uint8_t *data, int len);
void rtk_dispatch_status(rtk_state_t state, int status_code);
void rtk_dispatch_error(int code, const char *msg, int recoverable);

/* 工作线程 */
void* rtk_worker_thread(void *arg);

/* 六分SDK回调 */
void rtk_sixents_diff_callback(const char *buff, unsigned int len);
void rtk_sixents_status_callback(unsigned int status);
int rtk_sixents_log_callback(const char *buff, unsigned short len);

/* 工具函数 */
int rtk_safe_strcpy(char *dst, size_t dst_size, const char *src);
uint64_t rtk_get_time_ms(void);
void rtk_sleep_ms(int ms);

#endif /* RTK_INTERNAL_H */
