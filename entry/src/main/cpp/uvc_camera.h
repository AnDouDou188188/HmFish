// ★ UVC 摄像头取流头文件：单例连接/协商/取流接口
#ifndef UVC_CAMERA_H
#define UVC_CAMERA_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "libuvc/libuvc.h"

class YuvPipeline;

// saki UVCCamera 的 OHOS 移植：fd 注入式 UVC 相机封装
//   Connect(fd, vid, pid, busnum, devaddr)
//     → uvc_init2(ctx, NULL, "/dev/bus/usb")
//     → uvc_get_device_with_fd(ctx, &dev, vid, pid, NULL, fd, busnum, devaddr)
//     → uvc_open
//   Start(pipeline, w, h, fps)：★ 直接上设备支持的最高分辨率（NegotiateMax：
//     MJPEG 优先挑最大帧，优先 ≥15fps；失败回退固定 w×h 参数链，再失败
//     AutoNegotiate 按设备实际上报格式逐个协商），
//     帧回调 MJPEG→uvc_mjpeg2rgbx / YUYV→libyuv YUY2ToARGB(+R/B 交换)
class UvcCamera {
public:
    static UvcCamera &Instance();

    bool Connect(int fd, int vid, int pid, int busnum, int devaddr);
    bool Start(YuvPipeline *pipeline, int width, int height, int fps);
    void Stop();
    void Disconnect();

    bool IsConnected() const { return connected_.load(); }
    bool IsStreaming() const { return streaming_.load(); }
    // 最近一次 Connect/Start 失败的具体原因（阶段 + 错误码 + errno），
    // 供 NAPI 层取回展示/落盘，替代"连接失败"这种无信息提示
    std::string GetLastError() const { return lastError_; }
    // ★ 设备实际上报的格式/分辨率清单（诊断 -51 INVALID_MODE：协商找不到匹配格式）
    std::string DumpFormats() const;
    // ★ 取流诊断：累计收到/解码失败的帧数。
    //   连接成功≠有画面：接触不良/带宽不足时帧数为 0 或停增，
    //   ArkTS 侧看门狗据此给出明确提示（"已连接但无画面"）
    uint32_t FrameCount() const { return frameCount_.load(); }
    uint32_t DecodeFailCount() const { return decodeFailCount_.load(); }
    // ★ 首帧诊断（帧描述尺寸 vs JPEG 头真实尺寸 vs actual_bytes/帧头字节 + 解码返回码）：
    //   解码全失败时据此区分 空帧(内核取流)/尺寸不符(相机固件)/数据损坏(带宽/接触)
    std::string GetFrameInfo();

private:
    UvcCamera() = default;
    ~UvcCamera();
    UvcCamera(const UvcCamera &) = delete;
    UvcCamera &operator=(const UvcCamera &) = delete;

    void SetError(const char *stage, int code);
    // ★ 遍历设备上报的 MJPEG/YUYV 格式，挑最大分辨率协商（优先 ≥15fps 的帧）
    bool NegotiateMax();
    // ★ 遍历设备上报的 MJPEG/YUYV 格式与分辨率，逐个尝试协商
    bool AutoNegotiate();
    // ★ 首帧诊断信息采集（OnFrame 里 n==1 时调用一次）
    void RecordFrameInfo(const uvc_frame_t *frame);

    static void FrameCb(uvc_frame_t *frame, void *user);
    void OnFrame(uvc_frame_t *frame);

    uvc_context_t *ctx_ = nullptr;
    uvc_device_t *dev_ = nullptr;
    uvc_device_handle_t *devh_ = nullptr;
    uvc_stream_ctrl_t ctrl_ = {};
    uvc_frame_t *mjpegOut_ = nullptr;
    YuvPipeline *pipeline_ = nullptr;
    std::vector<uint8_t> yuyvBuf_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<uint32_t> frameCount_{0};
    std::atomic<uint32_t> decodeFailCount_{0};
    std::chrono::steady_clock::time_point lastStatTime_{};
    int lastNegW_ = 0;
    int lastNegH_ = 0;
    mutable std::mutex frameInfoMutex_;
    std::string lastFrameInfo_;
    std::string lastError_;
};

#endif  // UVC_CAMERA_H
