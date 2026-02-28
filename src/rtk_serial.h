/**
 * @file rtk_serial.h
 * @brief RTK SDK GPS串口模块头文件
 */

#ifndef RTK_SERIAL_H
#define RTK_SERIAL_H

#include <stdint.h>

/* ============================================================================
 * 串口配置
 * ========================================================================== */
#define RTK_SERIAL_BAUD_DEFAULT     115200
#define RTK_SERIAL_RECV_BUF_SIZE    4096
#define RTK_SERIAL_SEND_BUF_SIZE    4096

/* 重连配置 */
#define RTK_SERIAL_RECONNECT_DELAY_MS   2000    /* 重连延迟（毫秒） */
#define RTK_SERIAL_MAX_RECONNECT_COUNT  20      /* 最大重连次数 */
#define RTK_SERIAL_ERROR_THRESHOLD      3       /* 连续错误阈值触发重连 */

/**
 * @brief 串口配置结构体
 */
typedef struct {
    char port[64];              /* 串口设备路径，如 /dev/ttyUSB0 */
    int baudrate;               /* 波特率，默认115200 */
    int databits;               /* 数据位，默认8 */
    int stopbits;               /* 停止位，默认1 */
    char parity;                /* 校验位 'N':无 'O':奇 'E':偶 */
    int auto_reconnect;         /* 是否自动重连 1=是 0=否 */
} rtk_serial_config_t;

/**
 * @brief GPS定位结果结构体
 */
typedef struct {
    double latitude;            /* 纬度（度） */
    double longitude;           /* 经度（度） */
    double altitude;            /* 海拔高度（米） */
    double hdop;                /* 水平精度因子 */
    int fix_quality;            /* 定位质量 0:无效 1:单点 2:差分 4:RTK固定 5:RTK浮动 */
    int satellites;             /* 可见卫星数 */
    uint64_t timestamp_ms;      /* 时间戳（毫秒） */
    char raw_gga[256];          /* 原始GGA语句 */
    char raw_rmc[256];          /* 原始RMC语句 */
} rtk_position_t;

/**
 * @brief 串口状态枚举
 */
typedef enum {
    RTK_SERIAL_STATE_CLOSED = 0,    /* 关闭 */
    RTK_SERIAL_STATE_OPEN,          /* 打开 */
    RTK_SERIAL_STATE_ERROR,         /* 错误 */
    RTK_SERIAL_STATE_RECONNECTING   /* 重连中 */
} rtk_serial_state_t;

/* ============================================================================
 * 串口操作函数
 * ========================================================================== */

/**
 * @brief 打开GPS串口
 * @param config 串口配置
 * @return RTK_OK成功，负值失败
 */
int rtk_serial_open(const rtk_serial_config_t *config);

/**
 * @brief 关闭GPS串口
 */
void rtk_serial_close(void);

/**
 * @brief 检查串口是否打开
 * @return 1打开 0关闭
 */
int rtk_serial_is_open(void);

/**
 * @brief 获取串口状态
 * @return 串口状态枚举值
 */
rtk_serial_state_t rtk_serial_get_state(void);

/**
 * @brief 尝试重连串口
 * @return RTK_OK成功，负值失败
 */
int rtk_serial_reconnect(void);

/**
 * @brief 从串口读取数据（线程安全）
 * @param buf 接收缓冲区
 * @param len 缓冲区大小
 * @param timeout_ms 超时时间（毫秒）
 * @return 读取的字节数，0超时，负值错误（需要重连）
 */
int rtk_serial_read(uint8_t *buf, int len, int timeout_ms);

/**
 * @brief 向串口写入数据（线程安全）
 * @param data 发送数据
 * @param len 数据长度
 * @return 写入的字节数，负值错误
 */
int rtk_serial_write(const uint8_t *data, int len);

/**
 * @brief 向GPS发送RTCM差分数据
 * @param rtcm RTCM二进制数据
 * @param len 数据长度
 * @return RTK_OK成功，负值失败
 */
int rtk_serial_send_rtcm(const uint8_t *rtcm, int len);

/**
 * @brief 解析NMEA数据获取定位结果
 * @param nmea NMEA数据
 * @param len 数据长度
 * @param pos 输出定位结果
 * @return RTK_OK成功解析到GGA，否则返回负值
 */
int rtk_serial_parse_nmea(const char *nmea, int len, rtk_position_t *pos);

/**
 * @brief 获取最新定位结果
 * @param pos 输出定位结果
 * @return RTK_OK成功，RTK_ERR_NOT_RUNNING无数据
 */
int rtk_serial_get_position(rtk_position_t *pos);

#endif /* RTK_SERIAL_H */
