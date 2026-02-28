/**
 * @file rtk_gps_worker.c
 * @brief RTK SDK GPS自动模式工作线程
 * 
 * @details 实现完整的GPS数据链路：
 *          1. 从GPS串口读取原始NMEA数据
 *          2. 提取GGA发送给差分SDK获取RTCM
 *          3. 将RTCM差分数据发送回GPS模块
 *          4. 解析RTK定位结果并广播
 * 
 * @note 优化：缓冲区处理、串口断开重连
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "rtk_internal.h"
#include "rtk_serial.h"

/* ============================================================================
 * GPS工作线程上下文
 * ========================================================================== */
typedef struct {
    pthread_t thread;                   /* 线程句柄 */
    int running;                        /* 运行标志 */

    /* 接收缓冲区 */
    char recv_buf[4096];
    int recv_len;
    int buf_head;                       /* 优化：环形缓冲区头指针 */

    /* 统计 */
    uint64_t nmea_recv_count;           /* NMEA接收计数 */
    uint64_t rtcm_sent_count;           /* RTCM发送计数 */
    uint64_t position_count;            /* 定位结果计数 */
    uint64_t reconnect_count;           /* 重连次数 */

    /* 心跳与超时检测 */
    uint64_t last_broadcast_time;       /* 最近一次广播时间戳，用于定时心跳 */
    uint64_t last_nmea_time;            /* 最近一次收到有效NMEA的时间戳，用于检测GPS模块静默 */
} gps_worker_ctx_t;

static gps_worker_ctx_t g_gps_worker = {0};

/* 外部声明 */
extern void rtk_dispatch_position(const rtk_position_t *pos);

/* ============================================================================
 * NMEA数据处理（优化版：直接返回偏移量避免strstr）
 * ========================================================================== */

/**
 * @brief 从缓冲区中提取完整的NMEA语句
 * @param buf 缓冲区
 * @param len 缓冲区数据长度
 * @param sentence 输出语句
 * @param max_len 输出缓冲区大小
 * @param consumed 输出：消耗的字节数（包括语句前的垃圾数据）
 * @return 提取的NMEA语句长度，0表示无完整语句
 */
static int extract_nmea_sentence_v2(const char *buf, int len, char *sentence, int max_len, int *consumed) {
    *consumed = 0;
    
    /* 查找 $ 起始符 */
    const char *start = memchr(buf, '$', len);
    if (start == NULL) {
        /* 没有起始符，丢弃所有数据 */
        *consumed = len;
        return 0;
    }
    
    int offset = start - buf;
    int remaining = len - offset;
    
    /* 查找 \n 结束符 */
    const char *end = memchr(start, '\n', remaining);
    if (end == NULL) {
        /* 不完整语句，丢弃起始符之前的数据 */
        *consumed = offset;
        return 0;
    }
    
    int sentence_len = end - start + 1;
    if (sentence_len >= max_len) {
        /* 语句太长，跳过 */
        *consumed = offset + sentence_len;
        return 0;
    }
    
    memcpy(sentence, start, sentence_len);
    sentence[sentence_len] = '\0';
    
    /* 消耗的总字节数 = 起始符前的垃圾 + 语句本身 */
    *consumed = offset + sentence_len;
    
    return sentence_len;
}

/* ============================================================================
 * 广播辅助函数
 * ========================================================================== */

/**
 * @brief 获取差分服务状态码（用于 UDP 广播 diff_state 字段）
 *
 * diff_state 含义:
 *   0 = 未启动/已停止
 *   1 = 连接中（CONNECTING）
 *   2 = 运行正常（RUNNING）
 *   3 = 错误（ERROR）
 */
static int get_diff_state(void) {
    switch (rtk_sdk_get_state()) {
        case RTK_STATE_CONNECTING: return 1;
        case RTK_STATE_RUNNING:    return 2;
        case RTK_STATE_ERROR:      return 3;
        default:                   return 0;
    }
}

/**
 * @brief 广播定位结果（JSON格式，包含差分状态和GPS状态字段）
 *
 * 新增字段说明:
 *   diff_state - 差分服务状态（0未启动 1连接中 2运行中 3错误）
 *   diff_err_msg - 差分失败原因，正常时为空字符串
 *   gps_state  - GPS串口状态（1已连接无信号 2已连接有定位数据）
 */
static void broadcast_position(const rtk_position_t *pos) {
    if (pos == NULL) return;

    /* 读取差分错误信息（加锁保护） */
    char diff_err_msg[128] = {0};
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    strncpy(diff_err_msg, g_rtk_ctx.last_error_msg, sizeof(diff_err_msg) - 1);
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);

    /* gps_state: 1=串口已连但无卫星信号(fix=0), 2=有定位数据(fix>0) */
    int gps_state = (pos->fix_quality > 0) ? 2 : 1;

    /* 构造 JSON */
    char json[768];
    int len = snprintf(json, sizeof(json),
        "{"
        "\"type\":\"rtk_position\","
        "\"diff_state\":%d,"
        "\"diff_err_msg\":\"%s\","
        "\"gps_state\":%d,"
        "\"lat\":%.8f,"
        "\"lon\":%.8f,"
        "\"alt\":%.3f,"
        "\"hdop\":%.2f,"
        "\"fix\":%d,"
        "\"sat\":%d,"
        "\"ts\":%llu"
        "}",
        get_diff_state(),
        diff_err_msg,
        gps_state,
        pos->latitude,
        pos->longitude,
        pos->altitude,
        pos->hdop,
        pos->fix_quality,
        pos->satellites,
        (unsigned long long)pos->timestamp_ms
    );

    /* 广播 */
    if (g_rtk_ctx.broadcast_enabled) {
        rtk_broadcast_send((const uint8_t *)json, len);
    }

    /* 仅在有效定位时分发给回调 */
    if (pos->fix_quality > 0) {
        rtk_dispatch_position(pos);
        g_gps_worker.position_count++;
    }

    g_gps_worker.last_broadcast_time = rtk_get_time_ms();
}

/**
 * @brief 广播无定位状态心跳包
 *
 * @param gps_state GPS 状态码：
 *   0 = GPS 串口断开或未就绪（硬连接下极少发生）
 *   3 = 串口正常但 GPS 模块无任何 NMEA 输出（疑似模块故障）
 *
 * 位置数据全部为 0，接收端不应使用位置字段。
 */
static void broadcast_heartbeat(int gps_state) {
    /* 读取差分错误信息 */
    char diff_err_msg[128] = {0};
    pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
    strncpy(diff_err_msg, g_rtk_ctx.last_error_msg, sizeof(diff_err_msg) - 1);
    pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{"
        "\"type\":\"rtk_position\","
        "\"diff_state\":%d,"
        "\"diff_err_msg\":\"%s\","
        "\"gps_state\":%d,"
        "\"lat\":0.0,"
        "\"lon\":0.0,"
        "\"alt\":0.0,"
        "\"hdop\":0.0,"
        "\"fix\":0,"
        "\"sat\":0,"
        "\"ts\":%llu"
        "}",
        get_diff_state(),
        diff_err_msg,
        gps_state,
        (unsigned long long)rtk_get_time_ms()
    );

    if (g_rtk_ctx.broadcast_enabled) {
        rtk_broadcast_send((const uint8_t *)json, len);
    }

    g_gps_worker.last_broadcast_time = rtk_get_time_ms();
    RTK_LOGD("心跳广播: gps_state=%d, diff_state=%d", gps_state, get_diff_state());
}

/* ============================================================================
 * GPS工作线程（集成串口重连）
 * ========================================================================== */

static void* gps_worker_thread(void *arg) {
    (void)arg;
    
    uint8_t read_buf[1024];
    char sentence[512];
    rtk_position_t pos;
    uint64_t last_log_time = 0;
    int consumed = 0;
    
    RTK_LOGI("GPS工作线程启动");
    
    while (g_gps_worker.running) {
        uint64_t now = rtk_get_time_ms();

        /* 检查串口状态，需要时重连 */
        rtk_serial_state_t state = rtk_serial_get_state();
        if (state == RTK_SERIAL_STATE_ERROR) {
            /* 串口异常：定时广播心跳（gps_state=0），让接收端感知服务仍在运行 */
            if (now - g_gps_worker.last_broadcast_time >= RTK_GPS_HEARTBEAT_INTERVAL_MS) {
                broadcast_heartbeat(0);
            }
            RTK_LOGW("检测到串口异常，尝试重连...");
            int ret = rtk_serial_reconnect();
            if (ret != RTK_OK) {
                RTK_LOGE("串口重连失败，等待后重试");
                g_gps_worker.reconnect_count++;
                rtk_sleep_ms(RTK_SERIAL_RECONNECT_DELAY_MS);
                continue;
            }
            /* 重连成功，重置NMEA计时器（给模块启动时间） */
            g_gps_worker.reconnect_count++;
            g_gps_worker.recv_len     = 0;
            g_gps_worker.last_nmea_time = rtk_get_time_ms();
        } else if (state != RTK_SERIAL_STATE_OPEN) {
            /* 串口未就绪（初始化中/关闭中）：定时广播心跳 */
            if (now - g_gps_worker.last_broadcast_time >= RTK_GPS_HEARTBEAT_INTERVAL_MS) {
                broadcast_heartbeat(0);
            }
            rtk_sleep_ms(100);
            continue;
        }

        /* GPS模块静默检测：串口正常但长时间无NMEA输出（疑似模块故障） */
        if (g_gps_worker.last_nmea_time > 0 &&
            now - g_gps_worker.last_nmea_time >= RTK_GPS_NO_DATA_TIMEOUT_MS &&
            now - g_gps_worker.last_broadcast_time >= RTK_GPS_HEARTBEAT_INTERVAL_MS) {
            broadcast_heartbeat(3);
        }

        /* 从串口读取数据 */
        int n = rtk_serial_read(read_buf, sizeof(read_buf) - 1, 100);
        if (n < 0) {
            /* 读取错误，串口模块会自动更新状态 */
            RTK_LOGW("GPS串口读取错误: %d", n);
            rtk_sleep_ms(100);
            continue;
        }
        
        if (n == 0) {
            continue;  /* 超时，继续循环 */
        }
        
        /* 追加到缓冲区 */
        if (g_gps_worker.recv_len + n < (int)sizeof(g_gps_worker.recv_buf)) {
            memcpy(g_gps_worker.recv_buf + g_gps_worker.recv_len, read_buf, n);
            g_gps_worker.recv_len += n;
        } else {
            /* 缓冲区溢出，保留后半部分 */
            int keep = sizeof(g_gps_worker.recv_buf) / 2;
            memmove(g_gps_worker.recv_buf, 
                    g_gps_worker.recv_buf + g_gps_worker.recv_len - keep,
                    keep);
            g_gps_worker.recv_len = keep;
            /* 追加新数据 */
            memcpy(g_gps_worker.recv_buf + g_gps_worker.recv_len, read_buf, n);
            g_gps_worker.recv_len += n;
            RTK_LOGD("GPS接收缓冲区溢出，保留后半部分");
        }
        
        /* 尝试提取NMEA语句（优化版） */
        while (g_gps_worker.recv_len > 0) {
            int sent_len = extract_nmea_sentence_v2(
                g_gps_worker.recv_buf, 
                g_gps_worker.recv_len,
                sentence, 
                sizeof(sentence),
                &consumed
            );
            
            if (consumed > 0 && consumed <= g_gps_worker.recv_len) {
                /* 移除已消耗的数据 */
                memmove(g_gps_worker.recv_buf, 
                        g_gps_worker.recv_buf + consumed,
                        g_gps_worker.recv_len - consumed);
                g_gps_worker.recv_len -= consumed;
            }
            
            if (sent_len == 0) {
                break;  /* 没有完整语句了 */
            }
            
            g_gps_worker.nmea_recv_count++;
            
            /* 检查是否为GGA语句 */
            if (strstr(sentence, "GGA") != NULL) {
                /* 先解析以获取 fix_quality，再决定是否向差分SDK发送 */
                if (rtk_serial_parse_nmea(sentence, sent_len, &pos) == RTK_OK) {
                    /* 仅在有效定位(fix>0)时才向差分SDK上报GGA，无信号时上报无意义 */
                    if (pos.fix_quality > 0) {
                        rtk_sdk_input_gga(sentence, sent_len);
                    }

                    /* 无论 fix 是否有效都广播（接收端需要感知 gps_state） */
                    broadcast_position(&pos);

                    /* 更新NMEA时间戳（用于GPS模块静默检测） */
                    g_gps_worker.last_nmea_time = now;

                    /* 周期性日志（仅有效定位时记录） */
                    if (pos.fix_quality > 0 && now - last_log_time >= 5000) {
                        RTK_LOGI("RTK定位: %.8f, %.8f, 高度:%.2fm, 质量:%d, 卫星:%d",
                                 pos.latitude, pos.longitude, pos.altitude,
                                 pos.fix_quality, pos.satellites);
                        last_log_time = now;
                    }
                }
            }
        }
    }
    
    RTK_LOGI("GPS工作线程退出");
    return NULL;
}

/* ============================================================================
 * RTCM数据回调（发送给GPS模块）
 * ========================================================================== */

/**
 * @brief 当收到RTCM差分数据时，发送给GPS模块
 */
void rtk_gps_on_rtcm_data(const uint8_t *data, int len, void *user_data) {
    (void)user_data;
    
    if (rtk_serial_get_state() != RTK_SERIAL_STATE_OPEN) {
        return;
    }
    
    int ret = rtk_serial_send_rtcm(data, len);
    if (ret == RTK_OK) {
        g_gps_worker.rtcm_sent_count++;
        RTK_LOGD("RTCM发送到GPS: %d 字节", len);
    }
}

/* ============================================================================
 * GPS自动模式启动/停止
 * ========================================================================== */

int rtk_gps_worker_start(const char *port, int baudrate) {
    if (g_gps_worker.running) {
        return RTK_OK;
    }
    
    /* 打开串口 */
    int ret = rtk_sdk_open_gps_serial(port, baudrate);
    if (ret != RTK_OK) {
        return ret;
    }
    
    /* 注册RTCM回调（收到RTCM后发送给GPS） */
    rtk_sdk_set_rtcm_callback(rtk_gps_on_rtcm_data, NULL);
    
    /* 启动工作线程 */
    g_gps_worker.running = 1;
    g_gps_worker.recv_len = 0;
    g_gps_worker.nmea_recv_count = 0;
    g_gps_worker.rtcm_sent_count = 0;
    g_gps_worker.position_count = 0;
    g_gps_worker.reconnect_count = 0;
    g_gps_worker.last_broadcast_time = 0;  /* 置0使首次心跳立即触发 */
    g_gps_worker.last_nmea_time = rtk_get_time_ms(); /* 给模块启动预留10s缓冲 */

    ret = pthread_create(&g_gps_worker.thread, NULL, gps_worker_thread, NULL);
    if (ret != 0) {
        g_gps_worker.running = 0;
        rtk_sdk_close_gps_serial();
        RTK_LOGE("创建GPS工作线程失败: %d", ret);
        return RTK_ERR_THREAD;
    }
    
    RTK_LOGI("GPS自动模式已启动");
    return RTK_OK;
}

void rtk_gps_worker_stop(void) {
    if (!g_gps_worker.running) {
        return;
    }
    
    g_gps_worker.running = 0;
    
    /* 等待线程退出 */
    pthread_join(g_gps_worker.thread, NULL);
    
    /* 关闭串口 */
    rtk_sdk_close_gps_serial();
    
    /* 取消RTCM回调 */
    rtk_sdk_set_rtcm_callback(NULL, NULL);
    
    RTK_LOGI("GPS自动模式已停止 (NMEA:%llu, RTCM发送:%llu, 定位:%llu, 重连:%llu)",
             (unsigned long long)g_gps_worker.nmea_recv_count,
             (unsigned long long)g_gps_worker.rtcm_sent_count,
             (unsigned long long)g_gps_worker.position_count,
             (unsigned long long)g_gps_worker.reconnect_count);
}

int rtk_gps_worker_is_running(void) {
    return g_gps_worker.running;
}
