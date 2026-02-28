/**
 * @file rtk_serial.c
 * @brief RTK SDK GPS串口模块实现
 * 
 * @details 实现GPS串口读写、NMEA解析、RTCM发送
 *          优化：线程安全fd使用、串口断开自动重连
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <pthread.h>

#include "rtk_internal.h"
#include "rtk_serial.h"

/* ============================================================================
 * 串口上下文
 * ========================================================================== */
typedef struct {
    int fd;                                 /* 串口文件描述符 */
    rtk_serial_state_t state;               /* 串口状态 */
    rtk_serial_config_t config;             /* 配置（保存以支持重连） */
    pthread_mutex_t mutex;                  /* 互斥锁 */
    
    /* 错误计数（用于触发重连） */
    int error_count;
    int reconnect_count;
    
    /* 最新定位数据 */
    rtk_position_t last_position;           /* 最新定位结果 */
    int position_valid;                     /* 定位是否有效 */
    
    /* 接收缓冲区 */
    char recv_buf[RTK_SERIAL_RECV_BUF_SIZE];
    int recv_len;
} rtk_serial_ctx_t;

static rtk_serial_ctx_t g_serial_ctx = {
    .fd = -1,
    .state = RTK_SERIAL_STATE_CLOSED,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

/* ============================================================================
 * 波特率转换
 * ========================================================================== */
static speed_t baudrate_to_speed(int baudrate) {
    switch (baudrate) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

/* ============================================================================
 * 内部函数：打开串口（不加锁）
 * ========================================================================== */
static int serial_open_internal(const rtk_serial_config_t *config) {
    /* 打开串口 */
    int fd = open(config->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        RTK_LOGE("打开串口失败: %s - %s", config->port, strerror(errno));
        return RTK_ERR_FILE;
    }
    
    /* 配置串口参数 */
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    
    if (tcgetattr(fd, &tty) != 0) {
        RTK_LOGE("获取串口属性失败: %s", strerror(errno));
        close(fd);
        return RTK_ERR_FILE;
    }
    
    /* 设置波特率 */
    speed_t speed = baudrate_to_speed(config->baudrate > 0 ? config->baudrate : RTK_SERIAL_BAUD_DEFAULT);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    
    /* 控制模式 */
    tty.c_cflag |= (CLOCAL | CREAD);    /* 启用接收，忽略调制解调器信号 */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;                 /* 8位数据位 */
    tty.c_cflag &= ~PARENB;             /* 无校验 */
    tty.c_cflag &= ~CSTOPB;             /* 1位停止位 */
    tty.c_cflag &= ~CRTSCTS;            /* 无硬件流控 */
    
    /* 输入模式 */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  /* 禁用软件流控 */
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL);
    
    /* 输出模式 */
    tty.c_oflag &= ~OPOST;              /* 原始输出 */
    
    /* 本地模式 */
    tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    
    /* 读取超时设置 */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;                /* 100ms超时 */
    
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        RTK_LOGE("设置串口属性失败: %s", strerror(errno));
        close(fd);
        return RTK_ERR_FILE;
    }
    
    /* 清空缓冲区 */
    tcflush(fd, TCIOFLUSH);
    
    return fd;  /* 返回fd而不是RTK_OK */
}

/* ============================================================================
 * 串口打开/关闭
 * ========================================================================== */

int rtk_serial_open(const rtk_serial_config_t *config) {
    if (config == NULL || strlen(config->port) == 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    if (g_serial_ctx.state == RTK_SERIAL_STATE_OPEN) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_OK;
    }
    
    /* 保存配置（用于重连） */
    memcpy(&g_serial_ctx.config, config, sizeof(rtk_serial_config_t));
    
    /* 打开串口 */
    int fd = serial_open_internal(config);
    if (fd < 0) {
        g_serial_ctx.state = RTK_SERIAL_STATE_ERROR;
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return fd;
    }
    
    g_serial_ctx.fd = fd;
    g_serial_ctx.state = RTK_SERIAL_STATE_OPEN;
    g_serial_ctx.recv_len = 0;
    g_serial_ctx.position_valid = 0;
    g_serial_ctx.error_count = 0;
    g_serial_ctx.reconnect_count = 0;
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    RTK_LOGI("GPS串口已打开: %s, 波特率: %d", config->port, 
             config->baudrate > 0 ? config->baudrate : RTK_SERIAL_BAUD_DEFAULT);
    
    return RTK_OK;
}

void rtk_serial_close(void) {
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    if (g_serial_ctx.state == RTK_SERIAL_STATE_CLOSED) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return;
    }
    
    if (g_serial_ctx.fd >= 0) {
        close(g_serial_ctx.fd);
        g_serial_ctx.fd = -1;
    }
    
    g_serial_ctx.state = RTK_SERIAL_STATE_CLOSED;
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    RTK_LOGI("GPS串口已关闭");
}

int rtk_serial_is_open(void) {
    pthread_mutex_lock(&g_serial_ctx.mutex);
    int is_open = (g_serial_ctx.state == RTK_SERIAL_STATE_OPEN);
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    return is_open;
}

rtk_serial_state_t rtk_serial_get_state(void) {
    pthread_mutex_lock(&g_serial_ctx.mutex);
    rtk_serial_state_t state = g_serial_ctx.state;
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    return state;
}

/* ============================================================================
 * 串口重连
 * ========================================================================== */

int rtk_serial_reconnect(void) {
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    /* 检查是否可以重连 */
    if (g_serial_ctx.reconnect_count >= RTK_SERIAL_MAX_RECONNECT_COUNT) {
        RTK_LOGE("串口重连次数已达上限 (%d)", RTK_SERIAL_MAX_RECONNECT_COUNT);
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_FILE;
    }
    
    g_serial_ctx.state = RTK_SERIAL_STATE_RECONNECTING;
    g_serial_ctx.reconnect_count++;
    
    RTK_LOGI("尝试重连串口 (%d/%d): %s", 
             g_serial_ctx.reconnect_count, 
             RTK_SERIAL_MAX_RECONNECT_COUNT,
             g_serial_ctx.config.port);
    
    /* 关闭旧fd */
    if (g_serial_ctx.fd >= 0) {
        close(g_serial_ctx.fd);
        g_serial_ctx.fd = -1;
    }
    
    /* 解锁，等待延迟 */
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    rtk_sleep_ms(RTK_SERIAL_RECONNECT_DELAY_MS);
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    /* 重新打开 */
    int fd = serial_open_internal(&g_serial_ctx.config);
    if (fd < 0) {
        g_serial_ctx.state = RTK_SERIAL_STATE_ERROR;
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        RTK_LOGE("串口重连失败");
        return fd;
    }
    
    g_serial_ctx.fd = fd;
    g_serial_ctx.state = RTK_SERIAL_STATE_OPEN;
    g_serial_ctx.error_count = 0;
    g_serial_ctx.recv_len = 0;
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    RTK_LOGI("串口重连成功");
    return RTK_OK;
}

/* ============================================================================
 * 串口读写（线程安全：复制fd后操作）
 * ========================================================================== */

int rtk_serial_read(uint8_t *buf, int len, int timeout_ms) {
    if (buf == NULL || len <= 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    /* 获取fd的安全副本 */
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    if (g_serial_ctx.state != RTK_SERIAL_STATE_OPEN || g_serial_ctx.fd < 0) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_NOT_RUNNING;
    }
    
    /* 使用dup复制fd，即使原fd被close也不影响本次操作 */
    int fd = dup(g_serial_ctx.fd);
    if (fd < 0) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_FILE;
    }
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    /* 使用select实现超时 */
    fd_set rfds;
    struct timeval tv;
    
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0) {
        close(fd);
        /* 记录错误，可能需要重连 */
        pthread_mutex_lock(&g_serial_ctx.mutex);
        g_serial_ctx.error_count++;
        if (g_serial_ctx.error_count >= RTK_SERIAL_ERROR_THRESHOLD) {
            g_serial_ctx.state = RTK_SERIAL_STATE_ERROR;
        }
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        RTK_LOGE("select失败: %s", strerror(errno));
        return RTK_ERR_FILE;
    }
    if (ret == 0) {
        close(fd);
        return 0;  /* 超时 */
    }
    
    ssize_t n = read(fd, buf, len);
    close(fd);  /* 关闭复制的fd */
    
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        /* 记录错误 */
        pthread_mutex_lock(&g_serial_ctx.mutex);
        g_serial_ctx.error_count++;
        if (g_serial_ctx.error_count >= RTK_SERIAL_ERROR_THRESHOLD) {
            g_serial_ctx.state = RTK_SERIAL_STATE_ERROR;
        }
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        RTK_LOGE("串口读取失败: %s", strerror(errno));
        return RTK_ERR_FILE;
    }
    
    /* 读取成功，清零错误计数 */
    if (n > 0) {
        pthread_mutex_lock(&g_serial_ctx.mutex);
        g_serial_ctx.error_count = 0;
        pthread_mutex_unlock(&g_serial_ctx.mutex);
    }
    
    return (int)n;
}

int rtk_serial_write(const uint8_t *data, int len) {
    if (data == NULL || len <= 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    /* 获取fd的安全副本 */
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    if (g_serial_ctx.state != RTK_SERIAL_STATE_OPEN || g_serial_ctx.fd < 0) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_NOT_RUNNING;
    }
    
    int fd = dup(g_serial_ctx.fd);
    if (fd < 0) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_FILE;
    }
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    ssize_t written = write(fd, data, len);
    if (written < 0) {
        close(fd);
        pthread_mutex_lock(&g_serial_ctx.mutex);
        g_serial_ctx.error_count++;
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        RTK_LOGE("串口写入失败: %s", strerror(errno));
        return RTK_ERR_FILE;
    }
    
    /* 确保数据发送完成 */
    tcdrain(fd);
    close(fd);
    
    return (int)written;
}

int rtk_serial_send_rtcm(const uint8_t *rtcm, int len) {
    if (rtcm == NULL || len <= 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    int ret = rtk_serial_write(rtcm, len);
    if (ret > 0) {
        RTK_LOGD("发送RTCM到GPS: %d 字节", ret);
    }
    
    return ret > 0 ? RTK_OK : ret;
}

/* ============================================================================
 * NMEA解析
 * ========================================================================== */

/**
 * @brief 在NMEA数据中查找GGA语句（支持所有Talker ID：GP/GN/GB/GL/GA）
 * @return 找到的GGA语句起始位置，未找到返回NULL
 */
static const char *find_nmea_sentence(const char *nmea, const char *type) {
    /* 支持的Talker ID前缀：GP(GPS), GN(多系统), GB(北斗), GL(GLONASS), GA(Galileo) */
    static const char *talker_ids[] = { "$GP", "$GN", "$GB", "$GL", "$GA", NULL };
    
    for (int i = 0; talker_ids[i] != NULL; i++) {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "%s%s", talker_ids[i], type);
        const char *found = strstr(nmea, prefix);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/**
 * @brief 解析GGA语句（兼容所有Talker ID）
 */
static int parse_gga(const char *gga, rtk_position_t *pos) {
    /* $xxGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh */
    
    char fields[15][32] = {0};
    int field_count = 0;
    
    /* 动态跳过前缀：查找第一个逗号，兼容不同长度的Talker ID */
    const char *p = strchr(gga, ',');
    if (p == NULL) {
        return RTK_ERR_INVALID_GGA;
    }
    p++;  /* 跳过逗号 */
    
    /* 分割字段 */
    while (*p && field_count < 15) {
        const char *start = p;
        while (*p && *p != ',' && *p != '*') p++;
        int flen = p - start;
        if (flen > 0 && flen < 32) {
            memcpy(fields[field_count], start, flen);
        }
        field_count++;
        if (*p == ',') p++;
        else if (*p == '*') break;
    }
    
    if (field_count < 10) {
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 解析定位质量（fix=0 表示无卫星信号，仍解析其余字段以支持状态广播） */
    pos->fix_quality = atoi(fields[5]);
    
    /* 解析纬度 ddmm.mmmm */
    if (strlen(fields[1]) >= 4) {
        double lat_deg = (fields[1][0] - '0') * 10 + (fields[1][1] - '0');
        double lat_min = atof(fields[1] + 2);
        pos->latitude = lat_deg + lat_min / 60.0;
        if (fields[2][0] == 'S') pos->latitude = -pos->latitude;
    }
    
    /* 解析经度 dddmm.mmmm */
    if (strlen(fields[3]) >= 5) {
        double lon_deg = (fields[3][0] - '0') * 100 + (fields[3][1] - '0') * 10 + (fields[3][2] - '0');
        double lon_min = atof(fields[3] + 3);
        pos->longitude = lon_deg + lon_min / 60.0;
        if (fields[4][0] == 'W') pos->longitude = -pos->longitude;
    }
    
    /* 卫星数 */
    pos->satellites = atoi(fields[6]);
    
    /* HDOP */
    pos->hdop = atof(fields[7]);
    
    /* 海拔高度 */
    pos->altitude = atof(fields[8]);
    
    /* 保存原始GGA */
    strncpy(pos->raw_gga, gga, sizeof(pos->raw_gga) - 1);
    
    pos->timestamp_ms = rtk_get_time_ms();
    
    return RTK_OK;
}

int rtk_serial_parse_nmea(const char *nmea, int len, rtk_position_t *pos) {
    if (nmea == NULL || len <= 0 || pos == NULL) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    /* 查找GGA语句（支持 GP/GN/GB/GL/GA 等所有Talker ID） */
    const char *gga = find_nmea_sentence(nmea, "GGA");
    
    if (gga) {
        /* 验证GGA有效性 */
        if (rtk_gga_validate(gga, strlen(gga)) == RTK_OK) {
            if (parse_gga(gga, pos) == RTK_OK) {
                /* 仅在有效定位(fix>0)时更新位置缓存，fix=0时只广播状态不缓存位置 */
                if (pos->fix_quality > 0) {
                    pthread_mutex_lock(&g_serial_ctx.mutex);
                    memcpy(&g_serial_ctx.last_position, pos, sizeof(rtk_position_t));
                    g_serial_ctx.position_valid = 1;
                    pthread_mutex_unlock(&g_serial_ctx.mutex);
                }
                return RTK_OK;
            }
        }
    }
    
    /* 查找RMC语句（支持所有Talker ID） */
    const char *rmc = find_nmea_sentence(nmea, "RMC");
    if (rmc) {
        strncpy(pos->raw_rmc, rmc, sizeof(pos->raw_rmc) - 1);
    }
    
    return RTK_ERR_INVALID_GGA;
}

int rtk_serial_get_position(rtk_position_t *pos) {
    if (pos == NULL) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_serial_ctx.mutex);
    
    if (!g_serial_ctx.position_valid) {
        pthread_mutex_unlock(&g_serial_ctx.mutex);
        return RTK_ERR_NOT_RUNNING;
    }
    
    memcpy(pos, &g_serial_ctx.last_position, sizeof(rtk_position_t));
    
    pthread_mutex_unlock(&g_serial_ctx.mutex);
    
    return RTK_OK;
}
