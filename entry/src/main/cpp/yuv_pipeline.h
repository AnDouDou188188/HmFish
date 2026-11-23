// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ YuvPipeline —— HmFish 渲染管线核心类声明（yuv_pipeline.h）                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【文件职责】渲染管线核心头文件：YuvPipeline 类——帧处理/推理/叠加/推流/快照/  ║
// ║   变焦/位置水印/录像直供/SR 工作线程的全部成员与接口声明；实现在               ║
// ║   yuv_pipeline.cpp。napi_init.cpp 持其为全局单例并桥接 ArkTS 调用，         ║
// ║   uvc_camera.cpp 在 USB 帧回调解码后反调 ProcessExternalRgba 供帧，          ║
// ║   本头文件是三方共用的接口契约。                                              ║
// ║                                                                              ║
// ║ 【核心数据流】两条帧入口，一个汇合点：                                        ║
// ║   A. 手机相机：CameraService → ImageReceiver（CreateReceiver 创建的          ║
// ║      接收 surface）→ OnImageArrival → ProcessFrame（NV21→RGBA→旋转镜像      ║
// ║      →变焦→增强）→ ProcessCommonTail                                        ║
// ║   B. USB/UVC：libuvc 回调解码出 RGBA → ProcessExternalRgba（变焦→增强）     ║
// ║      → ProcessCommonTail                                                    ║
// ║   ProcessCommonTail 为公共尾部（识别触发→叠加绘制→预览/录像推流），保证      ║
// ║   手机/USB 帧源行为完全一致；最终显示帧固定存放在 rgbaBuf_                    ║
// ║   （displayWidth_ x displayHeight_，内存序 RGBA）。                          ║
// ║                                                                              ║
// ║ 【线程模型】渲染回调线程 + 三个后台工作线程，全部忙时跳过（丢帧不积压）：     ║
// ║   ① 相机/渲染线程：OnImageArrival / ProcessExternalRgba（事件驱动）         ║
// ║   ② InferenceLoop：YOLO 异步推理（inferCV_ 条件变量唤醒）                   ║
// ║   ③ SrWorkerLoop：AI 超分/降噪/去浊只产出"修复层"快照（不接管渲染）         ║
// ║   ④ FaceWorkerLoop：SCRFD 人脸检测（人脸启用时才入队）                       ║
// ║   跨线程交接一律"互斥锁 + 快照拷贝"，不长时间锁定共享缓冲；模型推理在         ║
// ║   libocr.so 自有 worker 中进行（经 OcrBridge.h 桥接）。                      ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef YUV_PIPELINE_H
#define YUV_PIPELINE_H

// ───────── C++ 标准库：原子/互斥/条件变量/线程构成多线程基础设施 ─────────
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <thread>                  // ★ 新增
#include <condition_variable>      // ★ 新增

// ───────── 鸿蒙原生接口：ImageReceiver 接收相机 NV21 帧，OHNativeWindow 推预览/录像 surface ─────────
#include <multimedia/image_framework/image/image_receiver_native.h>
#include <multimedia/image_framework/image/image_native.h>
#include <native_window/external_window.h>

// ───────── FreeType2：中文水印/检测标签文字渲染 ─────────
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

// YOLO 检测器封装（ncnn），napi 层的 initYolo/detect 也会直接使用
#include "yolo_detector.h"

// ★ DetectItem：单条 YOLO 检测结果（图像坐标、像素单位）：
//   x,y,w,h 为框中心与边长，score 为置信度，label 为类别索引（配合 classNames_ 取类名）
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

// ★ YuvPipeline：渲染管线核心类——相机帧进入后的全部"看"（转换/增强/叠加/推流）
//   与"算"（YOLO/人脸/SR/OCR 触发）都在此类完成。每次相机会话一个实例，
//   切换前后摄像头/USB 重连/退后台时由 napi 层销毁重建，因此外部设置
//   （水印/变焦/YOLO 开关/字体等）需在 napi 层做会话级缓存，重建后立即回灌
class YuvPipeline {
public:
    // ───────── 生命周期 ─────────

    // ★ YuvPipeline：默认构造；重资源（字体/模型/线程）在各启动接口内懒创建
    YuvPipeline() = default;
    // ★ ~YuvPipeline：析构须保证 Stop（join 全部工作线程、释放 FreeType/窗口引用），
    //   依赖 Stop 的幂等设计防止资源二次释放
    ~YuvPipeline();

    // ★ CreateReceiver：创建 ImageReceiver（手机相机帧入口），并把接收 surfaceId
    //   写入 outSurfaceId 交给 ArkTS 配置相机输出，相机帧随后经 OnImageArrival 持续到达。
    //   width/height 为占位尺寸（相机档位在 init 后才确定，后续 UpdateFrameSize 校正）
    bool CreateReceiver(int width, int height, std::string &outSurfaceId);
    // ★ Start：启动渲染主链（订阅 ImageReceiver 新帧回调）及全部工作线程
    void Start();
    // ★ Stop：停帧回调、唤醒并 join 全部工作线程、断开窗口引用；幂等，析构前必须调用
    void Stop();

    // ★ 手机相机路径：createYuvReceiver 先用占位尺寸（相机档位在 init 后才确定），
    //   init 完成后按实际选中的预览档位更新管线处理尺寸——保留旋转/镜像，
    //   重分配转换缓冲并同步预览 surface geometry，否则按 640x480 处理 1440x1440
    //   帧导致 UV 平面偏移错位（颜色错乱、录像内容错乱）
    bool UpdateFrameSize(int width, int height);

    // ★ UVC（USB 摄像头）外部供帧模式：无 ImageReceiver，
    // 由 uvc_camera 帧回调直接注入 RGBA
    bool ConfigureExternal(int width, int height);
    // ★ StartExternal：外部供帧模式下启动工作线程与渲染状态（帧驱动，无需订阅相机回调）
    void StartExternal();
    // ★ ProcessExternalRgba：USB 帧入口——接收解码后的 RGBA，内部完成变焦→SR 投喂
    //   →基底渲染→增强→公共尾部；由 uvc_camera.cpp 逐帧回调，必须非阻塞（忙时跳过）
    void ProcessExternalRgba(const uint8_t *rgba, int w, int h);

    // ───────── 识别结果与帧快照 ─────────

    // ★ GetLatestResults：返回最近一帧 YOLO 检测结果快照（resultsMutex_ 保护下拷贝，
    //   供绘制/napi 层查询，结果为空时返回空数组）
    std::vector<DetectItem> GetLatestResults();
    // ★ 懒快照：拍照用，直接拷贝当前帧（含叠加层：框/水印/OCR 框）
    std::vector<uint8_t> GetLatestRgba();
    // ★ GetFrameSize：返回最终显示帧尺寸（rgbaBuf_ 的 displayWidth_×displayHeight_），
    //   ArkTS 拍照编码/画布绘制依赖此值
    void GetFrameSize(int &w, int &h);
    // ★ GetInferCount：YOLO 累计推理次数（原子计数，HUD 统计用）
    uint64_t GetInferCount() const { return inferCount_.load(); }

    // ★ 干净帧快照（表格识别专用）：GetLatestRgba 含自绘叠加层
    //   （框/水印/OCR 框），识别会把水印当成文字。本机制请求管线在"叠加绘制前"
    //   的干净帧点抓一帧，条件变量同步等待（最长 timeoutMs）
    void RequestCleanSnapshot();
    // ★ WaitCleanSnapshot：阻塞等待干净帧——业务线程先标记请求，管线线程在
    //   下一帧 ProcessCommonTail 机会点抓帧并条件变量通知；超时返回 false
    bool WaitCleanSnapshot(int timeoutMs, std::vector<uint8_t> &out, int &w, int &h);

    // ───────── 显示窗口与绘制设置 ─────────

    // ★ SetPreviewWindow：绑定 XComponent 预览 surface（napi 层用 surfaceId 创建的
    //   native window），绑定后渲染帧直接上屏，不再经 JS 层中转
    void SetPreviewWindow(OHNativeWindow *win);
    // ★ SetYoloEnabled：YOLO 识别总开关（false 时推理线程不再入队，节省算力）
    void SetYoloEnabled(bool enabled);

    // ★ LoadFont：加载中文字幕/水印渲染所需的 TTF 字体（FreeType），每次实例化后设置一次
    void LoadFont(const std::string &fontPath);
    // ★ SetClassNames：YOLO 类别名列表（与模型输出 label 索引对齐，绘制框标签用）
    void SetClassNames(const std::vector<std::string> &names);

    // ★ SetRotation：设置相机传感器旋转补偿角（0/90/180/270），在 NV21→RGBA 转换时
    //   应用；90/270 时宽高互换（横置传感器竖屏显示）
    void SetRotation(int degrees);
    // ★ SetMirror：水平镜像开关（前置摄像头自拍镜像预览），旋转之后应用
    void SetMirror(bool mirror);
    // ★ USB 摄像头数字变焦（照抄 Android UVCPreview nativeSetZoomScale）：
    //   中心裁剪 1/zoom 后放大回原尺寸，clamp 1.0~5.0
    void SetZoomScale(float scale);
    // ★ UpdateDisplaySize：按当前旋转/变焦状态重算显示帧尺寸并同步预览 surface geometry
    void UpdateDisplaySize();
    // ★ ClearSurface：清空预览 surface 内容（填黑），防止切换摄像头/停管线后残留上一帧
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
    // ★ DetachRecordSurface：解绑录像 surface（录像停止时调用），此后管线不再向录像窗口供帧
    void DetachRecordSurface();

private:
    // ───────── 帧处理主链（相机/渲染线程执行） ─────────

    // ★ OnImageArrival：ImageReceiver 新帧回调入口——只取最新一帧（丢弃积压旧帧），
    //   交 ProcessFrame 处理；由相机驱动事件驱动，必须快速返回
    void OnImageArrival();
    // ★ ProcessFrame：单帧手机相机处理全链：NV21→RGBA→旋转→镜像→变焦→增强，
    //   结果写入 rgbaBuf_，随后进入公共尾部
    void ProcessFrame(OH_ImageNative *image);
    void ProcessCommonTail();   // OCR 触发→推理→叠加→上屏 的公共尾部
    // ★ StoreResults：YOLO 推理结果写回（resultsMutex_ 保护拷贝），同时刷新
    //   lastNonEmptyResultsTime_（供"目标丢失"降速与 HUD 提示判断）
    void StoreResults(const std::vector<DetectItem> &items);
    // ★ DrawOverlayOnRgba：在 rgbaBuf_ 上绘制 YOLO 检测框与类别标签（FreeType 中文渲染）
    void DrawOverlayOnRgba();
    // ★ DrawOcrOverlay：绘制 OCR 文字检测框（最近一次识别结果，青色折线）
    void DrawOcrOverlay();
    // ★ PushToSurface：把当前 rgbaBuf_ 帧推送到预览窗口（内部处理缩放与格式转换）
    bool PushToSurface();
    // ★ PushPixelsToSurface：把任意 RGBA 缓冲推送到预览窗口（变焦/清屏黑帧路径共用）
    bool PushPixelsToSurface(const uint8_t *src, int srcW, int srcH);
    // ★ PushToRecordSurface：把当前帧推送到录像窗口（RGBA→NV12 转换并缩放至录像分辨率）
    bool PushToRecordSurface(const uint8_t *src, int srcW, int srcH);

    // ───────── 中文文字渲染（FreeType） ─────────

    // ★ drawTextCN：把中文/混合文本按字形逐个渲染到 RGBA 缓冲（标签与水印共用）
    void drawTextCN(uint8_t *rgba, int W, int H,
                    const std::string &text, int x, int y,
                    int fontHeight, uint8_t r, uint8_t g, uint8_t b);
    // ★ measureTextCN：测量文本渲染宽度（像素，用于居中/右对齐排版）
    int  measureTextCN(const std::string &text, int fontHeight);

    // ───────── 异步推理（YOLO 工作线程） ─────────

    // ★ 新增：异步推理
    // ★ InferenceLoop：YOLO 推理线程主循环——inferCV_ 条件变量休眠，新帧入队被唤醒；
    //   推理前把帧拷贝到私有缓冲，避免与相机线程争抢
    void InferenceLoop();
    // ★ RequestInference：相机线程把当前帧交给推理线程（快照拷贝 + 条件变量唤醒）；
    //   inferPending_ 为真时直接丢弃本帧（忙时跳过：上一帧没推理完就不排队）
    void RequestInference(const uint8_t *rgba, int w, int h);

    // ───────── 相机接收与渲染状态 ─────────

    // ★ 手机相机帧接收器（UVC 外部供帧模式下为空）
    OH_ImageReceiverNative *receiver_ = nullptr;
    // ★ 相机原始输出尺寸（旋转前；rotation 90/270 时 width_/height_ 互换）
    int width_ = 640;
    int height_ = 480;
    // ★ 最终显示帧缓冲（displayWidth_×displayHeight_，内存序 RGBA）：
    //   叠加绘制/录像推流/懒快照都在它上面进行
    std::vector<uint8_t> rgbaBuf_;
    // ★ 管线运行标志：Start 置位、Stop 复位，控制全部线程循环退出
    bool running_ = false;

    // ★ ImageReceiver 回调订阅/退订互斥（防止 Stop 与相机回调线程竞争）
    std::mutex callbackMutex_;

    // ★ 最近一帧 YOLO 检测结果快照及时间戳（绘制线程与 napi 读取）
    std::mutex resultsMutex_;
    std::vector<DetectItem> latestResults_;
    std::chrono::steady_clock::time_point lastNonEmptyResultsTime_{};   // ★ 新增
    // ★ YOLO 推理累计计数（原子，HUD 统计）
    std::atomic<uint64_t> inferCount_{0};
    // ★ 上次推理触发时刻（配合节奏参数控制 YOLO 有目标/空闲两档间隔）
    std::chrono::steady_clock::time_point lastInferTime_;
    std::chrono::steady_clock::time_point lastRenderTime_;   // ★ 帧率节流：按 setPipelineTiming 的最小渲染间隔丢帧

    // ───────── 预览窗口 ─────────

    // ★ XComponent 预览 surface（napi 层 attachPreviewSurface 创建）
    OHNativeWindow *previewWindow_ = nullptr;
    // ★ 保护 previewWindow_：绑定/解绑与渲染线程并发访问
    std::mutex previewWindowMutex_;
    std::atomic<bool> yoloEnabled_{true};

    // ───────── FreeType 字体 ─────────

    // ★ FreeType 库与字体-face（中文标签/水印渲染），ftMutex_ 串行化并发渲染调用
    FT_Library ftLib_ = nullptr;
    FT_Face    ftFace_ = nullptr;
    std::mutex ftMutex_;

    // ★ YOLO 类别名（label 索引对齐）
    std::vector<std::string> classNames_;
    std::mutex classNamesMutex_;

    // ───────── 旋转/镜像/变焦与显示尺寸 ─────────

    // ★ 最终显示帧尺寸与旋转镜像变焦状态：
    //   displayWidth_/Height_ 即 rgbaBuf_ 尺寸，rotationDegrees_ 90/270 时与原始宽高互换
    int displayWidth_ = 0;
    int displayHeight_ = 0;
    int rotationDegrees_ = 0;
    std::atomic<bool> mirror_{false};
    std::atomic<float> zoomScale_{1.0f};   // ★ UVC 数字变焦倍数
    // ★ 旋转/镜像/变焦转换的中转缓冲（复用，避免每帧 malloc）
    std::vector<uint8_t> tempBuf_;

    // ───────── YOLO 推理线程状态 ─────────

    // ★ 异步推理线程：入队帧 + 条件变量唤醒 + 忙时跳过（inferPending_）
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

    // ───────── 录像直供窗口 ─────────

    // ★ 录像直供窗口（AVRecorder getInputSurface 的 NV12 消费者 surface）
    OHNativeWindow *recordWindow_ = nullptr;
    // ★ 保护 recordWindow_（绑定/解绑与渲染线程并发）及各转换缓冲
    std::mutex recordWindowMutex_;
    std::vector<uint8_t> recordScaledBuf_;
    // ★ 录像 R/B 通道修正中转（rgbaBuf_ 是 mem RGBA，ARGBToNV12 要求 mem BGRA）
    std::vector<uint8_t> recordBgraBuf_;
    // ★ 推流诊断计数（推流总数/成功数/失败日志限次，用于排查"录像文件无画面"）
    int recordPushTotal_ = 0;
    int recordPushOk_ = 0;
    int recordFailLogged_ = 0;

    // ───────── 位置水印 ─────────

    // ★ 位置水印：ArkTS 逆地理解析的地理位置名（图片/录像底部居中）；
    //   locBottomRatio_ 为水印条纵向位置（相对帧高 0.4~1.0），预览底部被面板遮挡时
    //   由 ArkTS 按可见区域调整，保证画面上看得见
    std::mutex locationMutex_;
    std::string locationText_;
    float locBottomRatio_ = 1.0f;

    // ───────── 干净帧快照（表格识别） ─────────

    // ★ 干净帧快照（表格识别）：管线线程在 ProcessCommonTail 叠加绘制前抓帧
    std::atomic<bool> cleanSnapRequested_{false};
    // ★ 快照交接：cleanSnapMutex_/cleanSnapCv_ 同步"请求→抓帧→通知"，超时由调用方处理
    std::mutex cleanSnapMutex_;
    std::condition_variable cleanSnapCv_;
    std::vector<uint8_t> cleanSnapBuf_;
    bool cleanSnapReady_ = false;   // cleanSnapMutex_ 保护

    // ───────── AI 画质增强（SR 工作线程，v3 细节层解耦架构） ─────────

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
    // ★ OpenCV5 深度优化版帧级画质增强（全部相机路径每帧调用）：
    //   cv::blur（KleidiCV/carotene NEON HAL + parallel_for_ 线程池）产出
    //   3x3/5x5 盒滤波后，单趟 parallel_for_ 合并全部逐像素处理——
    //   AI 修复层合成 + 细锐 + Clarity + 亮度/对比度/饱和度，
    //   替代旧链路每帧 7~11 趟全帧标量循环
    void ApplyFrameEnhance(uint8_t *rgba, int W, int H);
    // ★ SR 工作线程的启停/交接/缓冲集（含义与推理线程同构）：
    //   srBusy_ 入队置真、产出后置假；srInPending_ 为相机线程缩放后的小图
    std::thread srThread_;
    std::mutex srMutex_;
    std::condition_variable srCv_;
    std::atomic<bool> srRunning_{false};
    std::atomic<bool> srBusy_{false};      // 入队后置 true，工作线程产出修复层后置 false
    bool srPending_ = false;               // srMutex_ 保护
    std::vector<uint8_t> srInPending_;     // 相机线程 → 工作线程（缩放后小图）
    std::vector<uint8_t> srWorkIn_;        // 工作线程私有
    int srInW_ = 0, srInH_ = 0;            // pending 输入尺寸
    // ★ SR 工作线程的各级中间缓冲（放大/缩放/打包/微缩，命名与管线算法一一对应）
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

    // ───────── 人脸识别（face 工作线程） ─────────

    // ★ 人脸识别异步工作线程（对齐 SrWorker 模式：相机线程只拷贝入队，
    //   SCRFD + 106 点 + 性别年龄推理在工作线程执行，结果快照供绘制与 napi HUD）
    void FaceWorkerLoop();
    // ★ DrawFaceOverlay：在 rgbaBuf_ 上绘制人脸 106 点连线框（用快照结果，过期自动跳过）
    void DrawFaceOverlay();
    // ★ face 工作线程的启停/交接/缓冲集（结构与推理线程、SR 线程同构）
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