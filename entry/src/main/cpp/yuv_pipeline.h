// ★ 渲染管线核心头文件：YuvPipeline 类——帧处理/推理/叠加/推流/快照/变焦/
//   位置水印/录像直供/SR 工作线程的成员与接口声明
#ifndef YUV_PIPELINE_H
#define YUV_PIPELINE_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <thread>                  // ★ 新增
#include <condition_variable>      // ★ 新增

#include <multimedia/image_framework/image/image_receiver_native.h>
#include <multimedia/image_framework/image/image_native.h>
#include <native_window/external_window.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

#include "yolo_detector.h"

struct DetectItem { float x, y, w, h, score; int label; };

// ★ 人脸识别结果快照（face 工作线程写入，绘制线程/napi 读取）
struct FaceDrawInfo {
    float box[4];       // x1,y1,x2,y2（图像坐标）
    float score;        // 检测分
    int gender;         // 0=女 1=男 -1=未知
    float age;          // -1=未知
    float beauty;       // 0~100，-1=未知
    float comp[7];      // 颜值分项（各 0~100，-1=未知）：对称/眼形/眉形/鼻形/嘴形/三庭/眼型
    float pts[212];     // 106 关键点 x,y（图像坐标）
    std::chrono::steady_clock::time_point ts;   // 结果时间戳（>1.5s 视为过期丢弃）
};

class YuvPipeline {
public:
    YuvPipeline() = default;
    ~YuvPipeline();

    bool CreateReceiver(int width, int height, std::string &outSurfaceId);
    void Start();
    void Stop();

    // ★ 手机相机路径：createYuvReceiver 先用占位尺寸（相机档位在 init 后才确定），
    //   init 完成后按实际选中的预览档位更新管线处理尺寸——保留旋转/镜像，
    //   重分配转换缓冲并同步预览 surface geometry，否则按 640x480 处理 1440x1440
    //   帧导致 UV 平面偏移错位（颜色错乱、录像内容错乱）
    bool UpdateFrameSize(int width, int height);

    // ★ UVC（USB 摄像头）外部供帧模式：无 ImageReceiver，
    // 由 uvc_camera 帧回调直接注入 RGBA
    bool ConfigureExternal(int width, int height);
    void StartExternal();
    void ProcessExternalRgba(const uint8_t *rgba, int w, int h);

    std::vector<DetectItem> GetLatestResults();
    // ★ 懒快照：拍照用，直接拷贝当前帧（含叠加层：框/水印/OCR 框）
    std::vector<uint8_t> GetLatestRgba();
    void GetFrameSize(int &w, int &h);
    uint64_t GetInferCount() const { return inferCount_.load(); }

    // ★ 干净帧快照（表格识别专用）：GetLatestRgba 含自绘叠加层
    //   （框/水印/OCR 框），识别会把水印当成文字。本机制请求管线在"叠加绘制前"
    //   的干净帧点抓一帧，条件变量同步等待（最长 timeoutMs）
    void RequestCleanSnapshot();
    bool WaitCleanSnapshot(int timeoutMs, std::vector<uint8_t> &out, int &w, int &h);

    void SetPreviewWindow(OHNativeWindow *win);
    void SetYoloEnabled(bool enabled);

    void LoadFont(const std::string &fontPath);
    void SetClassNames(const std::vector<std::string> &names);

    void SetRotation(int degrees);
    void SetMirror(bool mirror);
    // ★ USB 摄像头数字变焦（照抄 Android UVCPreview nativeSetZoomScale）：
    //   中心裁剪 1/zoom 后放大回原尺寸，clamp 1.0~5.0
    void SetZoomScale(float scale);
    void UpdateDisplaySize();
    void ClearSurface();
    // ★ 位置水印文字（逆地理解析后的地理位置名），空串=不绘制
    void SetLocationText(const std::string &text);
    // ★ 位置水印纵向位置：ratio∈[0.4,1.0]，水印条画在 H*ratio 处（1.0=帧底）
    void SetLocationBottomRatio(float ratio);

    // ★ 人脸识别 HUD 信息（JSON：count/faces[gender,age,beauty,x,y,w,h]），过期结果自动剔除
    std::string GetFaceInfoJson();

    // ★ 录像不中断：AVRecorder 输入 surface 交给 native 直接喂 NV12 帧，
    //   相机/预览/识别全程不重配不中断（对齐 Android 录像架构）
    bool AttachRecordSurface(uint64_t surfaceId, int width, int height);
    void DetachRecordSurface();

private:
    void OnImageArrival();
    void ProcessFrame(OH_ImageNative *image);
    void ProcessCommonTail();   // OCR 触发→推理→叠加→上屏 的公共尾部
    void StoreResults(const std::vector<DetectItem> &items);
    void DrawOverlayOnRgba();
    void DrawOcrOverlay();
    bool PushToSurface();
    bool PushPixelsToSurface(const uint8_t *src, int srcW, int srcH);
    bool PushToRecordSurface(const uint8_t *src, int srcW, int srcH);

    void drawTextCN(uint8_t *rgba, int W, int H,
                    const std::string &text, int x, int y,
                    int fontHeight, uint8_t r, uint8_t g, uint8_t b);
    int  measureTextCN(const std::string &text, int fontHeight);

    // ★ 新增：异步推理
    void InferenceLoop();
    void RequestInference(const uint8_t *rgba, int w, int h);

    OH_ImageReceiverNative *receiver_ = nullptr;
    int width_ = 640;
    int height_ = 480;
    std::vector<uint8_t> rgbaBuf_;
    bool running_ = false;

    std::mutex callbackMutex_;

    std::mutex resultsMutex_;
    std::vector<DetectItem> latestResults_;
    std::chrono::steady_clock::time_point lastNonEmptyResultsTime_{};   // ★ 新增
    std::atomic<uint64_t> inferCount_{0};
    std::chrono::steady_clock::time_point lastInferTime_;
    std::chrono::steady_clock::time_point lastRenderTime_;   // ★ 帧率节流

    OHNativeWindow *previewWindow_ = nullptr;
    std::mutex previewWindowMutex_;
    std::atomic<bool> yoloEnabled_{true};

    FT_Library ftLib_ = nullptr;
    FT_Face    ftFace_ = nullptr;
    std::mutex ftMutex_;

    std::vector<std::string> classNames_;
    std::mutex classNamesMutex_;

    int displayWidth_ = 0;
    int displayHeight_ = 0;
    int rotationDegrees_ = 0;
    std::atomic<bool> mirror_{false};
    std::atomic<float> zoomScale_{1.0f};   // ★ UVC 数字变焦倍数
    std::vector<uint8_t> tempBuf_;

    // ★ 异步推理线程
    std::thread inferThread_;
    std::mutex inferMutex_;
    std::condition_variable inferCV_;
    std::vector<uint8_t> inferFrame_;
    int inferW_ = 0;
    int inferH_ = 0;
    bool inferPending_ = false;
    std::atomic<bool> inferRunning_{false};

    // ★ 复用 buffer，避免每帧 malloc
    std::vector<uint8_t> scaledRgbaBuf_;
    std::vector<uint8_t> blackBuf_;

    // ★ 录像直供窗口（AVRecorder getInputSurface 的 NV12 消费者 surface）
    OHNativeWindow *recordWindow_ = nullptr;
    std::mutex recordWindowMutex_;
    std::vector<uint8_t> recordScaledBuf_;
    // ★ 录像 R/B 通道修正中转（rgbaBuf_ 是 mem RGBA，ARGBToNV12 要求 mem BGRA）
    std::vector<uint8_t> recordBgraBuf_;
    // ★ 推流诊断计数
    int recordPushTotal_ = 0;
    int recordPushOk_ = 0;
    int recordFailLogged_ = 0;

    // ★ 位置水印：ArkTS 逆地理解析的地理位置名（图片/录像底部居中）
    std::mutex locationMutex_;
    std::string locationText_;
    // ★ 水印条的纵向位置（相对帧高 0.4~1.0）：预览底部被底部面板遮挡时，
    //   ArkTS 按实际可见区域下移比例，保证画面上看得见；1.0=帧最底部
    float locBottomRatio_ = 1.0f;

    // ★ 干净帧快照（表格识别）：管线线程在 ProcessCommonTail 叠加绘制前抓帧
    std::atomic<bool> cleanSnapRequested_{false};
    std::mutex cleanSnapMutex_;
    std::condition_variable cleanSnapCv_;
    std::vector<uint8_t> cleanSnapBuf_;
    bool cleanSnapReady_ = false;   // cleanSnapMutex_ 保护

    // ★ AI 画质增强异步工作线程（mode 2，v3 细节层解耦架构）：
    //   相机线程每帧照常渲染基底（全帧率流畅），SR 工作线程只异步产出
    //   三张"修复层"快照（全部显示分辨率、int8、3 通道、clamp ±96）：
    //   ① 细节层 delta = SR−双线性（强度滑条 0~220%、±64 软限幅）
    //   ② 降噪层 = SR 引导联合双边滤波平滑 − 原帧（负向噪声分量）
    //   ③ 去浊层 = 暗通道先验去雾 + 灰世界白平衡（水下去浊+色彩还原，低频）
    //   相机帧取最新层按龄衰减合成（max(500,2×推理周期) 内全强度，之后 400ms 淡出）。
    //   帧率与推理速度彻底解耦（v2"推理期间丢帧"的跳帧/闪烁根因），
    //   各层新旧 65/35 时域混合，因子自适应带滞回防震荡
    void SrWorkerLoop();
    void CompositeAiLayers(uint8_t *rgba, int W, int H);
    std::thread srThread_;
    std::mutex srMutex_;
    std::condition_variable srCv_;
    std::atomic<bool> srRunning_{false};
    std::atomic<bool> srBusy_{false};      // 入队后置 true，工作线程产出修复层后置 false
    bool srPending_ = false;               // srMutex_ 保护
    std::vector<uint8_t> srInPending_;     // 相机线程 → 工作线程（缩放后小图）
    std::vector<uint8_t> srWorkIn_;        // 工作线程私有
    int srInW_ = 0, srInH_ = 0;            // pending 输入尺寸
    std::vector<uint8_t> srOutBuf_;        // SR 输出（inW*scale x inH*scale）
    std::vector<uint8_t> srUpBuf_;         // 低分辨率网格的双线性放大（与 SR 输出同尺寸，细节增量基线）
    std::vector<uint8_t> srDetailBuf_;     // SR 输出缩放到显示尺寸
    std::vector<uint8_t> srUpScaledBuf_;   // 双线性基线缩放到显示尺寸
    std::vector<uint8_t> srGuideBuf_;      // SR 输出降回网格尺寸（降噪联合双边滤波的"无噪引导图"）
    std::vector<uint8_t> srPackBuf_;       // 降噪层打包缓冲（网格尺寸，delta+128 塞进 RGBA 经 libyuv 放大）
    std::vector<uint8_t> srTinyBuf_;       // 去浊估计微缩图（长边 128）
    std::vector<uint8_t> srLayerTmp_;      // 修复层放大临时（显示尺寸 RGBA）
    // ★ 修复层快照（srDeltaMutex_ 保护；SR 工作线程写，相机线程读合成）
    std::mutex srDeltaMutex_;
    std::vector<int8_t> srDeltaBuf_;       // ① 细节增量层（显示尺寸，clamp ±96）
    std::vector<int8_t> srNoiseBuf_;       // ② 降噪层（网格平滑−原帧，显示尺寸，clamp ±96）
    std::vector<int8_t> srClarityBuf_;     // ③ 去浊+色彩还原层（低频校正，显示尺寸，clamp ±96）
    int srDeltaW_ = 0, srDeltaH_ = 0;
    int srDeltaCycleMs_ = 0;               // 最近一次 SR 推理耗时（龄衰减窗口参考）
    std::chrono::steady_clock::time_point srDeltaTime_{};   // 修复层产出时刻
    std::atomic<bool> srDeltaValid_{false};

    // ★ 人脸识别异步工作线程（对齐 SrWorker 模式：相机线程只拷贝入队，
    //   SCRFD + 106 点 + 性别年龄推理在工作线程执行，结果快照供绘制与 napi HUD）
    void FaceWorkerLoop();
    void DrawFaceOverlay();
    std::thread faceThread_;
    std::mutex faceMutex_;
    std::condition_variable faceCv_;
    std::atomic<bool> faceRunning_{false};
    std::atomic<bool> faceBusy_{false};    // 入队后置 true，工作线程处理完置 false
    bool facePending_ = false;             // faceMutex_ 保护
    std::vector<uint8_t> faceInPending_;   // 相机线程 → 工作线程
    int faceInW_ = 0, faceInH_ = 0;
    std::vector<uint8_t> faceWork_;        // 工作线程私有
    std::vector<float> faceOutBuf_;        // 工作线程私有（face_detect_rgba 输出，每脸 223 float）
    // 结果快照（faceInfoMutex_ 保护）
    std::mutex faceInfoMutex_;
    std::vector<FaceDrawInfo> faceResults_;
    int faceResW_ = 0, faceResH_ = 0;
};

#endif