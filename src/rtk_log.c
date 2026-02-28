/**
 * @file rtk_log.c
 * @brief RTK SDK日志模块实现
 * 
 * @note 优化：添加全局日志开关、输出级别独立控制
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "rtk_internal.h"

/* ============================================================================
 * 日志配置
 * ========================================================================== */

/* 全局日志开关 */
static int g_log_enabled = 1;

/* 控制台输出开关 */
static int g_log_console_enabled = 1;

/* 文件输出开关 */
static int g_log_file_enabled = 1;

/* 日志回调开关 */
static int g_log_callback_enabled = 1;

/* ============================================================================
 * 日志级别名称
 * ========================================================================== */
static const char* g_log_level_names[] = {
    [RTK_LOG_OFF]   = "OFF",
    [RTK_LOG_ERROR] = "ERROR",
    [RTK_LOG_WARN]  = "WARN",
    [RTK_LOG_INFO]  = "INFO",
    [RTK_LOG_DEBUG] = "DEBUG"
};

/* ============================================================================
 * 日志开关控制
 * ========================================================================== */

/**
 * @brief 设置全局日志开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_enabled(int enabled) {
    g_log_enabled = enabled ? 1 : 0;
}

/**
 * @brief 获取全局日志开关状态
 * @return 1启用 0禁用
 */
int rtk_log_is_enabled(void) {
    return g_log_enabled;
}

/**
 * @brief 设置控制台输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_console_enabled(int enabled) {
    g_log_console_enabled = enabled ? 1 : 0;
}

/**
 * @brief 设置文件输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_file_enabled(int enabled) {
    g_log_file_enabled = enabled ? 1 : 0;
}

/**
 * @brief 设置回调输出开关
 * @param enabled 1启用 0禁用
 */
void rtk_log_set_callback_enabled(int enabled) {
    g_log_callback_enabled = enabled ? 1 : 0;
}

/* ============================================================================
 * 日志输出实现
 * ========================================================================== */

void rtk_log(rtk_log_level_t level, const char *fmt, ...) {
    /* 全局开关检查 */
    if (!g_log_enabled) {
        return;
    }
    
    /* 日志级别检查 */
    if (level == RTK_LOG_OFF || level > g_rtk_ctx.config.log_level) {
        return;
    }
    
    char buf[RTK_LOG_BUF_SIZE];
    char time_buf[32];
    va_list args;
    
    /* 获取时间戳 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    
    /* 格式化日志内容 */
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    /* 输出到控制台 */
    if (g_log_console_enabled) {
        fprintf(level == RTK_LOG_ERROR ? stderr : stdout,
                "[%s][RTK][%s] %s\n",
                time_buf,
                g_log_level_names[level],
                buf);
        fflush(level == RTK_LOG_ERROR ? stderr : stdout);
    }
    
    /* 输出到文件 */
    if (g_log_file_enabled && strlen(g_rtk_ctx.config.log_file) > 0) {
        FILE *fp = fopen(g_rtk_ctx.config.log_file, "a");
        if (fp) {
            fprintf(fp, "[%s][RTK][%s] %s\n", time_buf, g_log_level_names[level], buf);
            fclose(fp);
        }
    }
    
    /* 调用用户回调 */
    if (g_log_callback_enabled) {
        rtk_log_callback_t cb = NULL;
        void *user_data = NULL;
        
        pthread_mutex_lock(&g_rtk_ctx.callback_mutex);
        cb = g_rtk_ctx.log_cb;
        user_data = g_rtk_ctx.log_cb_data;
        pthread_mutex_unlock(&g_rtk_ctx.callback_mutex);
        
        if (cb) {
            cb(level, buf, user_data);
        }
    }
}
