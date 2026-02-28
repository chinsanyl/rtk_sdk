/**
 * @file rtk_config.c
 * @brief RTK SDK配置文件解析模块
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "rtk_internal.h"

/* ============================================================================
 * 辅助函数
 * ========================================================================== */

/* 去除字符串首尾空白 */
static char* trim(char *str) {
    if (str == NULL) return NULL;
    
    /* 去除尾部空白 */
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    
    /* 去除首部空白 */
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    
    return str;
}

/* ============================================================================
 * 配置文件解析
 * ========================================================================== */

/**
 * @brief 从INI格式配置文件加载配置
 * @param config 输出配置结构体
 * @param filepath 配置文件路径
 * @return RTK_OK成功，负值失败
 */
int rtk_config_load(rtk_config_t *config, const char *filepath) {
    if (config == NULL || filepath == NULL) {
        return RTK_ERR_NULL_PTR;
    }
    
    FILE *fp = fopen(filepath, "r");
    if (fp == NULL) {
        RTK_LOGE("无法打开配置文件: %s", filepath);
        return RTK_ERR_FILE;
    }
    
    /* 初始化默认值 */
    memset(config, 0, sizeof(rtk_config_t));
    config->timeout_sec = 10;
    config->use_https = 0;
    config->broadcast_port = 9000;
    strcpy(config->broadcast_addr, "255.255.255.255");
    config->log_level = RTK_LOG_INFO;
    
    char line[512];
    char section[64] = {0};
    
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *trimmed = trim(line);
        
        /* 跳过空行和注释 */
        if (trimmed[0] == '\0' || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        
        /* 解析节 [section] */
        if (trimmed[0] == '[') {
            char *end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                strncpy(section, trimmed + 1, sizeof(section) - 1);
            }
            continue;
        }
        
        /* 解析键值对 key = value */
        char *eq = strchr(trimmed, '=');
        if (eq == NULL) {
            continue;
        }
        
        *eq = '\0';
        char *key = trim(trimmed);
        char *value = trim(eq + 1);
        
        /* 根据节和键设置配置 */
        if (strcmp(section, "auth") == 0) {
            if (strcmp(key, "ak") == 0) {
                strncpy(config->ak, value, RTK_AK_MAX_LEN - 1);
            } else if (strcmp(key, "as") == 0) {
                strncpy(config->as, value, RTK_AS_MAX_LEN - 1);
            } else if (strcmp(key, "device_id") == 0) {
                strncpy(config->device_id, value, RTK_DEVICE_ID_MAX_LEN - 1);
            } else if (strcmp(key, "device_type") == 0) {
                strncpy(config->device_type, value, RTK_DEVICE_TYPE_MAX_LEN - 1);
            }
        } else if (strcmp(section, "network") == 0) {
            if (strcmp(key, "timeout") == 0) {
                config->timeout_sec = atoi(value);
            } else if (strcmp(key, "use_https") == 0) {
                config->use_https = atoi(value);
            }
        } else if (strcmp(section, "broadcast") == 0) {
            if (strcmp(key, "port") == 0) {
                config->broadcast_port = atoi(value);
            } else if (strcmp(key, "address") == 0) {
                strncpy(config->broadcast_addr, value, RTK_IP_ADDR_MAX_LEN - 1);
            }
        } else if (strcmp(section, "log") == 0) {
            if (strcmp(key, "level") == 0) {
                config->log_level = (rtk_log_level_t)atoi(value);
            } else if (strcmp(key, "file") == 0) {
                strncpy(config->log_file, value, sizeof(config->log_file) - 1);
            }
        } else if (strcmp(section, "gps_serial") == 0) {
            if (strcmp(key, "port") == 0) {
                strncpy(config->gps_serial_port, value, sizeof(config->gps_serial_port) - 1);
            } else if (strcmp(key, "baudrate") == 0) {
                config->gps_serial_baudrate = atoi(value);
            } else if (strcmp(key, "auto_mode") == 0) {
                config->gps_auto_mode = atoi(value);
            }
        }
    }
    
    fclose(fp);
    
    RTK_LOGI("配置文件加载成功: %s", filepath);
    
    return RTK_OK;
}

/**
 * @brief 验证配置有效性
 * @param config 配置结构体
 * @return RTK_OK有效，负值无效
 */
int rtk_config_validate(const rtk_config_t *config) {
    if (config == NULL) {
        return RTK_ERR_NULL_PTR;
    }
    
    if (strlen(config->ak) == 0) {
        RTK_LOGE("配置验证失败: AK为空");
        return RTK_ERR_INVALID_PARAM;
    }
    
    if (strlen(config->as) == 0) {
        RTK_LOGE("配置验证失败: AS为空");
        return RTK_ERR_INVALID_PARAM;
    }
    
    if (strlen(config->device_id) == 0) {
        RTK_LOGE("配置验证失败: device_id为空");
        return RTK_ERR_INVALID_PARAM;
    }
    
    if (strlen(config->device_type) == 0) {
        RTK_LOGE("配置验证失败: device_type为空");
        return RTK_ERR_INVALID_PARAM;
    }
    
    if (config->timeout_sec <= 0 || config->timeout_sec > 60) {
        RTK_LOGW("timeout_sec超出范围，使用默认值10");
    }
    
    if (config->broadcast_port <= 0 || config->broadcast_port > 65535) {
        RTK_LOGW("broadcast_port无效，使用默认值9000");
    }
    
    return RTK_OK;
}

/**
 * @brief 打印配置信息（调试用）
 * @param config 配置结构体
 */
void rtk_config_dump(const rtk_config_t *config) {
    if (config == NULL) return;
    
    RTK_LOGI("========== RTK SDK配置 ==========");
    RTK_LOGI("AK: %s", config->ak);
    RTK_LOGI("AS: %s...（已隐藏）", config->as[0] ? "***" : "");
    RTK_LOGI("Device ID: %s", config->device_id);
    RTK_LOGI("Device Type: %s", config->device_type);
    RTK_LOGI("Timeout: %d 秒", config->timeout_sec);
    RTK_LOGI("HTTPS: %s", config->use_https ? "是" : "否");
    RTK_LOGI("广播端口: %d", config->broadcast_port);
    RTK_LOGI("广播地址: %s", config->broadcast_addr);
    RTK_LOGI("日志级别: %d", config->log_level);
    RTK_LOGI("日志文件: %s", strlen(config->log_file) > 0 ? config->log_file : "(无)");
    RTK_LOGI("GPS串口: %s", strlen(config->gps_serial_port) > 0 ? config->gps_serial_port : "(未配置)");
    RTK_LOGI("GPS波特率: %d", config->gps_serial_baudrate > 0 ? config->gps_serial_baudrate : 115200);
    RTK_LOGI("GPS自动模式: %s", config->gps_auto_mode ? "启用" : "禁用");
    RTK_LOGI("==================================");
}
