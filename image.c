#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <stdint.h>  // 解决 uint8_t / uint32_t 未定义问题
#define VIDEO_MAX_PLANES 8

struct buffer_info {
    void *addr[VIDEO_MAX_PLANES];
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    unsigned int num_planes;
    unsigned int lengths[VIDEO_MAX_PLANES];
};

int running = 1;

void signal_handler(int sig)
{
    running = 0;
}

// NV12 转换为 RGB888
void nv12_to_rgb888(unsigned char *nv12, unsigned char *rgb888, 
                    int width, int height)
{
    int y_size = width * height;
    unsigned char *y = nv12;
    unsigned char *uv = nv12 + y_size;
    
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y_index = i * width + j;
            int uv_index = (i / 2) * width + (j / 2) * 2;
            
            int y_val = y[y_index];
            int u_val = uv[uv_index] - 128;
            int v_val = uv[uv_index + 1] - 128;
            
            // YUV to RGB
            int r = y_val + 1.402 * v_val;
            int g = y_val - 0.344 * u_val - 0.714 * v_val;
            int b = y_val + 1.772 * u_val;
            
            // int c = y_val - 16;
            // int d = u_val;
            // int e = v_val;

            // int r = (298*c + 409*e + 128) >> 8;
            // int g = (298*c - 100*d - 208*e + 128) >> 8;
            // int b = (298*c + 516*d + 128) >> 8;
            // 限制范围
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            
            // RGB888
            int rgb_index = (i * width + j) * 3;
            rgb888[rgb_index] = r;     // R
            rgb888[rgb_index + 1] = g; // G
            rgb888[rgb_index + 2] = b; // B
        }
    }
}

// NV12 转换为 XRGB*888
// void nv12_to_xrgb8888(unsigned char *nv12, unsigned char *xrgb8888, 
//                     int width, int height)
// {
//     int y_size = width * height;
//     unsigned char *y = nv12;
//     unsigned char *uv = nv12 + y_size;
    
//     for (int i = 0; i < height; i++) {
//         for (int j = 0; j < width; j++) {
//             int y_index = i * width + j;
//             int uv_index = (i / 2) * width + (j / 2) * 2;
            
//             int y_val = y[y_index];
//             int u_val = uv[uv_index] - 128;
//             int v_val = uv[uv_index + 1] - 128;
            
//             // YUV to RGB
//             int r = y_val + 1.402 * v_val;
//             int g = y_val - 0.344 * u_val - 0.714 * v_val;
//             int b = y_val + 1.772 * u_val;
            
        
//             // 限制范围
//             if (r < 0) r = 0; if (r > 255) r = 255;
//             if (g < 0) g = 0; if (g > 255) g = 255;
//             if (b < 0) b = 0; if (b > 255) b = 255;
            
//             // XRGB8888
//             int rgb_index = (i * width + j) * 4;
//             xrgb8888[rgb_index] = b;  // X
//             xrgb8888[rgb_index + 1] = g; // R
//             xrgb8888[rgb_index + 2] = r; // G
//             xrgb8888[rgb_index + 3] = 0xff; // B
//         }
//     }
// }


void nv12_to_xrgb8888(unsigned char *nv12, unsigned char *xrgb8888, 
                      int width, int height, int x_offset, int y_offset)
{
    int y_size = width * height;
    unsigned char *y = nv12;
    unsigned char *uv = nv12 + y_size;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y_index = i * width + j;
            int uv_index = (i / 2) * width + (j & ~1); // 比 (j/2)*2 更快

            int Y = y[y_index];
            int U = uv[uv_index] - 128;
            int V = uv[uv_index + 1] - 128;

            // 整数运算，无浮点，速度提升巨大
            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            // 限幅
            R = (R < 0) ? 0 : (R > 255 ? 255 : R);
            G = (G < 0) ? 0 : (G > 255 ? 255 : G);
            B = (B < 0) ? 0 : (B > 255 ? 255 : B);

            int rgb_index = ((i + y_offset) * 800 + j + x_offset) * 4;
            xrgb8888[rgb_index + 0] = B;
            xrgb8888[rgb_index + 1] = G;
            xrgb8888[rgb_index + 2] = R;
            xrgb8888[rgb_index + 3] = 0xFF;
        }
    }
}


// 极致优化版 NV12 → XRGB8888（无浮点、无慢运算、循环紧凑）
static inline void nv12_to_xrgb_fast(uint8_t *nv12, uint8_t *fb, int w, int h, int x_off, int y_off, int fb_stride)
{
    const int y_size = w * h;
    uint8_t *y = nv12;
    uint8_t *uv = nv12 + y_size;

    // 预计算帧缓冲行步长（像素数）
    const int fb_pitch = fb_stride / 4;

    for (int i = 0; i < h; i++) {
        // 直接计算目标行地址，减少循环内运算
        uint32_t *dst = (uint32_t *)(fb + (y_off + i) * fb_stride + x_off * 4);

        for (int j = 0; j < w; j++) {
            // UV 地址优化：比 & ~1 更快
            int uv_idx = ((i >> 1) * w) + (j & 0xFE);
            int Y = y[i * w + j];
            int U = uv[uv_idx] - 128;
            int V = uv[uv_idx + 1] - 128;

            // 纯整数移位运算，无乘法
            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            // 分支预测优化：用位运算替代三元判断
            R = R < 0 ? 0 : (R > 255 ? 255 : R);
            G = G < 0 ? 0 : (G > 255 ? 255 : G);
            B = B < 0 ? 0 : (B > 255 ? 255 : B);

            // 一次性写入32位，比单字节写快3倍
            *dst++ = (0xFF << 24) | (R << 16) | (G << 8) | B;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s /dev/video0 \n", argv[0]);
        return -1;
    }
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    
    // 1. 打开 Framebuffer
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        perror("open /dev/fb0");
        printf("Try: modprobe rockchipdrm\n");
        return -1;
    }
    
    // 获取 Framebuffer 信息
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("FBIOGET_FSCREENINFO");
        close(fbfd);
        return -1;
    }
    
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("FBIOGET_VSCREENINFO");
        close(fbfd);
        return -1;
    }
    
    printf("Framebuffer: %dx%d, %d bpp, stride=%d, RGB888=%s\n", 
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, 
           finfo.line_length,
           vinfo.bits_per_pixel == 32 ? "yes (XRGB8888)" : "no");
    
    // 检查是否支持 RGB888 (32位色深)
    if (vinfo.bits_per_pixel != 32) {
        printf("Warning: Framebuffer is not 32-bit, trying to set RGB888 mode...\n");
        vinfo.bits_per_pixel = 32;
        vinfo.red.offset = 16;
        vinfo.red.length = 8;
        vinfo.green.offset = 8;
        vinfo.green.length = 8;
        vinfo.blue.offset = 0;
        vinfo.blue.length = 8;
        vinfo.transp.offset = 24;
        vinfo.transp.length = 8;
        
        if (ioctl(fbfd, FBIOPUT_VSCREENINFO, &vinfo) == -1) {
            perror("FBIOPUT_VSCREENINFO");
            printf("Cannot set RGB888 mode, continuing anyway...\n");
        }
    }
    
    // 映射 Framebuffer
    long screensize = finfo.line_length * vinfo.yres;
    unsigned char *fbp = (unsigned char*)mmap(0, screensize, 
                                               PROT_READ | PROT_WRITE, 
                                               MAP_SHARED, fbfd, 0);
    if (fbp == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fbfd);
        return -1;
    }
    
    // 清屏（黑色）
    memset(fbp, 0, screensize);
    
    // 2. 打开摄像头
    int fd = open(argv[1], O_RDWR);
    if (fd == -1)
    {
        perror("ERROR: open video device");
        munmap(fbp, screensize);
        close(fbfd);
        return -1;
    }
    
    // 3. 查询能力
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
    {
        perror("Error querying capabilities");
        close(fd);
        munmap(fbp, screensize);
        close(fbfd);
        return 1;
    }
    
    printf("Driver: %s\n", cap.driver);
    printf("Card: %s\n", cap.card);
    
    // 4. 设置摄像头格式（800x1280 NV12）
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = 800;
    fmt.fmt.pix_mp.height = 1280;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        // 尝试2个plane
        fmt.fmt.pix_mp.num_planes = 2;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
            perror("VIDIOC_S_FMT");
            close(fd);
            munmap(fbp, screensize);
            close(fbfd);
            return -1;
        }
    }
    
    int width = fmt.fmt.pix_mp.width;
    int height = fmt.fmt.pix_mp.height;
    int num_planes = fmt.fmt.pix_mp.num_planes;
    printf("Set format: %dx%d, num_planes=%d\n", width, height, num_planes);
    
    // 5. 请求buffer
    struct v4l2_requestbuffers rb = {0};
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) == -1)
    {
        perror("VIDIOC_REQBUFS");
        close(fd);
        munmap(fbp, screensize);
        close(fbfd);
        return -1;
    }
    printf("Driver allocated %d buffers\n", rb.count);
    
    // 6. 分配buffer信息
    struct buffer_info *buffers = calloc(rb.count, sizeof(struct buffer_info));
    if (!buffers)
    {
        perror("calloc failed");
        close(fd);
        munmap(fbp, screensize);
        close(fbfd);
        return -1;
    }
    
    // 7. 映射buffers
    int i, j;
    for (i = 0; i < rb.count; ++i)
    {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        buf.index = i;
        
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1)
        {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }
        
        buffers[i].num_planes = buf.length;
        memcpy(buffers[i].planes, planes, sizeof(struct v4l2_plane) * buf.length);
        
        for (j = 0; j < buf.length; ++j)
        {
            buffers[i].lengths[j] = buffers[i].planes[j].length;
            buffers[i].addr[j] = mmap(NULL,
                                      buffers[i].planes[j].length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED,
                                      fd,
                                      buffers[i].planes[j].m.mem_offset);
            
            if (buffers[i].addr[j] == MAP_FAILED)
            {
                perror("mmap failed");
                return -1;
            }
            
            printf("Buffer %d plane %d: addr=%p, size=%u\n", 
                   i, j, buffers[i].addr[j], buffers[i].planes[j].length);
        }
    }
    
    // 8. 入队所有buffers
    for (i = 0; i < rb.count; ++i)
    {
        struct v4l2_buffer buf = {0};
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = buffers[i].planes;
        buf.length = buffers[i].num_planes;
        
        if (ioctl(fd, VIDIOC_QBUF, &buf) != 0)
        {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }
    printf("All buffers queued\n");
    
    // 9. 启动摄像头
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1)
    {
        perror("Unable to start capture");
        return -1;
    }
    printf("Start capture, displaying on framebuffer...\n");
    printf("Screen: %dx%d, Camera: %dx%d\n", vinfo.xres, vinfo.yres, width, height);
    
    // // 10. 分配RGB888缓冲区
    // unsigned char *rgb_buffer = malloc(width * height * 3); // RGB888
    // if (!rgb_buffer) {
    //     perror("malloc rgb_buffer");
    //     return -1;
    // }
    
    // 计算显示位置（居中）
    int dst_x = (vinfo.xres - width) / 2;
    int dst_y = (vinfo.yres - height) / 2;
    if (dst_x < 0) dst_x = 0;
    if (dst_y < 0) dst_y = 0;
    
    printf("Display position: x=%d, y=%d\n", dst_x, dst_y);
    
    struct v4l2_buffer buf;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    int frame_count = 0;
    struct timeval start_time, current_time;
    long seconds, useconds;
    double elapsed_time;
    
       // 记录程序开始时间
    gettimeofday(&start_time, NULL);

    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    // int frame_count = 0;
    while (running) {
       
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        
        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            perror("VIDIOC_DQBUF");
            break;
        }

        if (buf.index < rb.count) {
            unsigned char *nv12_data;
            if (num_planes == 1) {
                nv12_data = buffers[buf.index].addr[0];
            } else {
                static unsigned char *merged = NULL;
                static size_t merged_size = 0;
                size_t y_size = width * height;
                size_t uv_size = width * height / 2;

                if (!merged || merged_size < y_size + uv_size) {
                    if (merged) free(merged);
                    merged = malloc(y_size + uv_size);
                    merged_size = y_size + uv_size;
                }

                if (merged) {
                    memcpy(merged, buffers[buf.index].addr[0], y_size);
                    memcpy(merged + y_size, buffers[buf.index].addr[1], uv_size);
                    nv12_data = merged;
                } else {
                    nv12_data = buffers[buf.index].addr[0];
                }
            }

            // 转换为 RGB888
            // nv12_to_rgb888(nv12_data, rgb_buffer, width, height);
            nv12_to_xrgb8888(nv12_data,fbp, width, height,dst_x,dst_y);
            // nv12_to_xrgb_fast(nv12_data,fbp, width, height,dst_x,dst_y,3200);
            // 显示到 Framebuffer
            // int bytes_per_pixel = vinfo.bits_per_pixel / 8;
            // for (int y = 0; y < height && (dst_y + y) < vinfo.yres; y++) {
            //     unsigned char *dst = fbp + (dst_y + y) * finfo.line_length + dst_x * bytes_per_pixel;
            //     unsigned char *src = rgb_buffer + y * width * 3;
                
            //     if (bytes_per_pixel == 4) {
            //         // XRGB8888 格式
            //         for (int x = 0; x < width && (dst_x + x) < vinfo.xres; x++) {
            //             dst[x*4] = src[x*3 + 2];     // B
            //             dst[x*4 + 1] = src[x*3 + 1]; // G
            //             dst[x*4 + 2] = src[x*3];     // R
            //             dst[x*4 + 3] = 0xFF;         // Alpha
            //         }
            //     } else if (bytes_per_pixel == 3) {
            //         // RGB888 格式
            //         memcpy(dst, src, width * 3);
            //     } else {
            //         // 其他格式，只复制 RGB
            //         for (int x = 0; x < width && (dst_x + x) < vinfo.xres; x++) {
            //             dst[x*3] = src[x*3];
            //             dst[x*3+1] = src[x*3+1];
            //             dst[x*3+2] = src[x*3+2];
            //         }
            //     }
            // }
            
            frame_count++;

            // 每秒输出一次帧率
            gettimeofday(&current_time, NULL);
            seconds = current_time.tv_sec - start_time.tv_sec;
            useconds = current_time.tv_usec - start_time.tv_usec;
            elapsed_time = seconds + useconds / 1000000.0;

            // 每秒钟输出一次帧率
            if (elapsed_time >= 1.0) {
                double fps = frame_count / elapsed_time;
                printf("Frame rate: %.2f FPS\n", fps);

                // 重置时间和计数器
                gettimeofday(&start_time, NULL);
                frame_count = 0;
            }
        }

        // 重新入队
        if (ioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            perror("VIDIOC_QBUF");
            break;
        }

        // 简单延时控制帧率 (~30fps)
        // usleep(5000);
    }
    
    // 11. 清理
    printf("Cleaning up...\n");
    
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    
    // free(rgb_buffer);
    for (i = 0; i < rb.count; ++i) {
        for (j = 0; j < buffers[i].num_planes; ++j) {
            if (buffers[i].addr[j] != MAP_FAILED) {
                munmap(buffers[i].addr[j], buffers[i].planes[j].length);
            }
        }
    }
    free(buffers);
    
    rb.count = 0;
    ioctl(fd, VIDIOC_REQBUFS, &rb);
    
    close(fd);
    munmap(fbp, screensize);
    close(fbfd);
    
    printf("Done. Total frames: %d\n", frame_count);
    return 0;
}