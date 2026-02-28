/**
 * @file rtk_gga.c
 * @brief RTK SDK GGA数据处理模块
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "rtk_internal.h"

/* ============================================================================
 * GGA格式验证
 * ========================================================================== */

int rtk_gga_validate(const char *gga_str, int len) {
    if (gga_str == NULL || len <= 0) {
        return RTK_ERR_NULL_PTR;
    }
    
    /* 检查起始字符 */
    if (gga_str[0] != '$') {
        RTK_LOGD("GGA验证失败: 不以$开头");
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 检查结束字符 */
    if (len < 2 || gga_str[len-1] != '\n') {
        RTK_LOGD("GGA验证失败: 不以\\n结尾");
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 检查是否为GGA语句 */
    if (strstr(gga_str, "GGA") == NULL) {
        RTK_LOGD("GGA验证失败: 不包含GGA");
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 检查校验和分隔符 */
    const char *asterisk = strchr(gga_str, '*');
    if (asterisk == NULL) {
        RTK_LOGD("GGA验证失败: 缺少*校验和分隔符");
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 计算校验和 */
    unsigned char checksum = 0;
    for (const char *p = gga_str + 1; p < asterisk; p++) {
        checksum ^= (unsigned char)(*p);
    }
    
    /* 解析接收到的校验和 */
    unsigned int recv_checksum = 0;
    if (sscanf(asterisk + 1, "%02X", &recv_checksum) != 1) {
        RTK_LOGD("GGA验证失败: 校验和格式错误");
        return RTK_ERR_INVALID_GGA;
    }
    
    /* 验证校验和 */
    if (checksum != (unsigned char)recv_checksum) {
        RTK_LOGD("GGA验证失败: 校验和不匹配 (计算:%02X, 接收:%02X)", checksum, recv_checksum);
        return RTK_ERR_INVALID_GGA;
    }
    
    return RTK_OK;
}

/* ============================================================================
 * 构造GGA语句
 * ========================================================================== */

int rtk_gga_build(char *buf, int buf_size, double lat, double lon, double alt) {
    if (buf == NULL || buf_size < 100) {
        return -1;
    }
    
    /* 验证经纬度范围 */
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return -1;
    }
    
    /* 获取当前UTC时间 */
    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    
    /* 转换纬度为NMEA格式 (ddmm.mmmm) */
    char lat_dir = lat >= 0 ? 'N' : 'S';
    lat = fabs(lat);
    int lat_deg = (int)lat;
    double lat_min = (lat - lat_deg) * 60.0;
    
    /* 转换经度为NMEA格式 (dddmm.mmmm) */
    char lon_dir = lon >= 0 ? 'E' : 'W';
    lon = fabs(lon);
    int lon_deg = (int)lon;
    double lon_min = (lon - lon_deg) * 60.0;
    
    /* 构造GGA语句（不含校验和） */
    int len = snprintf(buf, buf_size - 5,
                       "$GPGGA,%02d%02d%02d,%02d%09.6f,%c,%03d%09.6f,%c,1,08,1.0,%.3f,M,0.0,M,,",
                       utc->tm_hour, utc->tm_min, utc->tm_sec,
                       lat_deg, lat_min, lat_dir,
                       lon_deg, lon_min, lon_dir,
                       alt);
    
    if (len <= 0 || len >= buf_size - 5) {
        return -1;
    }
    
    /* 计算校验和 */
    unsigned char checksum = 0;
    for (int i = 1; i < len; i++) {
        checksum ^= (unsigned char)buf[i];
    }
    
    /* 添加校验和和结束符 */
    int total = snprintf(buf + len, buf_size - len, "*%02X\r\n", checksum);
    
    return len + total;
}
