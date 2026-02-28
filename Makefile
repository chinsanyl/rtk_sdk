# RTK SDK Makefile
# 支持编译动态库、静态库和独立可执行文件

# 编译器设置
CC ?= gcc
AR ?= ar
CROSS_COMPILE ?=

# 使用交叉编译前缀
ifneq ($(CROSS_COMPILE),)
    CC := $(CROSS_COMPILE)gcc
    AR := $(CROSS_COMPILE)ar
endif

# 目录设置
TOP_DIR := $(shell pwd)
SRC_DIR := $(TOP_DIR)/src
INC_DIR := $(TOP_DIR)/include
MAIN_DIR := $(TOP_DIR)/main
BUILD_DIR := $(TOP_DIR)/build
LIB_DIR := $(TOP_DIR)/lib

# 六分SDK路径（内置于thirdparty目录）
SIXENTS_SDK_DIR := $(TOP_DIR)/thirdparty/sixents
SIXENTS_INC_DIR := $(SIXENTS_SDK_DIR)/inc
SIXENTS_LIB_DIR := $(SIXENTS_SDK_DIR)/lib

# 编译选项
CFLAGS := -Wall -Wextra -O2 -fPIC
CFLAGS += -I$(INC_DIR) -I$(SRC_DIR) -I$(SIXENTS_INC_DIR)

# 调试模式
ifdef DEBUG
    CFLAGS += -g -DRTK_DEBUG
endif

# 链接选项
LDFLAGS := -L$(SIXENTS_LIB_DIR) -lsixents-core-sdk -lpthread -lm

# 源文件
SRCS := $(SRC_DIR)/rtk_core.c \
        $(SRC_DIR)/rtk_log.c \
        $(SRC_DIR)/rtk_gga.c \
        $(SRC_DIR)/rtk_broadcast.c \
        $(SRC_DIR)/rtk_config.c \
        $(SRC_DIR)/rtk_serial.c \
        $(SRC_DIR)/rtk_gps_worker.c

MAIN_SRC := $(MAIN_DIR)/rtk_main.c

# 目标文件
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
MAIN_OBJ := $(BUILD_DIR)/rtk_main.o

# 输出文件
SHARED_LIB := $(LIB_DIR)/librtk_sdk.so
STATIC_LIB := $(LIB_DIR)/librtk_sdk.a
EXECUTABLE := $(BUILD_DIR)/rtk_service

# 默认目标
.PHONY: all clean install lib service dirs

all: dirs lib service

dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(LIB_DIR)

# 编译库
lib: dirs $(SHARED_LIB) $(STATIC_LIB)

# 编译可执行文件
service: dirs $(EXECUTABLE)

# 动态库
$(SHARED_LIB): $(OBJS)
	@echo "链接动态库: $@"
	$(CC) -shared -o $@ $^ $(LDFLAGS)

# 静态库
$(STATIC_LIB): $(OBJS)
	@echo "创建静态库: $@"
	$(AR) rcs $@ $^

# 可执行文件
$(EXECUTABLE): $(MAIN_OBJ) $(OBJS)
	@echo "链接可执行文件: $@"
	$(CC) -o $@ $^ $(LDFLAGS)

# 编译规则
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "编译: $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rtk_main.o: $(MAIN_DIR)/rtk_main.c
	@echo "编译: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	@echo "清理..."
	rm -rf $(BUILD_DIR) $(LIB_DIR)

# 安装
install: all
	@echo "安装到 /usr/local..."
	install -d /usr/local/include
	install -d /usr/local/lib
	install -d /usr/local/bin
	install -m 644 $(INC_DIR)/rtk_sdk.h /usr/local/include/
	install -m 755 $(SHARED_LIB) /usr/local/lib/
	install -m 644 $(STATIC_LIB) /usr/local/lib/
	install -m 755 $(EXECUTABLE) /usr/local/bin/
	ldconfig

# 显示信息
info:
	@echo "RTK SDK 构建配置"
	@echo "================"
	@echo "编译器: $(CC)"
	@echo "六分SDK: $(SIXENTS_SDK_DIR)"
	@echo "输出目录: $(BUILD_DIR)"
	@echo "库目录: $(LIB_DIR)"
