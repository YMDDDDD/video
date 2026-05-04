#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <SDL2/SDL.h> 
int main(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("Usage: %s /dev/video0 \n", argv[0]);
        return -1;
    }

    int fd = open(argv[1], O_RDWR);
    if (fd == -1)
    {
        perror("ERROR: open video device");
        return -1;  
    }

    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == -1)
    {
        perror("Error querying capabilities");
        close(fd);
        return 1;
    }

        // 检查是否支持单平面捕获
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        printf("Device does not support single-plane capture\n");
        close(fd);
        return -1;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    // printf("Driver: %s\n", cap.driver);
    // printf("Card: %s\n", cap.card);
    // printf("capabilities: %#X\n", cap.capabilities);
    // printf("device_caps: %#X\n", cap.device_caps);

    // int fmtindex = 0;
    // int sizeindex = 0;
    // struct v4l2_fmtdesc fmt;
    // 
    // fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    // fmt.index = 0;

    // // 枚举格式
    // while (1)
    // {
    //     memset(&fmt, 0, sizeof(fmt));
    //     fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    //     fmt.index = fmtindex++;

    //     int ret = ioctl(fd, VIDIOC_ENUM_FMT, &fmt);
    //     if (ret == -1)
    //     {
    //         if (errno == EINVAL)
    //         {
    //             printf("\nAll formats enumerated.\n");
    //             break;
    //         }
    //         else
    //         {
    //             perror("ioctl VIDIOC_ENUM_FMT");
    //             break;
    //         }
    //     }

    //     // 打印格式
    //     printf("\n=== Format %d: %s (0x%08x) ===\n",
    //            fmt.index, fmt.description, fmt.pixelformat);

    //     // ======================
    //     // 枚举该格式支持的分辨率
    //     // ======================
    //     struct v4l2_frmsizeenum fsize;
    //     sizeindex = 0; // 必须从 0 开始

    //     while (1)
    //     {
    //         // 每次必须清零！！！
    //         memset(&fsize, 0, sizeof(fsize));
    //         fsize.index = sizeindex++;
    //         fsize.pixel_format = fmt.pixelformat;

    //         ret = ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsize);
    //         if (ret == -1)
    //         {
    //             if (errno == EINVAL)
    //             {
    //                 printf("  No more frames.\n");
    //                 break;
    //             }
    //             else
    //             {
    //                 perror("ioctl VIDIOC_ENUM_FRAMESIZES");
    //                 break;
    //             }
    //         }

    //         if (fsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
    //         {
    //             printf("  Resolution: %dx%d\n",
    //                    fsize.discrete.width,
    //                    fsize.discrete.height);
    //         }
    //         else if (fsize.type == V4L2_FRMSIZE_TYPE_STEPWISE)
    //         {
    //             printf("  Stepwise: %dx%d -> %dx%d, step %dx%d\n",
    //                    fsize.stepwise.min_width,
    //                    fsize.stepwise.min_height,
    //                    fsize.stepwise.max_width,
    //                    fsize.stepwise.max_height,
    //                    fsize.stepwise.step_width,
    //                    fsize.stepwise.step_height);
    //         }
    //     }
    // }

    struct v4l2_format fmt1;
    memset(&fmt1, 0, sizeof(struct v4l2_format));
    fmt1.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt1.fmt.pix_mp.width = 1920;
    fmt1.fmt.pix_mp.height = 1080;
    fmt1.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt1.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt1.fmt.pix_mp.num_planes = 2; // NV12 是2 plane

    if (ioctl(fd, VIDIOC_S_FMT, &fmt1) == -1)
    {
        perror("VIDIOC_S_FMT");
    }
    printf("num_planes = %d\n", fmt1.fmt.pix_mp.num_planes);

    // 请求buffer
    struct v4l2_requestbuffers rb = {0};
    rb.count = 4;
    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rb.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd, VIDIOC_REQBUFS, &rb) == -1)
    {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    printf("Driver allocated %d buffers\n", rb.count);

    // 定义 buffer 信息结构
    struct buffer_info
    {
        void *addr[VIDEO_MAX_PLANES];
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        unsigned int num_planes;
    };
    struct buffer_info *buffers = calloc(rb.count, sizeof(struct buffer_info));
    if (!buffers)
    {
        perror("calloc failed");
        return -1;
    }

    // 查询并映射 buffers
    int i = 0;
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
        }

        buffers[i].num_planes = buf.length;
        memcpy(buffers[i].planes, planes, sizeof(struct v4l2_plane) * buf.length);

        printf("\nBuffer %d (%u planes):\n", i, buf.length);
        int j = 0;
        for (j = 0; j < buf.length; ++j)
        {
            buffers[i].addr[j] = mmap(NULL,
                                      buffers[i].planes[j].length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED,
                                      fd,
                                      buffers[i].planes[j].m.mem_offset);

            if (buffers[i].addr[j] == MAP_FAILED)
            {
                perror("mmap failed");
                
            }

            printf("  plane %d: addr=%p, size=%u, offset=%u\n",
                   j, buffers[i].addr[j],
                   buffers[i].planes[j].length,
                   buffers[i].planes[j].m.mem_offset);
        }
    }

    // 入队所有 buffers
    
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
            
        }
    }
    printf("All buffers queued successfully\n");

    

    /* 启动摄像头 */
    if (0 != ioctl(fd, VIDIOC_STREAMON, &type))
    {
        perror("Unable to start capture");
        return -1;
    }
    printf("start capture ok\n");

    close(fd);
    return 0;
}