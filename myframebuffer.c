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

// NV12 转换为 XRGB8888（整数运算，超快）
void nv12_to_xrgb8888(unsigned char *nv12, unsigned char *xrgb8888, 
                    int width, int height)
{
    int y_size = width * height;
    unsigned char *y = nv12;
    unsigned char *uv = nv12 + y_size;
    
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y_index = i * width + j;
            int uv_index = (i / 2) * width + (j & ~1);

            int Y = y[y_index];
            int U = uv[uv_index] - 128;
            int V = uv[uv_index + 1] - 128;

            // 整数无浮点运算，速度提升巨大
            int R = Y + ((359 * V) >> 8);
            int G = Y - ((88 * U + 183 * V) >> 8);
            int B = Y + ((454 * U) >> 8);

            R = (R < 0) ? 0 : (R > 255 ? 255 : R);
            G = (G < 0) ? 0 : (G > 255 ? 255 : G);
            B = (B < 0) ? 0 : (B > 255 ? 255 : B);

            int rgb_index = (i * width + j) * 4;
            xrgb8888[rgb_index + 0] = B;
            xrgb8888[rgb_index + 1] = G;
            xrgb8888[rgb_index + 2] = R;
            xrgb8888[rgb_index + 3] = 0xFF;
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
    
    signal(SIGINT, signal_handler);
    
    // 1. 打开 Framebuffer
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) {
        perror("open /dev/fb0");
        printf("Try: modprobe rockchipdrm\n");
        return -1;
    }
    
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
    
    printf("Framebuffer: %dx%d, %d bpp, stride=%d\n", 
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length);
    
    long screensize = finfo.line_length * vinfo.yres;
    unsigned char *fbp = (unsigned char*)mmap(0, screensize, 
                                               PROT_READ | PROT_WRITE, 
                                               MAP_SHARED, fbfd, 0);
    if (fbp == MAP_FAILED) {
        perror("mmap framebuffer");
        close(fbfd);
        return -1;
    }
    
    memset(fbp, 0, screensize);
    
    // 2. 打开摄像头
    int fd = open(argv[1], O_RDWR);
    if (fd == -1)
    {
        perror("open video device");
        munmap(fbp, screensize);
        close(fbfd);
        return -1;
    }
    
    // 3. 查询能力
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
    {
        perror("query cap");
        close(fd);
        munmap(fbp, screensize);
        close(fbfd);
        return 1;
    }
    
    printf("Driver: %s\nCard: %s\n", cap.driver, cap.card);
    
    // 4. 设置格式
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = 800;
    fmt.fmt.pix_mp.height = 1280;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        fmt.fmt.pix_mp.num_planes = 2;
        if (ioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
            perror("set format");
            close(fd);
            munmap(fbp, screensize);
            close(fbfd);
            return -1;
        }
    }
    
    int width = fmt.fmt.pix_mp.width;
    int height = fmt.fmt.pix_mp.height;
    int num_planes = fmt.fmt.pix_mp.num_planes;
    printf("Video: %dx%d, planes=%d\n", width, height, num_planes);
    
    // 5. 请求 buffer
    struct v4l2_requestbuffers rb = {0};
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;
    
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) == -1)
    {
        perror("reqbufs");
        close(fd);
        munmap(fbp, screensize);
        close(fbfd);
        return -1;
    }
    
    // 6. 映射 buffer
    struct buffer_info *buffers = calloc(rb.count, sizeof(struct buffer_info));
    for (int i = 0; i < rb.count; ++i)
    {
        struct v4l2_buffer buf = {0};
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = {0};
        
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
        buf.index = i;
        
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) { perror("querybuf"); return -1; }
        
        buffers[i].num_planes = buf.length;
        memcpy(buffers[i].planes, planes, sizeof(planes));
        
        for (int j = 0; j < buf.length; ++j)
        {
            buffers[i].lengths[j] = buffers[i].planes[j].length;
            buffers[i].addr[j] = mmap(NULL, buffers[i].planes[j].length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      fd, buffers[i].planes[j].m.mem_offset);
            if (buffers[i].addr[j] == MAP_FAILED) { perror("mmap"); return -1; }
        }
    }
    
    // 入队
    for (int i = 0; i < rb.count; ++i)
    {
        struct v4l2_buffer buf = {0};
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = buffers[i].planes;
        buf.length = buffers[i].num_planes;
        ioctl(fd, VIDIOC_QBUF, &buf);
    }
    
    // 启动流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) == -1) { perror("streamon"); return -1; }
    
    printf("Start capture...\n");
    
    // ==============================
    // 这里是【最标准的 select 循环】
    // ==============================
    struct timeval start_time, now;
    gettimeofday(&start_time, NULL);
    int frame_count = 0;

    while (running)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        // 超时时间（1秒）
        struct timeval tv = {1, 0};

        // ========== 【核心：select 阻塞等待】 ==========
        int ret = select(fd + 1, &fds, NULL, NULL, &tv);

        if (ret < 0) {
            perror("select");
            break;
        }
        if (ret == 0) {
            printf("select timeout...\n");
            continue;
        }

        // 数据已就绪 → DQBUF 一定成功
        struct v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;

        if (ioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
            perror("dqbuf");
            continue;
        }

        // 取数据
        unsigned char *nv12_data = buffers[buf.index].addr[0];

        // 直接渲染到 FB
        nv12_to_xrgb8888(nv12_data, fbp, width, height);

        // 统计帧率
        frame_count++;
        gettimeofday(&now, NULL);
        double dt = (now.tv_sec - start_time.tv_sec) + (now.tv_usec - start_time.tv_usec)/1000000.0;
        if (dt >= 1.0) {
            printf("FPS: %.1f\n", frame_count / dt);
            frame_count = 0;
            start_time = now;
        }

        // 重新入队
        ioctl(fd, VIDIOC_QBUF, &buf);
    }

    // 清理
    printf("Cleaning up...\n");
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    munmap(fbp, screensize);
    close(fbfd);
    return 0;
}