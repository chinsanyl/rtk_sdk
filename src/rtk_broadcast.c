/**
 * @file rtk_broadcast.c
 * @brief RTK SDK UDP广播模块
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtk_internal.h"

/* ============================================================================
 * UDP广播初始化
 * ========================================================================== */

int rtk_broadcast_init(int port, const char *addr) {
    pthread_mutex_lock(&g_rtk_ctx.broadcast_mutex);
    
    /* 检查是否已启用 */
    if (g_rtk_ctx.broadcast_enabled) {
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return RTK_OK;
    }
    
    /* 创建UDP Socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        RTK_LOGE("创建UDP Socket失败: %s", strerror(errno));
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return RTK_ERR_SOCKET;
    }
    
    /* 设置广播选项 */
    int broadcast_enable = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        RTK_LOGE("设置广播选项失败: %s", strerror(errno));
        close(sock);
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return RTK_ERR_SOCKET;
    }
    
    /* 设置地址重用 */
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    /* 配置广播地址 */
    memset(&g_rtk_ctx.broadcast_addr, 0, sizeof(g_rtk_ctx.broadcast_addr));
    g_rtk_ctx.broadcast_addr.sin_family = AF_INET;
    g_rtk_ctx.broadcast_addr.sin_port = htons((uint16_t)port);
    
    if (inet_pton(AF_INET, addr, &g_rtk_ctx.broadcast_addr.sin_addr) <= 0) {
        RTK_LOGE("无效的广播地址: %s", addr);
        close(sock);
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return RTK_ERR_INVALID_PARAM;
    }
    
    g_rtk_ctx.broadcast_socket = sock;
    g_rtk_ctx.broadcast_enabled = 1;
    
    pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
    
    RTK_LOGI("UDP广播已启用, 端口: %d, 地址: %s", port, addr);
    
    return RTK_OK;
}

/* ============================================================================
 * UDP广播注销
 * ========================================================================== */

void rtk_broadcast_deinit(void) {
    pthread_mutex_lock(&g_rtk_ctx.broadcast_mutex);
    
    if (!g_rtk_ctx.broadcast_enabled) {
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return;
    }
    
    if (g_rtk_ctx.broadcast_socket >= 0) {
        close(g_rtk_ctx.broadcast_socket);
        g_rtk_ctx.broadcast_socket = -1;
    }
    
    g_rtk_ctx.broadcast_enabled = 0;
    
    pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
    
    RTK_LOGI("UDP广播已禁用");
}

/* ============================================================================
 * UDP广播发送
 * ========================================================================== */

int rtk_broadcast_send(const uint8_t *data, int len) {
    if (data == NULL || len <= 0) {
        return RTK_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_rtk_ctx.broadcast_mutex);
    
    if (!g_rtk_ctx.broadcast_enabled || g_rtk_ctx.broadcast_socket < 0) {
        pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
        return RTK_ERR_NOT_RUNNING;
    }
    
    ssize_t sent = sendto(g_rtk_ctx.broadcast_socket,
                          data,
                          (size_t)len,
                          0,
                          (struct sockaddr *)&g_rtk_ctx.broadcast_addr,
                          sizeof(g_rtk_ctx.broadcast_addr));
    
    pthread_mutex_unlock(&g_rtk_ctx.broadcast_mutex);
    
    if (sent < 0) {
        RTK_LOGW("UDP广播发送失败: %s", strerror(errno));
        return RTK_ERR_BROADCAST;
    }
    
    if (sent != len) {
        RTK_LOGW("UDP广播发送不完整: %zd/%d", sent, len);
    }
    
    RTK_LOGD("UDP广播发送: %d 字节", len);
    
    return RTK_OK;
}
