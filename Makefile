# 设置内核源码路径和当前路径
KERNELDIR := /home/dxy/linux/rk3568_sdk/kernel/
CURRENT_PATH := $(shell pwd)

# 设置交叉编译工具链
CROSS_COMPILE := aarch64-none-linux-gnu-
CC := $(CROSS_COMPILE)gcc
ARCH := arm64

SYSROOT = /home/dxy/linux/rk3568_sdk/buildroot/output/rockchip_atk_dlrk3568/host/aarch64-buildroot-linux-gnu/sysroot

CFLAGS += -I$(SYSROOT)/usr/include
LDFLAGS += -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib -lSDL2

# 驱动编译目标 (所有内核模块)
obj-m := 

# 用户态测试程序
APP1 :=  myframebuffer

APP2 :=  image

APP_NAME := image myframebuffer
# 默认编译所有目标
all: kernel_modules $(APP_NAME)

# 编译内核驱动模块
kernel_modules:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	rm *.symvers *.order

# 编译用户态测试程序
$(APP1): $(APP1).c
	$(CC) $< -o $@  $(CFLAGS) $(LDFLAGS) --sysroot=$(SYSROOT) 

$(APP2): $(APP2).c
	$(CC) $< -o $@  $(CFLAGS) $(LDFLAGS) --sysroot=$(SYSROOT) 

# 清理编译生成的临时文件
clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURRENT_PATH) clean
	rm -rf $(APP_NAME)

