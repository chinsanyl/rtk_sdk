/**
 * @file rtk_core.c
 * @brief RTK SDK核心引擎实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

#include "rtk_internal.h"
#include "sixents_sdk.h"
#include "sixents_types.h"

/* ============================================================================
 * 全局上下文实例
 * ========================================================================== */
rtk_context_t g_rtk_ctx = {0};

/* ============================================================================
 * 错误码描述表
 * ========================================================================== */
static const char* g_error_strings[] = {
    [0] = "成功",
};

static const struct {
    int code;
    const char *msg;
} g_error_map[] = {
    { RTK_OK,                   "成功" },
    { RTK_ERR_INVALID_PARAM,    "无效参数" },
    { RTK_ERR_NULL_PTR,         "空指针" },
    { RTK_ERR_BUFFER_OVERFLOW,  "缓冲区溢出" },
    { RTK_ERR_INVALID_GGA,      "无效GGA数据" },
    { RTK_ERR_NOT_INIT,         "未初始化" },
    { RTK_ERR_ALREADY_INIT,     "重复初始化" },
    { RTK_ERR_NOT_RUNNING,      "未运行" },
    { RTK_ERR_ALREADY_RUNNING,  "已运行" },
    { RTK_ERR_AUTH_FAILED,      "鉴权失败" },
    { RTK_ERR_CONNECT_FAILED,   "连接失败" },
    { RTK_ERR_TIMEOUT,          "超时" },
    { RTK_ERR_SOCKET,           "Socket错误" },
    { RTK_ERR_BROADCAST,        "广播错误" },
    { RTK_ERR_THREAD,           "线程创建失败" },
    { RTK_ERR_MUTEX,            "互斥锁错误" },
    { RTK_ERR_MEMORY,           "内存分配失败" },
    { RTK_ERR_FILE,             "文件操作失败" },
    { RTK_ERR_SIXENTS_INIT,     "六分SDK初始化失败" },
    { RTK_ERR_SIXENTS_START,    "六分SDK启动失败" },
    { RTK_ERR_SIXENTS_SEND,     "六分SDK发送失败" },
    { 0, NULL }
};

/* ============================================================================
 * 工具函数实现
 * ========================================================================== */

int rtk_safe_strcpy(char *dst, size_t dst_size, const char *src) {
    if (!dst || !src || dst_size == 0) {
        return -1;
    }
    size_t src_len = strlen(src);
    if (src_len >= dst_size) {
        return -1;
    }
    memcpy(dst, src, src_len + 1);
    return 0;
}

uint64_t rtk_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void rtk_sleep_ms(int ms) {
    usleep(ms * 1000);
}

const char* rtk_strerror(int err) {
    for (int i = 0; g_error_map[i].msg != NULL; i++) {
        if (g_error_map[i].code == err) {
            return g_error_map[i].msg;
        }
    }
    return "未知错误";
}

/* ============================================================================
 * 六分SDK回调函数
 * ========================================================================== */

void rtk_sixents_diff_callback(const char *buff, unsigned int len) {
    if (buff == NULL || len == 0) {
        return;
    }
    
    /* 统计 */
    g_rtk_ctx.rtcm_recv_count++;
    g_rtk_ctx.rtcm_recv_bytes += len;
    
    RTK_LOGD("收到RTCM数据: %u 字节", len);
    
    /* 分发给回调（线程库模式） */
    rtk_dispatch_rtcm((const uint8_t *)buff, (int)len);
    
    /* 广播（进程模式） */
    if (g_rtk_ctx.broadcast_enabled) {
        rtk_broadcast_send((const uint8_t *)buff, (int)len);
    }
}

void rtk_sixents_status_callback(unsigned int status) {
    RTK_LOGI("六分SDK状态: %u", status);
    
    /* 分发状态变化 */
    rtk_dispatch_status(g_rtk_ctx.state, (int)status);
    
    /* 处理关键状态码 */
    switch (status) {
        case 1201: /* SIXENTS_STATE_AUTHENTICATE_OK */
            RTK_LOGI("鉴权成功");
            break;
        case 1206: /* SIXENTS_STATE_AUTHENTICATE_FAIL */
            RTK_LOGE("鉴权失败");
            rtk_dispatch_error(RTK_ERR_AUTH_FAILED, "鉴权失败", 0);
            break;
        case 1401: /* SIXENTS_STATE_RTCM_GET_SUCCESS */
            RTK_LOGD("RTCM获取成功");
            break;
        case 1404: /* SIXENTS_STATE_RTCM_GGA_GET_TIMEOUT */
            RTK_LOGW("60秒未收到GGA数据");
            rtk_dispatch_error(RTK_ERR_TIMEOUT, "60秒未收到GGA数据", 1);
            break;
        case 1406: /* SIXENTS_STATE_RTCM_GET_RTCM_TIMEOUT */
            RTK_LOGW("60秒未获取到RTCM数据");
            rtk_dispatch_error(RTK_ERR_TIMEOUT, "60秒未获取到RTCM数据", 1);
            break;
    }
}

int rtk_sixents_log_callback(const char *buff, unsigned short len) {
    if (buff && len > 0) {
        RTK_LOGD("[SIXENTS] %s", buff);
    }
    return (int)len;
}

/* ============================================================================
 * 工作线程
 * ========================================================================== */

void* rtk_worker_thread(void *arg) {
    (void)arg;
    
    sixents_sdkConf param;
    int ret = 0;
    int retry_count = 0;
    uint64_t last_gga_time = 0;
    
    RTK_LOGI("工作线程启动");
    
    /* 初始化六分SDK配置 */
    memset(&param, 0, sizeof(param));
    param.keyType = SIXENTS_KEY_TYPE_AK;
    memcpy(param.key, g_rtk_ctx.config.ak, strlen(g_rtk_ctx.config.ak));
    memcpy(param.secret, g_rtk_ctx.config.as, strlen(g_rtk_ctx.config.as));
    memcpy(param.devID, g_rtk_ctx.config.device_id, strlen(g_rtk_ctx.config.device_id));
    memcpy(param.devType, g_rtk_ctx.config.device_type, strlen(g_rtk_ctx.config.device_type));
    
    param.timeout = (unsigned int)g_rtk_ctx.config.timeout_sec;
    param.sockIOBlockFlag = SIXENTS_SOCK_IOFLAG_NOBLOCK;
    param.logPrintLevel = SIXENTS_LL_DEBUG;
    
    /* 回调函数 */
    param.cbGetDiffData = (sixents_cbGetDiffData)rtk_sixents_diff_callback;
    param.cbGetStatus = (sixents_cbGetStatus)rtk_sixents_status_callback;
    param.cbTrace = (sixents_cbTrace)rtk_sixents_log_callback;
    
    /* 初始化六分SDK */
    while (g_rtk_ctx.worker_running && !g_rtk_ctx.sixents_init) {
        ret = sixents_sdkInit(&param);
        if (ret == SIXENTS_RET_OK) {
            g_rtk_ctx.sixents_init = 1;
            RTK_LOGI("六分SDK初始化成功");
            break;
        } else {
            RTK_LOGE("六分SDK初始化失败: %d, 重试 %d/%d", ret, retry_count + 1, RTK_MAX_RETRY_COUNT);
            sixents_sdkFinal();
            retry_count++;
            if (retry_count >= RTK_MAX_RETRY_COUNT) {
                rtk_dispatch_error(RTK_ERR_SIXENTS_INIT, "六分SDK初始化失败", 0);
                goto exit;
            }
            rtk_sleep_ms(RTK_RECONNECT_DELAY_MS);
        }
    }
    
    /* 启动六分SDK */
    retry_count = 0;
    while (g_rtk_ctx.worker_running && !g_rtk_ctx.sixents_started) {
        ret = sixents_sdkStart();
        if (ret == SIXENTS_RET_OK) {
            g_rtk_ctx.sixents_started = 1;
            RTK_LOGI("六分SDK启动成功");
            
            /* 更新状态 */
            pthread_mutex_lock(&g_rtk_ctx.state_mutex);
            g_rtk_ctx.state = RTK_STATE_RUNNING;
            pthread_mutex_unlock(&g_rtk_ctx.state_mutex);

            /* 连接成功，清除之前的错误信息 */
            pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
            g_rtk_ctx.last_error_code = 0;
            g_rtk_ctx.last_error_msg[0] = '\0';
            pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);

            rtk_dispatch_status(RTK_STATE_RUNNING, 0);
            break;
        } else {
            RTK_LOGE("六分SDK启动失败: %d, 重试 %d/%d", ret, retry_count + 1, RTK_MAX_RETRY_COUNT);
            retry_count++;
            if (retry_count >= RTK_MAX_RETRY_COUNT) {
                rtk_dispatch_error(RTK_ERR_SIXENTS_START, "六分SDK启动失败", 0);
                goto exit;
            }
            rtk_sleep_ms(RTK_RECONNECT_DELAY_MS);
        }
    }
    
    /* 主循环 */
    while (g_rtk_ctx.worker_running) {
        rtk_sleep_ms(RTK_TICK_INTERVAL_MS);
        
        /* 发送GGA数据 */
        uint64_t now = rtk_get_time_ms();
        if (now - last_gga_time >= RTK_GGA_INTERVAL_MS) {
            pthread_mutex_lock(&g_rtk_ctx.gga_mutex);
            if (g_rtk_ctx.gga_ready && g_rtk_ctx.gga_len > 0) {
                ret = sixents_sdkSendGGAStr(g_rtk_ctx.gga_buffer, (unsigned short)g_rtk_ctx.gga_len);
                if (ret == SIXENTS_RET_OK) {
                    g_rtk_ctx.gga_send_count++;
                    RTK_LOGD("发送GGA成功");
                } else {
                    RTK_LOGW("发送GGA失败: %d", ret);
                }
            }
            pthread_mutex_unlock(&g_rtk_ctx.gga_mutex);
            last_gga_time = now;
        }
        
        /* 驱动SDK执行 */
        ret = sixents_sdkTick();
        if (ret != SIXENTS_RET_OK) {
            RTK_LOGW("sixents_sdkTick失败: %d", ret);
        }
    }
    
exit:
    /* 停止六分SDK */
    if (g_rtk_ctx.sixents_started) {
        sixents_sdkStop();
        g_rtk_ctx.sixents_started = 0;
        RTK_LOGI("六分SDK已停止");
    }
    
    if (g_rtk_ctx.sixents_init) {
        sixents_sdkFinal();
        g_rtk_ctx.sixents_init = 0;
        RTK_LOGI("六分SDK已注销");
    }
    
    RTK_LOGI("工作线程退出");
    return NULL;
}

/* ============================================================================
 * 核心API实现
 * ========================================================================== */

int rtk_sdk_init(const rtk_config_t *config) {
    if (config == NULL) {
        return RTK_ERR_NULL_PTR;
    }
    
    /* 验证必填参数 */
    if (strlen(config->ak) == 0 || strlen(config->as) == 0 ||
        strlen(config->device_id) == 0 || strlen(config->device_type) == 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    
    if (g_rtk_ctx.initialized) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        return RTK_ERR_ALREADY_INIT;
    }
    
    /* 初始化上下文 */
    memset(&g_rtk_ctx, 0, sizeof(g_rtk_ctx));
    
    /* 初始化互斥锁 */
    pthread_mutex_init(&g_rtk_ctx.state_mutex, NULL);
    pthread_mutex_init(&g_rtk_ctx.callback_mutex, NULL);
    pthread_mutex_init(&g_rtk_ctx.gga_mutex, NULL);
    pthread_mutex_init(&g_rtk_ctx.broadcast_mutex, NULL);
    
    /* 拷贝配置 */
    memcpy(&g_rtk_ctx.config, config, sizeof(rtk_config_t));
    
    /* 设置默认值 */
    if (g_rtk_ctx.config.timeout_sec <= 0) {
        g_rtk_ctx.config.timeout_sec = 10;
    }
    if (g_rtk_ctx.config.broadcast_port <= 0) {
        g_rtk_ctx.config.broadcast_port = 9000;
    }
    if (strlen(g_rtk_ctx.config.broadcast_addr) == 0) {
        strcpy(g_rtk_ctx.config.broadcast_addr, "255.255.255.255");
    }
    
    g_rtk_ctx.broadcast_socket = -1;
    g_rtk_ctx.state = RTK_STATE_INIT;
    g_rtk_ctx.initialized = 1;
    
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    
    RTK_LOGI("RTK SDK初始化成功, 版本: %s", RTK_SDK_VERSION_STR);
    
    return RTK_OK;
}

void rtk_sdk_deinit(void) {
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    
    if (!g_rtk_ctx.initialized) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        return;
    }
    
    /* 停止运行 */
    if (g_rtk_ctx.running) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        rtk_sdk_stop();
        pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    }
    
    /* 禁用广播 */
    rtk_broadcast_deinit();
    
    /* 销毁互斥锁 */
    pthread_mutex_destroy(&g_rtk_ctx.callback_mutex);
    pthread_mutex_destroy(&g_rtk_ctx.gga_mutex);
    pthread_mutex_destroy(&g_rtk_ctx.broadcast_mutex);
    
    g_rtk_ctx.initialized = 0;
    g_rtk_ctx.state = RTK_STATE_IDLE;
    
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    pthread_mutex_destroy(&g_rtk_ctx.state_mutex);
    
    RTK_LOGI("RTK SDK已注销");
}

int rtk_sdk_start(void) {
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    
    if (!g_rtk_ctx.initialized) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        return RTK_ERR_NOT_INIT;
    }
    
    if (g_rtk_ctx.running) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        return RTK_ERR_ALREADY_RUNNING;
    }
    
    g_rtk_ctx.state = RTK_STATE_CONNECTING;
    g_rtk_ctx.running = 1;
    g_rtk_ctx.worker_running = 1;
    
    /* 创建工作线程 */
    int ret = pthread_create(&g_rtk_ctx.worker_thread, NULL, rtk_worker_thread, NULL);
    if (ret != 0) {
        g_rtk_ctx.running = 0;
        g_rtk_ctx.worker_running = 0;
        g_rtk_ctx.state = RTK_STATE_ERROR;
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        RTK_LOGE("创建工作线程失败: %d", ret);
        return RTK_ERR_THREAD;
    }
    
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    
    RTK_LOGI("RTK SDK启动");
    rtk_dispatch_status(RTK_STATE_CONNECTING, 0);
    
    return RTK_OK;
}

void rtk_sdk_stop(void) {
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    
    if (!g_rtk_ctx.running) {
        pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
        return;
    }
    
    g_rtk_ctx.state = RTK_STATE_STOPPING;
    g_rtk_ctx.worker_running = 0;
    
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    
    RTK_LOGI("正在停止RTK SDK...");
    
    /* 等待工作线程退出 */
    pthread_join(g_rtk_ctx.worker_thread, NULL);
    
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    g_rtk_ctx.running = 0;
    g_rtk_ctx.state = RTK_STATE_INIT;
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    
    RTK_LOGI("RTK SDK已停止");
    rtk_dispatch_status(RTK_STATE_INIT, 0);
}

rtk_state_t rtk_sdk_get_state(void) {
    rtk_state_t state;
    pthread_mutex_lock(&g_rtk_ctx.state_mutex);
    state = g_rtk_ctx.state;
    pthread_mutex_unlock(&g_rtk_ctx.state_mutex);
    return state;
}

const char* rtk_sdk_get_version(void) {
    return RTK_SDK_VERSION_STR;
}

/* ============================================================================
 * GGA输入API实现
 * ========================================================================== */

int rtk_sdk_input_gga(const char *gga_str, int len) {
    if (gga_str == NULL) {
        return RTK_ERR_NULL_PTR;
    }
    
    if (len <= 0 || len >= RTK_GGA_BUF_SIZE) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    /* 验证GGA格式 */
    if (rtk_gga_validate(gga_str, len) != RTK_OK) {
        return RTK_ERR_INVALID_GGA;
    }
    
    pthread_mutex_lock(&g_rtk_ctx.gga_mutex);
    memcpy(g_rtk_ctx.gga_buffer, gga_str, len);
    g_rtk_ctx.gga_buffer[len] = '\0';
    g_rtk_ctx.gga_len = len;
    g_rtk_ctx.gga_ready = 1;
    pthread_mutex_unlock(&g_rtk_ctx.gga_mutex);
    
    return RTK_OK;
}

int rtk_sdk_input_position(double lat, double lon, double alt) {
    char gga_buf[RTK_GGA_BUF_SIZE];
    int len = rtk_gga_build(gga_buf, sizeof(gga_buf), lat, lon, alt);
    if (len <= 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    return rtk_sdk_input_gga(gga_buf, len);
}

/* ============================================================================
 * 回调注册API实现
 * ========================================================================== */

void rtk_sdk_set_rtcm_callback(rtk_rtcm_callback_t cb, void *user_data) {
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    g_rtk_ctx.rtcm_cb = cb;
    g_rtk_ctx.rtcm_cb_data = user_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
}

void rtk_sdk_set_status_callback(rtk_status_callback_t cb, void *user_data) {
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    g_rtk_ctx.status_cb = cb;
    g_rtk_ctx.status_cb_data = user_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
}

void rtk_sdk_set_error_callback(rtk_error_callback_t cb, void *user_data) {
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    g_rtk_ctx.error_cb = cb;
    g_rtk_ctx.error_cb_data = user_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
}

void rtk_sdk_set_log_callback(rtk_log_callback_t cb, void *user_data) {
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    g_rtk_ctx.log_cb = cb;
    g_rtk_ctx.log_cb_data = user_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
}

/* ============================================================================
 * 广播控制API实现
 * ========================================================================== */

int rtk_sdk_enable_broadcast(int port, const char *broadcast_addr) {
    if (port <= 0 || port > 65535) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    const char *addr = broadcast_addr ? broadcast_addr : "255.255.255.255";
    return rtk_broadcast_init(port, addr);
}

void rtk_sdk_disable_broadcast(void) {
    rtk_broadcast_deinit();
}

int rtk_sdk_is_broadcast_enabled(void) {
    return g_rtk_ctx.broadcast_enabled;
}

/* ============================================================================
 * 日志控制API实现
 * ========================================================================== */

void rtk_sdk_set_log_level(rtk_log_level_t level) {
    g_rtk_ctx.config.log_level = level;
}

rtk_log_level_t rtk_sdk_get_log_level(void) {
    return g_rtk_ctx.config.log_level;
}

/* ============================================================================
 * 回调分发实现
 * ========================================================================== */

void rtk_dispatch_rtcm(const uint8_t *data, int len) {
    rtk_rtcm_callback_t cb = NULL;
    void *user_data = NULL;
    
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    cb = g_rtk_ctx.rtcm_cb;
    user_data = g_rtk_ctx.rtcm_cb_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
    
    if (cb) {
        cb(data, len, user_data);
    }
}

void rtk_dispatch_status(rtk_state_t state, int status_code) {
    rtk_status_callback_t cb = NULL;
    void *user_data = NULL;
    
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    cb = g_rtk_ctx.status_cb;
    user_data = g_rtk_ctx.status_cb_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
    
    if (cb) {
        cb(state, status_code, user_data);
    }
}

void rtk_dispatch_error(int code, const char *msg, int recoverable) {
    rtk_error_callback_t cb = NULL;
    void *user_data = NULL;

    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    cb = g_rtk_ctx.error_cb;
    user_data = g_rtk_ctx.error_cb_data;
    /* 保存最近一次错误信息，供 UDP 广播 diff_state/diff_err_msg 字段使用 */
    g_rtk_ctx.last_error_code = code;
    if (msg) {
        strncpy(g_rtk_ctx.last_error_msg, msg, sizeof(g_rtk_ctx.last_error_msg) - 1);
        g_rtk_ctx.last_error_msg[sizeof(g_rtk_ctx.last_error_msg) - 1] = '\0';
    } else {
        g_rtk_ctx.last_error_msg[0] = '\0';
    }
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
    
    if (cb) {
        rtk_error_info_t err = {
            .code = code,
            .msg = msg,
            .recoverable = recoverable
        };
        cb(&err, user_data);
    }
}

/* ============================================================================
 * GPS串口API实现
 * ========================================================================== */

#include "rtk_serial.h"

/* 定位回调相关 */
static rtk_position_callback_t g_position_cb = NULL;
static void *g_position_cb_data = NULL;

int rtk_sdk_open_gps_serial(const char *port, int baudrate) {
    if (port == NULL || strlen(port) == 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    rtk_serial_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.port, port, sizeof(cfg.port) - 1);
    cfg.baudrate = baudrate > 0 ? baudrate : 115200;
    cfg.databits = 8;
    cfg.stopbits = 1;
    cfg.parity = 'N';
    
    return rtk_serial_open(&cfg);
}

void rtk_sdk_close_gps_serial(void) {
    rtk_serial_close();
}

int rtk_sdk_is_gps_serial_open(void) {
    return rtk_serial_is_open();
}

int rtk_sdk_get_position(rtk_position_result_t *pos) {
    if (pos == NULL) {
        return RTK_ERR_NULL_PTR;
    }
    
    rtk_position_t internal_pos;
    int ret = rtk_serial_get_position(&internal_pos);
    if (ret != RTK_OK) {
        return ret;
    }
    
    /* 转换内部结构到公开结构 */
    pos->latitude = internal_pos.latitude;
    pos->longitude = internal_pos.longitude;
    pos->altitude = internal_pos.altitude;
    pos->hdop = internal_pos.hdop;
    pos->fix_quality = internal_pos.fix_quality;
    pos->satellites = internal_pos.satellites;
    pos->timestamp_ms = internal_pos.timestamp_ms;
    
    return RTK_OK;
}

void rtk_sdk_set_position_callback(rtk_position_callback_t cb, void *user_data) {
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    g_position_cb = cb;
    g_position_cb_data = user_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
}

/* 分发定位结果 */
void rtk_dispatch_position(const rtk_position_t *internal_pos) {
    if (internal_pos == NULL) return;
    
    rtk_position_callback_t cb = NULL;
    void *user_data = NULL;
    
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    cb = g_position_cb;
    user_data = g_position_cb_data;
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
    
    if (cb) {
        rtk_position_result_t pos = {
            .latitude = internal_pos->latitude,
            .longitude = internal_pos->longitude,
            .altitude = internal_pos->altitude,
            .hdop = internal_pos->hdop,
            .fix_quality = internal_pos->fix_quality,
            .satellites = internal_pos->satellites,
            .timestamp_ms = internal_pos->timestamp_ms
        };
        cb(&pos, user_data);
    }
}
