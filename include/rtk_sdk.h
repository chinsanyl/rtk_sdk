/**
 * @file rtk_sdk.h
 * @brief RTK定位SDK公开API头文件
 * @author RTK SDK Team
 * @version 1.0.0
 * @date 2026-02-04
 * 
 * @details 基于六分科技差分SDK封装的RTK定位SDK
 *          支持独立进程模式（UDP广播）和线程库模式（回调）
 */

#ifndef RTK_SDK_H
#define RTK_SDK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 版本信息
 * ========================================================================== */
#define RTK_SDK_VERSION_MAJOR   1
#define RTK_SDK_VERSION_MINOR   0
#define RTK_SDK_VERSION_PATCH   0
#define RTK_SDK_VERSION_STR     "1.0.0"

/* ============================================================================
 * 缓冲区大小定义
 * ========================================================================== */
#define RTK_AK_MAX_LEN          16      /* AK最大长度 */
#define RTK_AS_MAX_LEN          128     /* AS最大长度 */
#define RTK_DEVICE_ID_MAX_LEN   128     /* 设备ID最大长度 */
#define RTK_DEVICE_TYPE_MAX_LEN 32      /* 设备类型最大长度 */
#define RTK_GGA_BUF_SIZE        256     /* GGA缓冲区大小 */
#define RTK_RTCM_BUF_SIZE       2048    /* RTCM缓冲区大小 */
#define RTK_LOG_BUF_SIZE        512     /* 日志缓冲区大小 */
#define RTK_IP_ADDR_MAX_LEN     64      /* IP地址最大长度 */

/* ============================================================================
 * 错误码定义
 * ========================================================================== */
typedef enum {
    RTK_OK = 0,                         /* 成功 */
    
    /* 参数错误 (-1 ~ -99) */
    RTK_ERR_INVALID_PARAM = -1,         /* 无效参数 */
    RTK_ERR_NULL_PTR = -2,              /* 空指针 */
    RTK_ERR_BUFFER_OVERFLOW = -3,       /* 缓冲区溢出 */
    RTK_ERR_INVALID_GGA = -4,           /* 无效GGA数据 */
    
    /* 状态错误 (-100 ~ -199) */
    RTK_ERR_NOT_INIT = -100,            /* 未初始化 */
    RTK_ERR_ALREADY_INIT = -101,        /* 重复初始化 */
    RTK_ERR_NOT_RUNNING = -102,         /* 未运行 */
    RTK_ERR_ALREADY_RUNNING = -103,     /* 已运行 */
    
    /* 网络错误 (-200 ~ -299) */
    RTK_ERR_AUTH_FAILED = -200,         /* 鉴权失败 */
    RTK_ERR_CONNECT_FAILED = -201,      /* 连接失败 */
    RTK_ERR_TIMEOUT = -202,             /* 超时 */
    RTK_ERR_SOCKET = -203,              /* Socket错误 */
    RTK_ERR_BROADCAST = -204,           /* 广播错误 */
    
    /* 系统错误 (-300 ~ -399) */
    RTK_ERR_THREAD = -300,              /* 线程创建失败 */
    RTK_ERR_MUTEX = -301,               /* 互斥锁错误 */
    RTK_ERR_MEMORY = -302,              /* 内存分配失败 */
    RTK_ERR_FILE = -303,                /* 文件操作失败 */
    
    /* 六分SDK错误 (-400 ~ -499) */
    RTK_ERR_SIXENTS_INIT = -400,        /* 六分SDK初始化失败 */
    RTK_ERR_SIXENTS_START = -401,       /* 六分SDK启动失败 */
    RTK_ERR_SIXENTS_SEND = -402,        /* 六分SDK发送失败 */
} rtk_error_t;

/* ============================================================================
 * 状态枚举
 * ========================================================================== */
typedef enum {
    RTK_STATE_IDLE = 0,                 /* 空闲/未初始化 */
    RTK_STATE_INIT,                     /* 已初始化 */
    RTK_STATE_CONNECTING,               /* 连接中 */
    RTK_STATE_RUNNING,                  /* 运行中 */
    RTK_STATE_STOPPING,                 /* 停止中 */
    RTK_STATE_ERROR                     /* 错误状态 */
} rtk_state_t;

/* ============================================================================
 * 日志级别
 * ========================================================================== */
typedef enum {
    RTK_LOG_OFF = 0,                    /* 关闭日志 */
    RTK_LOG_ERROR = 1,                  /* 错误 */
    RTK_LOG_WARN = 2,                   /* 警告 */
    RTK_LOG_INFO = 3,                   /* 信息 */
    RTK_LOG_DEBUG = 4                   /* 调试 */
} rtk_log_level_t;

/* ============================================================================
 * 数据结构定义
 * ========================================================================== */

/**
 * @brief RTK SDK配置结构体
 */
typedef struct {
    /* 鉴权信息 */
    char ak[RTK_AK_MAX_LEN];            /* 鉴权AK */
    char as[RTK_AS_MAX_LEN];            /* 鉴权AS */
    char device_id[RTK_DEVICE_ID_MAX_LEN];      /* 设备ID */
    char device_type[RTK_DEVICE_TYPE_MAX_LEN];  /* 设备类型 */
    
    /* 网络配置 */
    int timeout_sec;                    /* 超时时间（秒），默认10 */
    int use_https;                      /* 是否使用HTTPS，0=HTTP, 1=HTTPS */
    
    /* 广播配置（进程模式） */
    int broadcast_port;                 /* UDP广播端口，默认9000 */
    char broadcast_addr[RTK_IP_ADDR_MAX_LEN];   /* 广播地址，默认255.255.255.255 */
    
    /* GPS串口配置（进程模式） */
    char gps_serial_port[64];           /* GPS串口设备，如 /dev/ttyUSB0 */
    int gps_serial_baudrate;            /* GPS串口波特率，默认115200 */
    int gps_auto_mode;                  /* 自动模式: 1=自动读取GPS并发送差分 */
    
    /* 日志配置 */
    rtk_log_level_t log_level;          /* 日志级别 */
    char log_file[256];                 /* 日志文件路径，空则只输出到控制台 */
} rtk_config_t;

/**
 * @brief 错误信息结构体
 */
typedef struct {
    int code;                           /* 错误码 */
    const char *msg;                    /* 错误描述 */
    int recoverable;                    /* 是否可恢复 1=可恢复 0=不可恢复 */
} rtk_error_info_t;

/* ============================================================================
 * 回调函数类型定义（线程库模式使用）
 * ========================================================================== */

/**
 * @brief RTCM差分数据回调
 * @param data RTCM二进制数据
 * @param len 数据长度
 * @param user_data 用户自定义数据
 * @note 回调在工作线程中执行，请勿阻塞
 */
typedef void (*rtk_rtcm_callback_t)(const uint8_t *data, int len, void *user_data);

/**
 * @brief 状态变化回调
 * @param state 当前状态
 * @param status_code 六分SDK状态码
 * @param user_data 用户自定义数据
 */
typedef void (*rtk_status_callback_t)(rtk_state_t state, int status_code, void *user_data);

/**
 * @brief 错误回调
 * @param err 错误信息
 * @param user_data 用户自定义数据
 */
typedef void (*rtk_error_callback_t)(const rtk_error_info_t *err, void *user_data);

/**
 * @brief 日志回调
 * @param level 日志级别
 * @param msg 日志内容
 * @param user_data 用户自定义数据
 */
typedef void (*rtk_log_callback_t)(rtk_log_level_t level, const char *msg, void *user_data);

/* ============================================================================
 * 核心API
 * ========================================================================== */

/**
 * @brief 初始化RTK SDK
 * @param config 配置参数，不能为空
 * @return RTK_OK成功，负值失败
 * @note 必须在其他API之前调用
 */
int rtk_sdk_init(const rtk_config_t *config);

/**
 * @brief 注销RTK SDK
 * @note 释放所有资源，程序退出前必须调用
 */
void rtk_sdk_deinit(void);

/**
 * @brief 开始RTK定位
 * @return RTK_OK成功，负值失败
 * @note 此函数会启动工作线程，连接服务器
 */
int rtk_sdk_start(void);

/**
 * @brief 停止RTK定位
 * @note 会等待工作线程退出
 */
void rtk_sdk_stop(void);

/**
 * @brief 获取当前运行状态
 * @return 当前状态枚举值
 */
rtk_state_t rtk_sdk_get_state(void);

/**
 * @brief 获取SDK版本号
 * @return 版本字符串
 */
const char* rtk_sdk_get_version(void);

/**
 * @brief 错误码转字符串
 * @param err 错误码
 * @return 错误描述字符串
 */
const char* rtk_strerror(int err);

/* ============================================================================
 * GGA数据输入API
 * ========================================================================== */

/**
 * @brief 输入GGA数据（NMEA格式字符串）
 * @param gga_str GGA字符串，需符合NMEA-0183格式
 * @param len 字符串长度
 * @return RTK_OK成功，负值失败
 * @note 建议每1-2秒输入一次
 */
int rtk_sdk_input_gga(const char *gga_str, int len);

/**
 * @brief 输入经纬度高度（SDK内部构造GGA）
 * @param lat 纬度（度）
 * @param lon 经度（度）
 * @param alt 高度（米）
 * @return RTK_OK成功，负值失败
 */
int rtk_sdk_input_position(double lat, double lon, double alt);

/* ============================================================================
 * 回调注册API（线程库模式）
 * ========================================================================== */

/**
 * @brief 注册RTCM差分数据回调
 * @param cb 回调函数，NULL则取消注册
 * @param user_data 用户自定义数据
 */
void rtk_sdk_set_rtcm_callback(rtk_rtcm_callback_t cb, void *user_data);

/**
 * @brief 注册状态变化回调
 * @param cb 回调函数，NULL则取消注册
 * @param user_data 用户自定义数据
 */
void rtk_sdk_set_status_callback(rtk_status_callback_t cb, void *user_data);

/**
 * @brief 注册错误回调
 * @param cb 回调函数，NULL则取消注册
 * @param user_data 用户自定义数据
 */
void rtk_sdk_set_error_callback(rtk_error_callback_t cb, void *user_data);

/**
 * @brief 注册日志回调
 * @param cb 回调函数，NULL则取消注册
 * @param user_data 用户自定义数据
 */
void rtk_sdk_set_log_callback(rtk_log_callback_t cb, void *user_data);

/* ============================================================================
 * UDP广播控制API（进程模式）
 * ========================================================================== */

/**
 * @brief 启用UDP广播
 * @param port 广播端口
 * @param broadcast_addr 广播地址，NULL使用默认255.255.255.255
 * @return RTK_OK成功，负值失败
 */
int rtk_sdk_enable_broadcast(int port, const char *broadcast_addr);

/**
 * @brief 禁用UDP广播
 */
void rtk_sdk_disable_broadcast(void);

/**
 * @brief 检查广播是否启用
 * @return 1启用 0未启用
 */
int rtk_sdk_is_broadcast_enabled(void);

/* ============================================================================
 * 日志控制API
 * ========================================================================== */

/**
 * @brief 设置日志级别
 * @param level 日志级别
 */
void rtk_sdk_set_log_level(rtk_log_level_t level);

/**
 * @brief 获取当前日志级别
 * @return 日志级别
 */
rtk_log_level_t rtk_sdk_get_log_level(void);

/**
 * @brief 设置全局日志开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_enabled(int enabled);

/**
 * @brief 获取全局日志开关状态
 * @return 1启用 0禁用
 */
int rtk_log_is_enabled(void);

/**
 * @brief 设置控制台输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_console_enabled(int enabled);

/**
 * @brief 设置文件输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_file_enabled(int enabled);

/**
 * @brief 设置回调输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_callback_enabled(int enabled);

/* ============================================================================
 * GPS串口API（进程模式）
 * ========================================================================== */

/**
 * @brief RTK定位结果结构体
 */
typedef struct {
    double latitude;            /* 纬度（度） */
    double longitude;           /* 经度（度） */
    double altitude;            /* 海拔高度（米） */
    double hdop;                /* 水平精度因子 */
    int fix_quality;            /* 定位质量 0:无效 1:单点 2:差分 4:RTK固定 5:RTK浮动 */
    int satellites;             /* 可见卫星数 */
    uint64_t timestamp_ms;      /* 时间戳（毫秒） */
} rtk_position_result_t;

/**
 * @brief 定位结果回调
 * @param pos 定位结果
 * @param user_data 用户自定义数据
 */
typedef void (*rtk_position_callback_t)(const rtk_position_result_t *pos, void *user_data);

/**
 * @brief 打开GPS串口（自动模式）
 * @param port 串口设备路径，如 /dev/ttyUSB0
 * @param baudrate 波特率，0使用默认115200
 * @return RTK_OK成功，负值失败
 */
int rtk_sdk_open_gps_serial(const char *port, int baudrate);

/**
 * @brief 关闭GPS串口
 */
void rtk_sdk_close_gps_serial(void);

/**
 * @brief 检查GPS串口是否打开
 * @return 1打开 0关闭
 */
int rtk_sdk_is_gps_serial_open(void);

/**
 * @brief 获取最新RTK定位结果
 * @param pos 输出定位结果
 * @return RTK_OK成功，RTK_ERR_NOT_RUNNING无数据
 */
int rtk_sdk_get_position(rtk_position_result_t *pos);

/**
 * @brief 注册定位结果回调
 * @param cb 回调函数，NULL则取消注册
 * @param user_data 用户自定义数据
 */
void rtk_sdk_set_position_callback(rtk_position_callback_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* RTK_SDK_H */
