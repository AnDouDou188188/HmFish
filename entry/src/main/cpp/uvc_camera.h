// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ UvcCamera —— UVC 摄像头取流封装（uvc_camera.h）                              ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】libuvc 的 USB(UVC) 摄像头封装（saki UVCCamera 的 OHOS 移植、         ║
// ║   fd 注入式）：设备发现/打开 → UVC 流协商（格式/分辨率/帧率）→ 等时取流      ║
// ║   → MJPEG/YUYV 帧解码为 RGBA → 回调送 YuvPipeline::ProcessExternalRgba。    ║
// ║                                                                              ║
// ║ 【UVC 协议关键流程】                                                          ║
// ║   ① 描述符解析：uvc_open 时 libuvc 经注入的 fd 从 usbfs 读取设备/配置/       ║
// ║      接口/端点描述符，构建 stream_ifs（视频流接口）→ format_descs            ║
// ║      （MJPEG/YUY2 格式）→ frame_descs（每格式多档分辨率 + 帧率 intervals）   ║
// ║   ② 流协商：uvc_get_stream_ctrl_format_size_fps 以 PROBING_COMMIT 控制请求   ║
// ║      探测设备是否接受该 格式+分辨率+帧率 组合，成功则填回 uvc_stream_ctrl_t   ║
// ║      （bFormatIndex/bFrameIndex/dwFrameInterval 等，后续取流全靠它）         ║
// ║   ③ 等时传输：uvc_start_streaming_bandwidth 按协商带宽选等时端点             ║
// ║      altsetting，内核 usbfs 提交等时 URB；libuvc 取流线程收包重组 payload    ║
// ║      为完整帧后回调 FrameCb → OnFrame 解码投递                               ║
// ║                                                                              ║
// ║ 【线程模型】FrameCb/OnFrame（解码 + 供帧）运行在 libuvc 内部取流线程；       ║
// ║   Connect/Start/Stop/Disconnect 由 NAPI/ArkTS 侧线程调用；                  ║
// ║   跨线程状态一律用 atomic（connected_/streaming_/帧计数），                  ║
// ║   首帧诊断文本用 frameInfoMutex_ 互斥，避免回调线程写/NAPI 线程读竞争。      ║
// ║                                                                              ║
// ║ 【输出格式】RGBA（内存序 R,G,B,A，与 YuvPipeline 显示帧一致）；              ║
// ║   尺寸 = 实际协商分辨率（可能 ≠ 请求的 width/height）。                      ║
// ║                                                                              ║
// ║ 【与其它模块关系】YuvPipeline：RGBA 消费方（ProcessExternalRgba）；          ║
// ║   NAPI 层（usb 相机侧）：驱动 Connect/Start 状态机并回传诊断信息。           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
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
// ★ UvcCamera：单例 UVC 相机封装。
//   为什么单例：一个 USB 设备同一时刻只允许一个 libuvc 上下文占用，
//   多实例并发 open 会互相抢 fd 导致 -EBUSY；单例保证全局唯一连接状态与
//   诊断信息。生命周期：Connect（建 ctx→定位设备→open）→ Start（协商→取流）
//   → Stop（仅停流，句柄保留）→ Disconnect（全量释放，需重连）。
class UvcCamera {
public:
    // ───────── 单例与连接管理 ─────────

    // ★ Instance：进程级单例入口（C++11 magic static，首次调用时构造，
    //   之后的调用零开销；析构时机由编译器保证在进程退出前）
    static UvcCamera &Instance();

    // ★ Connect：以 ArkTS 侧经 @ohos.usbManager 申请到的设备 fd 建立连接。
    //   fd 注入的原因：OHOS 应用无直接 usbfs 节点权限，必须先在 ArkTS 侧
    //   拿到授权 fd 再交给 libuvc 在用户态完成枚举与后续 IO；
    //   每一步失败的原因（阶段 + libuvc 错误码 + errno）记入 GetLastError
    bool Connect(int fd, int vid, int pid, int busnum, int devaddr);
    // ★ Start：绑定渲染管线并启动取流。w/h/fps 仅为"期望值"——实际分辨率由
    //   NegotiateMax/AutoNegotiate 按设备能力决定（详见 .cpp 三级协商链），
    //   成功后经 ConfigureExternal 把管线（含预览 surface 比例）重配为实际尺寸
    bool Start(YuvPipeline *pipeline, int width, int height, int fps);
    // ★ Stop：仅停止等时取流（uvc_stop_streaming），保留设备句柄与上下文，
    //   支持 Stop 后直接再次 Start（换分辨率重开场景无需重新授权 fd）
    void Stop();
    // ★ Disconnect：全量释放（停流→close 设备→unref 设备→exit 上下文→
    //   释放解码缓冲），之后必须重新 Connect 才能再次取流
    void Disconnect();

    // ───────── 状态与诊断查询（NAPI 层跨线程读取）─────────

    // ★ IsConnected / IsStreaming：原子状态位，无锁读取
    bool IsConnected() const { return connected_.load(); }
    bool IsStreaming() const { return streaming_.load(); }
    // 最近一次 Connect/Start 失败的具体原因（阶段 + 错误码 + errno），
    // 供 NAPI 层取回展示/落盘，替代"连接失败"这种无信息提示
    std::string GetLastError() const { return lastError_; }
    // ★ DumpFormats：设备实际上报的格式/分辨率清单（诊断 -51 INVALID_MODE：
    //   协商找不到匹配格式时，看设备到底上报了什么）
    std::string DumpFormats() const;
    // ★ 取流诊断：累计收到/解码失败的帧数。
    //   连接成功≠有画面：接触不良/带宽不足时帧数为 0 或停增，
    //   ArkTS 侧看门狗据此给出明确提示（"已连接但无画面"）
    uint32_t FrameCount() const { return frameCount_.load(); }
    uint32_t DecodeFailCount() const { return decodeFailCount_.load(); }
    // ★ GetFrameInfo：首帧诊断（帧描述尺寸 vs JPEG 头真实尺寸 vs
    //   actual_bytes/帧头字节 + 解码返回码）：解码全失败时据此区分
    //   空帧(内核取流)/尺寸不符(相机固件)/数据损坏(带宽/接触)
    std::string GetFrameInfo();

private:
    UvcCamera() = default;
    ~UvcCamera();
    UvcCamera(const UvcCamera &) = delete;
    UvcCamera &operator=(const UvcCamera &) = delete;

    // ───────── 内部工具与协商 ─────────

    // ★ SetError：统一失败记录入口（阶段名 + 错误码 + 当前 errno），
    //   阶段名即调用链位置，配合 strerror 免去"猜哪一步挂了"
    void SetError(const char *stage, int code);
    // ★ NegotiateMax：遍历设备上报的 MJPEG/YUYV 格式，挑最大分辨率协商
    //   （优先 ≥15fps 的帧——避免协商到大分辨率但 5fps 的"幻灯片"档位）
    bool NegotiateMax();
    // ★ AutoNegotiate：遍历设备上报的 MJPEG/YUYV 全部格式与分辨率逐个协商，
    //   是固定参数全部失败（如 -51 INVALID_MODE）后的最后兜底
    bool AutoNegotiate();
    // ★ RecordFrameInfo：首帧诊断信息采集（OnFrame 里 n==1 时调用一次，
    //   之后诊断文本不再更新，避免高频回调下的字符串开销）
    void RecordFrameInfo(const uvc_frame_t *frame);

    // ───────── 帧回调（libuvc 取流线程）─────────

    // ★ FrameCb：libuvc 的 C 风格帧回调入口，仅做 user→this 转发进 OnFrame
    static void FrameCb(uvc_frame_t *frame, void *user);
    // ★ OnFrame：帧解码与投递核心（MJPEG→uvc_mjpeg2rgbx / YUYV→libyuv
    //   两路解码 + 帧计数/统计日志），运行在 libuvc 取流线程
    void OnFrame(uvc_frame_t *frame);

    // ───────── libuvc 对象链（生命周期：ctx → dev → devh，释放按逆序）─────────
    // libuvc 上下文（绑定 usbfs 根路径 "/dev/bus/usb"）
    uvc_context_t *ctx_ = nullptr;
    // 设备对象（fd + vid + pid 定位，open 前需持有引用）
    uvc_device_t *dev_ = nullptr;
    // 设备句柄（open 后才能读描述符、做控制传输与取流协商）
    uvc_device_handle_t *devh_ = nullptr;
    // 最近一次成功的流协商参数（bFormatIndex/bFrameIndex/dwFrameInterval 等）
    uvc_stream_ctrl_t ctrl_ = {};
    // MJPEG→RGBA 解码输出帧（首帧到达时按 w*h*4 懒分配，换分辨率时重建）
    uvc_frame_t *mjpegOut_ = nullptr;
    // 供帧目标管线（Start 绑定，Disconnect 置空；回调线程经它投 RGBA）
    YuvPipeline *pipeline_ = nullptr;
    // YUYV→RGBA 中间/输出双用途缓冲（先 YUY2ToARGB，再原地 ARGBToBGRA 换 R/B）
    std::vector<uint8_t> yuyvBuf_;

    // ───────── 状态与诊断（atomic/锁保护，供 NAPI 线程跨线程读取）─────────
    // 连接状态位（Connect 成功置 true，Disconnect 置 false）
    std::atomic<bool> connected_{false};
    // 取流状态位（uvc_start_streaming_bandwidth 成功后置 true）
    std::atomic<bool> streaming_{false};
    // 累计收到帧数（ArkTS 看门狗判断"有连接无画面"的依据）
    std::atomic<uint32_t> frameCount_{0};
    // 累计解码失败帧数（帧到了但画面黑 = 全解码失败，与 frameCount_ 区分）
    std::atomic<uint32_t> decodeFailCount_{0};
    // 5 秒周期统计日志的时间基准
    std::chrono::steady_clock::time_point lastStatTime_{};
    // 实际协商到的宽高（可能与请求 w/h 不同；ConfigureExternal 用它纠画面比例）
    int lastNegW_ = 0;
    int lastNegH_ = 0;
    // 保护 lastFrameInfo_（取流线程写 / NAPI 查询线程读）
    mutable std::mutex frameInfoMutex_;
    // 首帧诊断快照（RecordFrameInfo 一次性写入）
    std::string lastFrameInfo_;
    // 最近一次 Connect/Start 失败的 阶段+错误码+errno 描述
    std::string lastError_;
};

#endif  // UVC_CAMERA_H
