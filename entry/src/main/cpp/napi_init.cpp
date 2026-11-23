// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ napi_init.cpp —— ArkTS ↔ C++ NAPI 桥接层（entry 模块 native 实现）          ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【文件职责】ArkTS ↔ native(libentry.so) 全部导出函数注册与参数转换，          ║
// ║   含管线生命周期、设置下发、拍照/录像、UVC 连接取流等入口。                    ║
// ║   所有 JS 可调用的 native 能力都在 Init() 的 napi_property_descriptor 表中     ║
// ║   声明并挂载到 exports 对象，ArkTS 侧 import 'libentry.so' 后直接调用。        ║
// ║                                                                              ║
// ║ 【核心数据流】ArkTS（页面/UI）→ NAPI 参数解包 → 全局单例（gPipeline 管线/    ║
// ║   gDetector 检测器/ocr_bridge→libocr.so/UvcCamera）→ 各业务实现。             ║
// ║   耗时操作（模型加载/引擎释放/表格识别）一律走 napi_async_work + Promise，     ║
// ║   避免主线程卡顿触发 appfreeze。                                              ║
// ║                                                                              ║
// ║ 【线程安全】gPipelineMutex/gDetectorMutex 串行化全部单例访问；设置类状态       ║
// ║   （水印/变焦/YOLO 开关/字体/类名）额外维护"会话级缓存"——管线销毁重建         ║
// ║   （切换前后摄像头/USB 重连/退后台）后立即回灌，ArkTS 无需重复下发。           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include <napi/native_api.h>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <hilog/log.h>
#include <native_window/external_window.h>
// ★ 旧照片 AI 修复用（bilateralFilter 去噪 / cvtColor）：OpenCV 头为全局
//   include 目录（见 CMakeLists include_directories），entry 链接 imgproc+core 静态库
#include "opencv2/core.hpp"
#include "opencv2/imgproc.hpp"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NAPI"

#include "yolo_detector.h"
#include "yuv_pipeline.h"
#include "OcrBridge.h"
#include "uvc_camera.h"

// ───────── 全局单例与会话级状态 ─────────
// ★ gDetector：YOLO 检测器全局单例（initYolo/detect 与管线内部同步检测
//   NativeDetectOnRgba 共用），全部访问经 gDetectorMutex 串行
static std::unique_ptr<YoloDetector> gDetector;
static std::mutex gDetectorMutex;
// ★ gConfThresh：YOLO 置信度阈值（setConfThresh 入口），同步检测路径共用
static float gConfThresh = 0.25f;
// ★ gPipeline：渲染管线全局单例（相机会话级，切换/重连时销毁重建）
static std::unique_ptr<YuvPipeline> gPipeline;
static std::mutex gPipelineMutex;

// ★ 字体/类名会话级缓存：管线实例销毁重建后这两项设置随实例丢失，
//   而 ArkTS 不会重新下发 → 全局存一份，重建后立即回灌
static std::string gPendingFontPath;
static std::vector<std::string> gPendingClassNames;
// ★ 位置水印全局缓存：管线切摄像头/退后台重连会销毁重建（locationText_/locBottomRatio_ 随实例丢失），
//   ArkTS 最长 180s 才重新推送 → 全局存一份，重建后立即回灌
static std::string gPendingLocationText;
static float gPendingLocBottomRatio = 1.0f;

// YOLO 开关的会话级状态：pipeline 重建（切换内外相机/USB 重连）后需重新应用到新实例，
// 否则新实例默认 yoloEnabled_=true，会出现"已关闭识别仍在识别"
static std::atomic<bool> gYoloOn{true};

// USB 摄像头数字变焦的会话级状态：管线重建（USB 重连）后需重新应用到新实例
static std::atomic<float> gUvcZoom{1.0f};

// ★ extern "C" 跨文件函数声明：实现在 yuv_pipeline.cpp，用 C 链接避免 C++ 名字修饰，
//   此处直接声明即可调用（无需额外头文件）；涵盖管线内部开关/参数（识别框、画质增强、
//   人脸、运动、水深）与能耗节奏参数的 setter/getter
extern "C" {
    void SetYoloEnabled(bool enabled);
    // ★ 识别框/标签画面显示开关（yuv_pipeline.cpp 定义）
    void SetDrawBoxesEnabled(bool enabled);
    // ★ USB 画质增强（yuv_pipeline.cpp 定义）：mode 0关/1非AI/2 AI增强
    //   （超分+降噪+去浊+色彩还原）
    void SetUsbEnhance(int mode, int brightness, int sharpness,
                       int contrast, int saturation, int strength,
                       int denoise, int clarity, int color);
    // ★ 人脸识别画面开关（yuv_pipeline.cpp 定义）
    void SetFaceEnabled(bool enabled);
    void SetMotionEnabled(bool enabled);
    float GetMotionLevel();
    void SetDepthEnabled(bool enabled);
    float GetDepthCm();
    void SetDepthParams(float weightG, int turbidity);
    // ★ 能耗模式：节奏参数（yuv_pipeline.cpp 定义）
    void SetPipelineTiming(int renderMs, int yoloActiveMs, int yoloIdleMs, int sensorMs);
}

// ★ NativeDetectOnRgba：同步 YOLO 检测入口（非 NAPI，供 yuv_pipeline.cpp 的
//   推理线程回调）：try_lock 保证渲染线程永不阻塞——检测器被换（initYolo/
//   releaseYolo 持锁中）时直接返回 false 跳过本帧；置信度取 gConfThresh，NMS 固定 0.25
bool NativeDetectOnRgba(const uint8_t *rgba, int w, int h, std::vector<Detection> &out) {
    std::unique_lock<std::mutex> lock(gDetectorMutex, std::try_to_lock);
    if (!lock.owns_lock() || !gDetector) return false;
    out = gDetector->Detect(rgba, w, h, gConfThresh, 0.25f);
    return true;
}

// ═══════════ YOLO ═══════════
// ★ InitYolo（ArkTS 调用名：initYolo）：加载 YOLO 鱼类识别模型（ncnn）。
//   参数：args[0]=param 文件路径，args[1]=bin 文件路径，args[2]=模型输入边长（默认 640）。
//   要点：先同步释放旧检测器（防止双模型占内存）、持锁重建；
//   返回值：boolean 是否加载成功
// ★ 修改：接收 paramPath, binPath, inputSize 三个参数
static napi_value InitYolo(napi_env env, napi_callback_info info) {
    size_t argc = 3; 
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char paramPath[1024] = {0};
    char binPath[1024] = {0};
    size_t len = 0;
    
    napi_get_value_string_utf8(env, args[0], paramPath, sizeof(paramPath), &len);
    napi_get_value_string_utf8(env, args[1], binPath, sizeof(binPath), &len);
    
    int32_t size = 640;
    if (argc > 2) napi_get_value_int32(env, args[2], &size);

    std::lock_guard<std::mutex> lock(gDetectorMutex);
    if (gDetector) { gDetector->Release(); gDetector.reset(); }
    
    gDetector = std::make_unique<YoloDetector>();
    bool ok = gDetector->InitFromFile(std::string(paramPath), std::string(binPath), size);
    
    napi_value r; 
    napi_get_boolean(env, ok, &r); 
    return r;
}

// ★ Detect（ArkTS 调用名：detect）：一次性同步 YOLO 检测（调试/演示路径，
//   正式识别走管线内部异步线程）。参数：args[0]=RGBA ArrayBuffer，args[1]=宽，
//   args[2]=高，args[3]=置信度阈值。要点：try_lock 防阻塞（模型被换时返回空数组）；
//   返回值：二维数组，每项 [x,y,w,h,score,label]（x,y 为框中心，图像坐标）
static napi_value Detect(napi_env env, napi_callback_info info) {
    napi_value empty; napi_create_array(env, &empty);
    size_t argc = 4; napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    void *data = nullptr; size_t byteLen = 0;
    napi_get_arraybuffer_info(env, args[0], &data, &byteLen);
    int32_t w = 0, h = 0;
    napi_get_value_int32(env, args[1], &w);
    napi_get_value_int32(env, args[2], &h);
    double th = 0.25;
    napi_get_value_double(env, args[3], &th);
    std::vector<Detection> dets;
    {
        std::unique_lock<std::mutex> lock(gDetectorMutex, std::try_to_lock);
        if (!lock.owns_lock() || !gDetector) return empty;
        dets = gDetector->Detect(static_cast<const uint8_t *>(data), w, h, (float)th, 0.45f);
    }
    napi_value result; napi_create_array(env, &result);
    for (size_t i = 0; i < dets.size(); ++i) {
        napi_value arr; napi_create_array_with_length(env, 6, &arr);
        float v[6] = {dets[i].x, dets[i].y, dets[i].w, dets[i].h, dets[i].score, (float)dets[i].label};
        for (int j = 0; j < 6; ++j) { napi_value num; napi_create_double(env, v[j], &num); napi_set_element(env, arr, j, num); }
        napi_set_element(env, result, i, arr);
    }
    return result;
}

// ★ ReleaseYolo（ArkTS 调用名：releaseYolo）：释放 YOLO 检测器，
//   释放 ncnn 网络/缓冲内存；无参数无返回值，可安全重复调用
static napi_value ReleaseYolo(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(gDetectorMutex);
    if (gDetector) { gDetector->Release(); gDetector.reset(); }
    napi_value u; napi_get_undefined(env, &u); return u;
}

// ═══════════ YuvPipeline (保持原样) ═══════════
// ★ CreateYuvReceiver（ArkTS 调用名：createYuvReceiver）：创建手机相机路径渲染管线。
//   参数：args[0]/args[1]=占位宽/高（相机档位 init 后才确定，之后 updateFrameSize 校正）。
//   要点：先停旧管线再重建 → 按序回灌会话级状态（YOLO 开关/位置水印/字体/类名）→
//   CreateReceiver 取接收 surfaceId；失败返回 undefined，成功返回 surfaceId 字符串
//   （ArkTS 用它配置相机输出）。管线"销毁重建"是切换前后摄像头/退后台重连的统一入口
static napi_value CreateYuvReceiver(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t w = 640, h = 480;
    if (argc > 0) napi_get_value_int32(env, args[0], &w);
    if (argc > 1) napi_get_value_int32(env, args[1], &h);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) { gPipeline->Stop(); gPipeline.reset(); }
    gPipeline = std::make_unique<YuvPipeline>();
    gPipeline->SetYoloEnabled(gYoloOn.load());
    // ★ 重建管线回灌位置水印（否则切摄像头后最长 180s 画面无水印）
    gPipeline->SetLocationText(gPendingLocationText);
    gPipeline->SetLocationBottomRatio(gPendingLocBottomRatio);
    std::string sid;
    if (!gPipeline->CreateReceiver(w, h, sid)) {
        napi_value u; napi_get_undefined(env, &u); return u;
    }

    if (!gPendingFontPath.empty()) {
        gPipeline->LoadFont(gPendingFontPath);
    }
    if (!gPendingClassNames.empty()) {
        gPipeline->SetClassNames(gPendingClassNames);
    }

    napi_value r; napi_create_string_utf8(env, sid.c_str(), sid.size(), &r); return r;
}

// ★ StartPipeline（ArkTS 调用名：startPipeline）：启动管线（订阅相机帧回调、
//   拉起全部工作线程）。要点：启动前再补灌一次字体/类名（兼容 ArkTS 先 setFontPath
//   后 startPipeline 的时序），幂等可重复调用
static napi_value StartPipeline(napi_env env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) {
        if (!gPendingFontPath.empty()) gPipeline->LoadFont(gPendingFontPath);
        if (!gPendingClassNames.empty()) gPipeline->SetClassNames(gPendingClassNames);
        gPipeline->Start();
    }
    return nullptr;
}

// ★ 手机相机路径：createYuvReceiver 用占位尺寸创建，camera init 选定实际
//   预览档位后调用本接口把管线处理尺寸对齐到真实档位（保留旋转/镜像）
// （ArkTS 调用名：updateFrameSize）参数：args[0]/args[1]=实际预览档位宽/高；返回值：boolean
static napi_value UpdateFrameSizeNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t w = 0, h = 0;
    if (argc > 0) napi_get_value_int32(env, args[0], &w);
    if (argc > 1) napi_get_value_int32(env, args[1], &h);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    bool ok = false;
    if (gPipeline) ok = gPipeline->UpdateFrameSize(w, h);
    napi_value r; napi_get_boolean(env, ok, &r); return r;
}

// ★ StopPipeline（ArkTS 调用名：stopPipeline）：停止并销毁渲染管线。
//   要点：必须先停 UVC 取流并断开（UVC 帧回调持有管线裸指针，先停 UVC 防止
//   回调访问悬垂指针），再 join 管线各线程后销毁。相机页面销毁/退后台的统一出口
static napi_value StopPipeline(napi_env env, napi_callback_info) {
    // 若 UVC 尚在推流，先停 UVC 再销毁管线，避免帧回调访问悬垂指针
    UvcCamera::Instance().Stop();
    UvcCamera::Instance().Disconnect();
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) { gPipeline->Stop(); gPipeline.reset(); }
    return nullptr;
}

// ═══════════ UVC（USB 摄像头，fd 注入） ═══════════
// ★ SetupExternalPipeline（ArkTS 调用名：setupExternalPipeline）：创建 USB 摄像头
//   路径渲染管线（无 ImageReceiver，uvc_camera 帧回调解码出 RGBA 反调 ProcessExternalRgba 供帧）。
//   参数：args[0]/args[1]=相机协商出的取流宽/高。
//   要点：先停旧 UVC 与旧管线 → 重建 → 回灌会话级状态（YOLO 开关/变焦/位置水印/
//   字体/类名）→ ConfigureExternal；成功直接 StartExternal，失败销毁新管线。
//   返回值：boolean 是否创建成功
static napi_value SetupExternalPipeline(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t w = 640, h = 480;
    if (argc > 0) napi_get_value_int32(env, args[0], &w);
    if (argc > 1) napi_get_value_int32(env, args[1], &h);

    UvcCamera::Instance().Stop();
    UvcCamera::Instance().Disconnect();

    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) { gPipeline->Stop(); gPipeline.reset(); }
    gPipeline = std::make_unique<YuvPipeline>();
    gPipeline->SetYoloEnabled(gYoloOn.load());
    gPipeline->SetZoomScale(gUvcZoom.load());   // ★ 重连后恢复变焦倍数
    // ★ 重建管线回灌位置水印（USB 重连后画面不再丢水印）
    gPipeline->SetLocationText(gPendingLocationText);
    gPipeline->SetLocationBottomRatio(gPendingLocBottomRatio);
    bool ok = gPipeline->ConfigureExternal(w, h);
    if (ok) {
        if (!gPendingFontPath.empty()) gPipeline->LoadFont(gPendingFontPath);
        if (!gPendingClassNames.empty()) gPipeline->SetClassNames(gPendingClassNames);
        gPipeline->StartExternal();
    } else {
        gPipeline.reset();
    }
    napi_value r; napi_get_boolean(env, ok, &r); return r;
}

// ★ UvcConnectNapi（ArkTS 调用名：uvcConnect）：打开 USB 摄像头设备。
//   参数：args[0]=fd（ArkTS 经 USBManager 打开的设备节点），args[1]/args[2]=VID/PID，
//   args[3]/args[4]=总线号/设备地址（诊断用）。返回值：boolean 是否连接成功
static napi_value UvcConnectNapi(napi_env env, napi_callback_info info) {
    size_t argc = 5; napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t fd = -1, vid = 0, pid = 0, busnum = 0, devaddr = 0;
    if (argc > 0) napi_get_value_int32(env, args[0], &fd);
    if (argc > 1) napi_get_value_int32(env, args[1], &vid);
    if (argc > 2) napi_get_value_int32(env, args[2], &pid);
    if (argc > 3) napi_get_value_int32(env, args[3], &busnum);
    if (argc > 4) napi_get_value_int32(env, args[4], &devaddr);

    bool ok = UvcCamera::Instance().Connect(fd, vid, pid, busnum, devaddr);
    napi_value r; napi_get_boolean(env, ok, &r); return r;
}

// ★ UvcStartStreamingNapi（ArkTS 调用名：uvcStartStreaming）：启动 USB 取流。
//   参数：args[0]/args[1]=取流宽/高，args[2]=帧率。
//   要点：持 gPipelineMutex 把相机对象绑定到当前管线（帧回调解码后直投管线）。
//   返回值：boolean 是否取流成功
static napi_value UvcStartStreamingNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t w = 640, h = 480, fps = 30;
    if (argc > 0) napi_get_value_int32(env, args[0], &w);
    if (argc > 1) napi_get_value_int32(env, args[1], &h);
    if (argc > 2) napi_get_value_int32(env, args[2], &fps);

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(gPipelineMutex);
        if (gPipeline) ok = UvcCamera::Instance().Start(gPipeline.get(), w, h, fps);
    }
    napi_value r; napi_get_boolean(env, ok, &r); return r;
}

// ★ UvcStopStreamingNapi（ArkTS 调用名：uvcStopStreaming）：停止 USB 取流
//   （保留设备连接，可再次 startStreaming 而无需重连）
static napi_value UvcStopStreamingNapi(napi_env env, napi_callback_info) {
    UvcCamera::Instance().Stop();
    return nullptr;
}

// ★ UvcDisconnectNapi（ArkTS 调用名：uvcDisconnect）：停止取流并关闭 USB 设备
//   （释放 fd 与 libuvc 资源，下次使用需重新 uvcConnect）
static napi_value UvcDisconnectNapi(napi_env env, napi_callback_info) {
    UvcCamera::Instance().Stop();
    UvcCamera::Instance().Disconnect();
    return nullptr;
}

// 取最近一次 UVC Connect/Start 失败的具体原因（阶段+错误码+errno）
// （ArkTS 调用名：uvcGetLastError）返回值：string（空错误时返回 "no error"）
static napi_value UvcGetLastError(napi_env env, napi_callback_info) {
    std::string err = UvcCamera::Instance().GetLastError();
    if (err.empty()) err = "no error";
    napi_value r; napi_create_string_utf8(env, err.c_str(), err.size(), &r); return r;
}

// ★ 设备上报的格式/分辨率清单（诊断协商失败）
// （ArkTS 调用名：uvcDumpFormats）返回值：string（设备描述符中的格式/分辨率列表）
static napi_value UvcDumpFormatsNapi(napi_env env, napi_callback_info) {
    std::string s = UvcCamera::Instance().DumpFormats();
    napi_value r; napi_create_string_utf8(env, s.c_str(), s.size(), &r); return r;
}

// ★ 取流帧计数（"连接成功但黑屏"看门狗：0=无帧、停增=断流、decodeFail=解码失败）
// （ArkTS 调用名：uvcGetFrameCount）返回值：number（累计帧数）
static napi_value UvcGetFrameCountNapi(napi_env env, napi_callback_info) {
    napi_value r; napi_create_uint32(env, UvcCamera::Instance().FrameCount(), &r); return r;
}

// ★ UvcGetDecodeFailCountNapi（ArkTS 调用名：uvcGetDecodeFailCount）：USB 帧解码累计
//   失败计数（偶发增长属正常，持续快速增长=码流异常；配合帧数看门狗区分"无帧"
//   与"有帧解不出"两种黑屏成因）。返回值：number
static napi_value UvcGetDecodeFailCountNapi(napi_env env, napi_callback_info) {
    napi_value r; napi_create_uint32(env, UvcCamera::Instance().DecodeFailCount(), &r); return r;
}

// ★ 首帧诊断（帧描述尺寸 vs JPEG 真实尺寸 vs actual_bytes/帧头 + 解码返回码）：
//   连接成功但黑屏时一次定位真因
// （ArkTS 调用名：uvcGetFrameInfo）返回值：string（首帧多维诊断信息）
static napi_value UvcGetFrameInfoNapi(napi_env env, napi_callback_info) {
    std::string s = UvcCamera::Instance().GetFrameInfo();
    napi_value r; napi_create_string_utf8(env, s.c_str(), s.size(), &r); return r;
}

// ★ GetLatestResults（ArkTS 调用名：getLatestResults）：取最近一帧 YOLO 检测结果。
//   返回值：二维数组，每项 [x,y,w,h,score,label]（图像坐标，x,y 为框中心与边长）
static napi_value GetLatestResults(napi_env env, napi_callback_info) {
    napi_value result; napi_create_array(env, &result);
    std::vector<DetectItem> items;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) items = gPipeline->GetLatestResults(); }
    for (size_t i = 0; i < items.size(); ++i) {
        napi_value arr; napi_create_array_with_length(env, 6, &arr);
        float v[6] = {items[i].x, items[i].y, items[i].w, items[i].h, items[i].score, (float)items[i].label};
        for (int j = 0; j < 6; ++j) { napi_value num; napi_create_double(env, v[j], &num); napi_set_element(env, arr, j, num); }
        napi_set_element(env, result, i, arr);
    }
    return result;
}

// ★ GetInferCount（ArkTS 调用名：getInferCount）：取 YOLO 累计推理次数（HUD 统计）。
//   返回值：number（可能超 2^31，故用 napi_create_double）
static napi_value GetInferCount(napi_env env, napi_callback_info) {
    uint64_t c = 0;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) c = gPipeline->GetInferCount(); }
    napi_value r; napi_create_double(env, (double)c, &r); return r;
}

// ★ SetConfThresh（ArkTS 调用名：setConfThresh）：设置 YOLO 置信度阈值。
//   参数：args[0]=阈值（0~1）。对管线内检测与 detect 接口同时生效
static napi_value SetConfThresh(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double v = 0.25; napi_get_value_double(env, args[0], &v);
    gConfThresh = (float)v;
    napi_value u; napi_get_undefined(env, &u); return u;
}

// ★ CaptureLatestFrame（ArkTS 调用名：captureLatestFrame）：拍照——取当前显示帧
//   （含叠加层：检测框/水印/OCR 框）。返回值：ArrayBuffer（RGBA 数据，像素编码
//   需配合 getFrameSize）；管线未启动返回零长度 ArrayBuffer
static napi_value CaptureLatestFrame(napi_env env, napi_callback_info) {
    std::vector<uint8_t> data;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) data = gPipeline->GetLatestRgba(); }
    void *out = nullptr; napi_value buf;
    napi_create_arraybuffer(env, data.size(), &out, &buf);
    if (!data.empty() && out) std::memcpy(out, data.data(), data.size());
    return buf;
}

// ★ GetFrameSize（ArkTS 调用名：getFrameSize）：取最终显示帧尺寸。
//   返回值：[宽, 高] 数组（像素），ArkTS 拍照编码/画布绘制依赖此值
static napi_value GetFrameSize(napi_env env, napi_callback_info) {
    int w = 0, h = 0;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) gPipeline->GetFrameSize(w, h); }
    napi_value arr; napi_create_array_with_length(env, 2, &arr);
    napi_value n1, n2; napi_create_int32(env, w, &n1); napi_create_int32(env, h, &n2);
    napi_set_element(env, arr, 0, n1); napi_set_element(env, arr, 1, n2);
    return arr;
}

// ★ SetYoloEnabledNapi（ArkTS 调用名：setYoloEnabled）：YOLO 识别总开关。
//   参数：args[0]=boolean。要点：三重写入——gYoloOn 会话级缓存（管线重建后回灌）、
//   extern "C" SetYoloEnabled（native 侧检测状态）、当前管线实例（立即停止入队）
static napi_value SetYoloEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = true; napi_get_value_bool(env, args[0], &enabled);
    gYoloOn.store(enabled);
    SetYoloEnabled(enabled);
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) gPipeline->SetYoloEnabled(enabled); }
    return nullptr;
}

// ★ SetMotionEnabledNapi（ArkTS 调用名：setMotionEnabled）：运动检测开关
//   （鱼群活跃度评估）。参数：args[0]=boolean；实现在 yuv_pipeline.cpp（帧差统计）
static napi_value SetMotionEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false; napi_get_value_bool(env, args[0], &enabled);
    SetMotionEnabled(enabled);
    return nullptr;
}

// ★ GetMotionLevelNapi（ArkTS 调用名：getMotionLevel）：取当前画面运动强度。
//   返回值：number（帧差统计值，0=无运动，越大越剧烈）
static napi_value GetMotionLevelNapi(napi_env env, napi_callback_info info) {
    float level = GetMotionLevel();
    napi_value result; napi_create_double(env, (double)level, &result); return result;
}

// ★ SetDepthEnabledNapi（ArkTS 调用名：setDepthEnabled）：水深估算开关
//   （探鱼场景，结合运动强度与钓组参数估算）。参数：args[0]=boolean
static napi_value SetDepthEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false; napi_get_value_bool(env, args[0], &enabled);
    SetDepthEnabled(enabled);
    return nullptr;
}

// ★ GetDepthCmNapi（ArkTS 调用名：getDepthCm）：取当前估算水深。
//   返回值：number（厘米，未开启估算时为 0）
static napi_value GetDepthCmNapi(napi_env env, napi_callback_info info) {
    float depth = GetDepthCm();
    napi_value result; napi_create_double(env, (double)depth, &result); return result;
}

// ★ 用户设置水深模型参数：摄像头重量(g)、水的浑浊度(0清澈/1微浑/2浑浊)
// （ArkTS 调用名：setDepthParams）参数：args[0]=坠重（克），args[1]=浑浊度档位
static napi_value SetDepthParamsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double weightG = 30.0; int32_t turbidity = 1;
    if (argc > 0) napi_get_value_double(env, args[0], &weightG);
    if (argc > 1) napi_get_value_int32(env, args[1], &turbidity);
    SetDepthParams((float)weightG, (int)turbidity);
    return nullptr;
}

// ★ 识别框/标签画面显示开关：false 时检测照常，仅不绘制框与文字
// （ArkTS 调用名：setDrawBoxesEnabled）参数：args[0]=boolean
static napi_value SetDrawBoxesNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = true;
    if (argc > 0) napi_get_value_bool(env, args[0], &enabled);
    SetDrawBoxesEnabled(enabled);
    return nullptr;
}

// ★ 画质增强（USB/手机前后置全部相机生效）：
//   mode 0 关 / 1 非AI(亮度+锐度+对比度+饱和度) / 2 AI增强
//   （超分细节 + AI降噪 + 水下去浊 + 色彩还原，修复层异步合成不卡顿）
// （ArkTS 调用名：setUsbEnhance）参数：args[0]=mode，args[1..4]=亮度/锐度/对比度/饱和度，
//   args[5]=AI 强度，args[6]=降噪，args[7]=清晰度，args[8]=色彩还原
static napi_value SetUsbEnhanceNapi(napi_env env, napi_callback_info info) {
    size_t argc = 9; napi_value args[9];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t mode = 0, brightness = 0, sharpness = 0, contrast = 0, saturation = 0, strength = 100;
    int32_t denoise = 50, clarity = 50, color = 60;
    if (argc > 0) napi_get_value_int32(env, args[0], &mode);
    if (argc > 1) napi_get_value_int32(env, args[1], &brightness);
    if (argc > 2) napi_get_value_int32(env, args[2], &sharpness);
    if (argc > 3) napi_get_value_int32(env, args[3], &contrast);
    if (argc > 4) napi_get_value_int32(env, args[4], &saturation);
    if (argc > 5) napi_get_value_int32(env, args[5], &strength);
    if (argc > 6) napi_get_value_int32(env, args[6], &denoise);
    if (argc > 7) napi_get_value_int32(env, args[7], &clarity);
    if (argc > 8) napi_get_value_int32(env, args[8], &color);
    SetUsbEnhance((int)mode, (int)brightness, (int)sharpness,
                  (int)contrast, (int)saturation, (int)strength,
                  (int)denoise, (int)clarity, (int)color);
    return nullptr;
}

// ═══════════ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage）═══════════

// 异步加载三个人脸模型（对齐 initTable 模式，避免阻塞 UI 线程）
// ★ FaceInitCtx：异步工作上下文——三个模型路径 + 推理线程数 + 结果标志；
//   JS 入口 new、Complete 回调 delete（napi_async_work 固定资源生命周期，防泄漏）
struct FaceInitCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string detPath;
    std::string lmPath;
    std::string gaPath;
    int threads = 3;
    bool ok = false;
};

// ★ FaceInitExecute：异步加载执行体（napi 工作线程，UI 不阻塞）——
//   先确保 libocr.so 已加载（内部 dlopen+dlsym），再 faceInit 加载检测/关键点/性别年龄三模型
static void FaceInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<FaceInitCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "face init: ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::faceInit(ctx->detPath, ctx->lmPath, ctx->gaPath, ctx->threads);
}

// ★ FaceInitComplete：异步加载完成回调（JS 线程）——resolve Promise<boolean>，
//   按序清理 async work 与上下文（napi_delete_async_work → delete ctx）
static void FaceInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<FaceInitCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ InitFace（ArkTS 调用名：initFace）：异步加载人脸模型集，返回 Promise<boolean>。
//   参数：args[0]=检测模型路径，args[1]=关键点模型路径，args[2]=性别年龄模型路径，
//   args[3]=推理线程数（默认 3）。模型推理实现在 libocr.so，经 ocr_bridge 桥接
static napi_value InitFace(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char detPath[1024] = {0};
    char lmPath[1024] = {0};
    char gaPath[1024] = {0};
    size_t len = 0;
    int32_t threads = 3;
    if (argc > 0) napi_get_value_string_utf8(env, args[0], detPath, sizeof(detPath), &len);
    if (argc > 1) napi_get_value_string_utf8(env, args[1], lmPath, sizeof(lmPath), &len);
    if (argc > 2) napi_get_value_string_utf8(env, args[2], gaPath, sizeof(gaPath), &len);
    if (argc > 3) napi_get_value_int32(env, args[3], &threads);

    auto *ctx = new FaceInitCtx();
    ctx->detPath = std::string(detPath);
    ctx->lmPath = std::string(lmPath);
    ctx->gaPath = std::string(gaPath);
    ctx->threads = (int)threads;

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "initFace", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           FaceInitExecute, FaceInitComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ★ ReleaseFace（ArkTS 调用名：releaseFace）：释放人脸模型集（回收 onnxruntime 会话内存）
static napi_value ReleaseFace(napi_env env, napi_callback_info) {
    ocr_bridge::faceRelease();
    return nullptr;
}

// ★ 人脸识别画面开关：false 时工作线程不再入队，已画连线框 1.5s 过期自动消失
// （ArkTS 调用名：setFaceEnabled）参数：args[0]=boolean
static napi_value SetFaceEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    if (argc > 0) napi_get_value_bool(env, args[0], &enabled);
    SetFaceEnabled(enabled);
    return nullptr;
}

// ★ HUD 人脸信息（JSON：count/faces[gender,age,beauty,x,y,w,h]，500ms 轮询）
// （ArkTS 调用名：getFaceInfo）返回值：string（JSON，管线未启动返回 {"count":0,"faces":[]}）
static napi_value GetFaceInfoNapi(napi_env env, napi_callback_info) {
    std::string json = "{\"count\":0,\"faces\":[]}";
    {
        std::lock_guard<std::mutex> lock(gPipelineMutex);
        if (gPipeline) json = gPipeline->GetFaceInfoJson();
    }
    napi_value result = nullptr;
    napi_create_string_utf8(env, json.c_str(), json.size(), &result);
    return result;
}

// ★ AI 超分模型加载（libocr.so 经 OcrBridge dlsym）：onnxPath + 推理线程数
// （ArkTS 调用名：initSr）参数：args[0]=ONNX 模型路径，args[1]=线程数（默认 4）；
//   返回值：boolean 是否加载成功
static napi_value InitSrNapi(napi_env env, napi_callback_info info) {
    size_t argc = 2; napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {0}; size_t len = 0;
    int32_t threads = 4;
    if (argc > 0) napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);
    if (argc > 1) napi_get_value_int32(env, args[1], &threads);
    const bool ok = ocr_bridge::srInit(std::string(path), (int)threads);
    napi_value r;
    napi_get_boolean(env, ok, &r);
    return r;
}

// ★ 释放超分模型（切换/关闭时回收 25MB 级推理缓冲）
// （ArkTS 调用名：releaseSr）无参数无返回值
static napi_value ReleaseSrNapi(napi_env env, napi_callback_info info) {
    ocr_bridge::srRelease();
    return nullptr;
}

// ★ AttachPreviewSurface（ArkTS 调用名：attachPreviewSurface）：把 XComponent 的
//   预览 surface 绑定到管线。参数：args[0]=surfaceId 字符串（十进制）。
//   要点：由 id 创建 native window 交给管线；绑定后渲染帧直接上屏
static napi_value AttachPreviewSurface(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    char sidStr[64] = {0}; size_t len = 0;
    napi_get_value_string_utf8(env, args[0], sidStr, sizeof(sidStr), &len);
    uint64_t surfaceId = strtoull(sidStr, nullptr, 10);

    OHNativeWindow *window = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 || window == nullptr) {
        OH_LOG_ERROR(LOG_APP, "CreateNativeWindowFromSurfaceId failed");
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(gPipelineMutex);
        if (gPipeline) gPipeline->SetPreviewWindow(window);
    }
    return nullptr;
}

// ★ 录像直供：AVRecorder getInputSurface 的 surfaceId 交给 native，
//   由现有帧路径直接喂 NV12 帧（相机/预览不重配不中断）
// （ArkTS 调用名：attachRecordSurface）参数：args[0]=录像 surfaceId 字符串，
//   args[1]/args[2]=录像宽/高；无返回值
static napi_value AttachRecordSurfaceNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3; napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    char sidStr[64] = {0}; size_t len = 0;
    napi_get_value_string_utf8(env, args[0], sidStr, sizeof(sidStr), &len);
    uint64_t surfaceId = strtoull(sidStr, nullptr, 10);
    int width = 0;
    int height = 0;
    if (argc >= 3) {
        napi_get_value_int32(env, args[1], &width);
        napi_get_value_int32(env, args[2], &height);
    }

    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (!gPipeline) {
        OH_LOG_ERROR(LOG_APP, "AttachRecordSurface: pipeline not running");
        return nullptr;
    }
    gPipeline->AttachRecordSurface(surfaceId, width, height);
    return nullptr;
}

// ★ DetachRecordSurfaceNapi（ArkTS 调用名：detachRecordSurface）：解绑录像 surface，
//   录像停止/失败时调用，此后管线不再向录像窗口供帧
static napi_value DetachRecordSurfaceNapi(napi_env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->DetachRecordSurface();
    return nullptr;
}

// ★ SetRotationNapi（ArkTS 调用名：setRotation）：设置画面旋转角（0/90/180/270，
//   相机传感器方向+屏幕方向合成值）。参数：args[0]=角度；90/270 时宽高互换
static napi_value SetRotationNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t degrees = 0;
    napi_get_value_int32(env, args[0], &degrees);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->SetRotation(degrees);
    return nullptr;
}

// ★ 前置摄像头镜像开关（对齐 Android 前摄自拍镜像预览）
// （ArkTS 调用名：setMirror）参数：args[0]=boolean
static napi_value SetMirrorNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    bool mirror = false;
    napi_get_value_bool(env, args[0], &mirror);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->SetMirror(mirror);
    return nullptr;
}

// ★ ClearSurfaceNapi（ArkTS 调用名：clearSurface）：清空预览 surface（填黑帧），
//   防止切换摄像头/停管线后画面残留上一帧
static napi_value ClearSurfaceNapi(napi_env env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->ClearSurface();
    return nullptr;
}

// ★ USB 摄像头数字变焦（照抄 Android CameraUVC.setZoomScale：1.0~5.0，中心裁剪放大）
// （ArkTS 调用名：setUvcZoom）参数：args[0]=变焦倍数；会话级缓存 gUvcZoom，USB 重连后回灌
static napi_value SetUvcZoomNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    double scale = 1.0;
    napi_get_value_double(env, args[0], &scale);
    gUvcZoom.store((float)scale);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->SetZoomScale((float)scale);
    return nullptr;
}

// ★ 位置水印文字：ArkTS 逆地理解析的地理位置名，画到图片/录像底部居中（空串=清除）
// （ArkTS 调用名：setLocationText）参数：args[0]=位置名；会话级缓存，管线重建后回灌
static napi_value SetLocationTextNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    char text[256] = {0};
    size_t len = 0;
    napi_get_value_string_utf8(env, args[0], text, sizeof(text), &len);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    gPendingLocationText = std::string(text);
    if (gPipeline) gPipeline->SetLocationText(gPendingLocationText);
    return nullptr;
}

// ★ 位置水印纵向位置：预览底部被底部面板遮挡时，ArkTS 按可见区域传比例上移
// （ArkTS 调用名：setLocationBottomRatio）参数：args[0]=纵向位置比例（0.4~1.0）
static napi_value SetLocationBottomRatioNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    double ratio = 1.0;
    napi_get_value_double(env, args[0], &ratio);
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    gPendingLocBottomRatio = (float)ratio;
    if (gPipeline) gPipeline->SetLocationBottomRatio(gPendingLocBottomRatio);
    return nullptr;
}

// ═══════════ OCR ═══════════
// ★ OcrAsyncCtx：OCR 引擎初始化异步工作上下文——模型目录 + 结果标志；
//   生命周期与 FaceInitCtx 相同（Execute/Complete 配对 new/delete）
struct OcrAsyncCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string modelDir;
    bool ok = false;
};

// ★ OcrInitExecute：异步初始化执行体——确保 libocr.so 已加载后按"模型目录+
//   字典"方式初始化 det/cls/rec 三件套（内部 onnxruntime 建会话，耗时）
static void OcrInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<OcrAsyncCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "ocr bridge load failed, see OcrBridge logs");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::init(ctx->modelDir);
}

// ★ OcrInitComplete：初始化完成回调——resolve Promise<boolean>，清理 work 与上下文
static void OcrInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<OcrAsyncCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ InitOcr（ArkTS 调用名：initOcr）：异步初始化 OCR 引擎，返回 Promise<boolean>。
//   参数：args[0]=模型目录（内含 det/cls/rec/keys 文件）
static napi_value InitOcr(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char dir[1024] = {0};
    size_t len = 0;
    if (argc > 0) napi_get_value_string_utf8(env, args[0], dir, sizeof(dir), &len);

    auto *ctx = new OcrAsyncCtx();
    ctx->modelDir = std::string(dir);

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "initOcr", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           OcrInitExecute, OcrInitComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ★ 异步释放 OCR 引擎：release() 内部 join OCR worker（可能正跑长推理），
//   同步调用会卡主线程 >6s 触发 appfreeze，必须放到 napi 异步线程执行
struct OcrReleaseCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
};

// ★ OcrReleaseExecute：异步释放执行体（napi 工作线程）——必须异步的原因：
//   release 内部 join OCR worker（可能正跑长推理），同步调用会卡主线程 >6s
//   触发 appfreeze
static void OcrReleaseExecute(napi_env env, void *data) {
    ocr_bridge::release();
}

// ★ OcrReleaseComplete：释放完成回调——resolve Promise<undefined>，清理 work 与上下文
static void OcrReleaseComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<OcrReleaseCtx *>(data);
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ ReleaseOcr（ArkTS 调用名：releaseOcr）：异步释放 OCR 引擎，返回 Promise<undefined>。
//   要点：release 耗时（见 OcrReleaseExecute），整体放 napi 工作线程执行
static napi_value ReleaseOcr(napi_env env, napi_callback_info) {
    auto *ctx = new OcrReleaseCtx();
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "releaseOcr", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           OcrReleaseExecute, OcrReleaseComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ★ SetOcrEnabledNapi（ArkTS 调用名：setOcrEnabled）：OCR 识别总开关。
//   参数：args[0]=boolean；关闭后管线侧 maybeTrigger 变为空操作（省电）
static napi_value SetOcrEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    if (argc > 0) napi_get_value_bool(env, args[0], &enabled);
    ocr_bridge::setEnabled(enabled);
    return nullptr;
}

// ★ GetOcrTextNapi（ArkTS 调用名：getOcrText）：取最近一次 OCR 识别的整段文本。
//   返回值：string（各行以换行拼接），无结果/未加载返回空串
static napi_value GetOcrTextNapi(napi_env env, napi_callback_info) {
    std::string text = ocr_bridge::getResult();
    napi_value r = nullptr;
    napi_create_string_utf8(env, text.c_str(), text.size(), &r);
    return r;
}

// ★ OCR 识别进度（done/total），供 HUD"识别中"徽标显示（不再画在画面上）
// （ArkTS 调用名：getOcrProgress）返回值：[done, total] 数组
static napi_value GetOcrProgressNapi(napi_env env, napi_callback_info) {
    int done = 0, total = 0;
    ocr_bridge::getProgress(done, total);
    napi_value arr = nullptr;
    napi_create_array_with_length(env, 2, &arr);
    napi_value d = nullptr, t = nullptr;
    napi_create_int32(env, done, &d);
    napi_create_int32(env, total, &t);
    napi_set_element(env, arr, 0, d);
    napi_set_element(env, arr, 1, t);
    return arr;
}

// ═══════════ 能耗模式 / OCR 参数 ═══════════
// ★ 节奏参数统一入口：渲染节流 + YOLO 有目标/空闲间隔 + 水深采样间隔（立即生效）
// （ArkTS 调用名：setPipelineTiming）参数：args[0]=渲染节流 ms，args[1]=YOLO 有目标
//   推理间隔 ms，args[2]=YOLO 空闲推理间隔 ms，args[3]=传感器采样间隔 ms
static napi_value SetPipelineTimingNapi(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) return nullptr;
    int32_t renderMs = 40, yoloMs = 300, yoloIdleMs = 800, sensorMs = 250;
    napi_get_value_int32(env, args[0], &renderMs);
    napi_get_value_int32(env, args[1], &yoloMs);
    napi_get_value_int32(env, args[2], &yoloIdleMs);
    napi_get_value_int32(env, args[3], &sensorMs);
    SetPipelineTiming(renderMs, yoloMs, yoloIdleMs, sensorMs);
    return nullptr;
}

// ★ YOLO 推理线程数：会话级原子 + 当前检测器热更新（ncnn 下次 create_extractor 生效）
// （ArkTS 调用名：setYoloThreads）参数：args[0]=线程数
static napi_value SetYoloThreadsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t n = 8;
    napi_get_value_int32(env, args[0], &n);
    std::lock_guard<std::mutex> lock(gDetectorMutex);
    if (gDetector) gDetector->SetNumThreads(n);
    return nullptr;
}

// ★ OCR 定时识别间隔（能耗模式可调，立即生效）
// （ArkTS 调用名：setOcrTriggerInterval）参数：args[0]=间隔 ms
static napi_value SetOcrTriggerIntervalNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t ms = 4000;
    napi_get_value_int32(env, args[0], &ms);
    ocr_bridge::setTriggerInterval(ms);
    return nullptr;
}

// ★ OCR det 阈值三件套（检出/框置信度/文本框扩展，立即生效）
// （ArkTS 调用名：setOcrDetParams）参数：args[0]=检出阈值，args[1]=框置信度，args[2]=扩展比
static napi_value SetOcrDetParamsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return nullptr;
    double dbScore = 0.5, dbBox = 0.3, unclip = 1.6;
    napi_get_value_double(env, args[0], &dbScore);
    napi_get_value_double(env, args[1], &dbBox);
    napi_get_value_double(env, args[2], &unclip);
    ocr_bridge::setDetParams((float) dbScore, (float) dbBox, (float) unclip);
    return nullptr;
}

// ★ OCR 推理线程数（下次 init 生效）
// （ArkTS 调用名：setOcrNumThreads）参数：args[0]=线程数
static napi_value SetOcrNumThreadsNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;
    int32_t n = 4;
    napi_get_value_int32(env, args[0], &n);
    ocr_bridge::setNumThreads(n);
    return nullptr;
}

// ★ OCR 多模型集：按显式 det/cls/rec/keys 路径初始化（paddle / xiaomi 等任意命名）
struct OcrInitPathsCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string detPath;
    std::string clsPath;
    std::string recPath;
    std::string keysPath;
    bool ok = false;
};

// ★ OcrInitPathsExecute：异步初始化（显式路径版）执行体——按 det/cls/rec/keys
//   四个显式文件路径初始化，支持任意命名的模型集（paddle/xiaomi 等）
static void OcrInitPathsExecute(napi_env env, void *data) {
    auto *ctx = static_cast<OcrInitPathsCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::initPaths(ctx->detPath, ctx->clsPath, ctx->recPath, ctx->keysPath);
}

// ★ OcrInitPathsComplete：初始化（显式路径版）完成回调——resolve Promise，清理
static void OcrInitPathsComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<OcrInitPathsCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ InitOcrPaths（ArkTS 调用名：initOcrPaths）：按显式模型文件路径初始化 OCR 引擎，
//   返回 Promise<boolean>。参数：args[0]=det 路径，args[1]=cls 路径，args[2]=rec 路径，
//   args[3]=keys 字典路径。要点：参数不足时立即 resolve(false)，仍返回 Promise
//   保证 ArkTS 侧 await 风格统一
static napi_value InitOcrPaths(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) {
        napi_value promise = nullptr;
        napi_deferred deferred = nullptr;
        napi_create_promise(env, &deferred, &promise);
        napi_value r = nullptr;
        napi_get_boolean(env, false, &r);
        napi_resolve_deferred(env, deferred, r);
        return promise;
    }
    auto *ctx = new OcrInitPathsCtx();
    char buf[4][1024] = {};
    for (int i = 0; i < 4; i++) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[i], buf[i], sizeof(buf[i]), &len);
    }
    ctx->detPath = std::string(buf[0]);
    ctx->clsPath = std::string(buf[1]);
    ctx->recPath = std::string(buf[2]);
    ctx->keysPath = std::string(buf[3]);

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "initOcrPaths", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           OcrInitPathsExecute, OcrInitPathsComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ★ SetFontPath（ArkTS 调用名：setFontPath）：设置中文标签/水印渲染字体（TTF）。
//   要点：双写——gPendingFontPath 会话级缓存（管线重建后回灌）+ 当前管线实例立即生效
static napi_value SetFontPath(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    char path[1024] = {0}; size_t len = 0;
    napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);

    std::lock_guard<std::mutex> lock(gPipelineMutex);
    gPendingFontPath = std::string(path);
    if (gPipeline) {
        gPipeline->LoadFont(gPendingFontPath);
    }
    return nullptr;
}

// ★ SetClassNames（ArkTS 调用名：setClassNames）：设置 YOLO 类别名列表。
//   参数：args[0]=string 数组（与模型输出 label 索引对齐）。
//   要点：双写——会话级缓存 + 当前管线实例（检测框标签显示用）
static napi_value SetClassNames(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return nullptr;

    uint32_t len = 0;
    if (napi_get_array_length(env, args[0], &len) != napi_ok) return nullptr;

    std::vector<std::string> names;
    names.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        napi_value item;
        napi_get_element(env, args[0], i, &item);
        char buf[256] = {0}; size_t l = 0;
        napi_get_value_string_utf8(env, item, buf, sizeof(buf), &l);
        names.emplace_back(buf);
    }

    std::lock_guard<std::mutex> lock(gPipelineMutex);
    gPendingClassNames = names;
    if (gPipeline) {
        gPipeline->SetClassNames(gPendingClassNames);
    }
    return nullptr;
}

// ═══════════ 表格结构识别（PP-Structure SLANet）═══════════

// 加载 SLANet 表格模型（独立于 OCR 模型生命周期）
struct TableInitCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string modelPath;
    bool ok = false;
};

// ★ TableInitExecute：异步表格模型加载执行体——SLANet 独立于 OCR 三件套
//   （单独生命周期，卸载 OCR 不影响表格模型）
static void TableInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<TableInitCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "table init: ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::initTable(ctx->modelPath);
}

// ★ TableInitComplete：表格模型加载完成回调——resolve Promise<boolean>，清理
static void TableInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<TableInitCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ InitTable（ArkTS 调用名：initTable）：异步加载 SLANet 表格结构模型，
//   返回 Promise<boolean>。参数：args[0]=表格模型文件路径
static napi_value InitTable(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {0};
    size_t len = 0;
    if (argc > 0) napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);

    auto *ctx = new TableInitCtx();
    ctx->modelPath = std::string(path);

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "initTable", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           TableInitExecute, TableInitComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// 单帧同步识别：取"叠加绘制前"的干净帧（不含水印/检测框，避免识别出
// 自绘内容）→ SLANet 结构 + 表格区域 OCR → 单元格匹配 → JSON。
// ★ 无框自动换帧重试：SLANet 未识别到表格结构时换新画面再试（最多 3 次），
//   免去用户手动反复点击；预览图（采集帧/结构框标注图）经 tsfn 实时回传 JS
struct TableRecognizeCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    napi_threadsafe_function tsfn = nullptr;  // 阶段事件 → JS 回调（未传回调则为空）
    std::string outDir;
    std::string json;
};

// tsfn 载荷：识别阶段事件（stage + 预览图路径）
struct TableEventData {
    int stage = 0;
    std::string path;
};

// JS 线程：调用 ArkTS 回调 onEvent(stage, path)
static void TableEventJsCall(napi_env env, napi_value jsCallback, void *context, void *data) {
    auto *ev = static_cast<TableEventData *>(data);
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    napi_value stageVal = nullptr;
    napi_value pathVal = nullptr;
    napi_create_int32(env, ev->stage, &stageVal);
    napi_create_string_utf8(env, ev->path.c_str(), ev->path.size(), &pathVal);
    napi_value args[2] = {stageVal, pathVal};
    napi_value result = nullptr;
    napi_call_function(env, undefined, jsCallback, 2, args, &result);
    delete ev;
}

// libocr 识别线程内同步回调 → tsfn 投递到 JS 线程（非阻塞，队列打满即丢弃）
static void tableEventTrampoline(int stage, const char *path, void *userdata) {
    auto *ctx = static_cast<TableRecognizeCtx *>(userdata);
    if (!ctx || !ctx->tsfn || !path) return;
    auto *ev = new TableEventData();
    ev->stage = stage;
    ev->path = std::string(path);
    if (napi_call_threadsafe_function(ctx->tsfn, ev, napi_tsfn_nonblocking) != napi_ok) {
        delete ev;
    }
}

// ★ TableRecognizeExecute：表格识别执行体（napi 工作线程）——核心流程：
//   抓"叠加绘制前"的干净帧（避免把水印/自绘框识别成内容）→ recognizeTableEx
//   （SLANet 结构 + 表格区域 OCR + 单元格匹配 → JSON）→ 未识别到表格结构时
//   换新画面自动重试（最多 3 次，间隔 600ms 等画面变化）。
//   要点：等干净帧期间持 gPipelineMutex（防管线被销毁），推理本身不持锁
static void TableRecognizeExecute(napi_env env, void *data) {
    auto *ctx = static_cast<TableRecognizeCtx *>(data);
    ctx->json.clear();
    for (int attempt = 0; attempt < 3; attempt++) {
        std::vector<uint8_t> rgba;
        int w = 0;
        int h = 0;
        {
            // 持 gPipelineMutex 等待干净帧（最长 600ms），防止等待期间管线被销毁；
            // 推理本身不持锁
            std::lock_guard<std::mutex> lock(gPipelineMutex);
            if (!gPipeline) {
                ctx->json = "{\"error\":\"相机未启动\"}";
                return;
            }
            gPipeline->RequestCleanSnapshot();
            if (!gPipeline->WaitCleanSnapshot(600, rgba, w, h)) {
                if (ctx->json.empty())
                    ctx->json = "{\"error\":\"未取到相机画面，请开启预览后重试\"}";
                return;
            }
        }
        bool structureFound = false;
        ctx->json = ocr_bridge::recognizeTableEx(rgba.data(), w, h, ctx->outDir,
                                                 tableEventTrampoline, ctx, structureFound);
        if (structureFound) return;  // 有框：锁定当前帧，结果即最终 JSON
        // 无框：稍候换新画面自动重试（间隔等待画面变化）
        if (attempt < 2) std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
    // 3 次均无框：json 为最后一次的 {"error":"未识别到表格结构"}，换成可操作提示
    if (ctx->json.find("未识别到表格结构") != std::string::npos) {
        ctx->json = "{\"error\":\"未识别到表格，请对准表格画面后重试\"}";
    }
}

// ★ TableRecognizeComplete：识别完成回调——先释放 tsfn（必须先于 resolve，
//   否则 Promise 结束后 JS 回调仍可能被调用），再 resolve JSON 串，最后清理
static void TableRecognizeComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<TableRecognizeCtx *>(data);
    if (ctx->tsfn) {
        napi_release_threadsafe_function(ctx->tsfn, napi_tsfn_release);
        ctx->tsfn = nullptr;
    }
    napi_value result = nullptr;
    napi_create_string_utf8(env, ctx->json.c_str(), ctx->json.size(), &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ RecognizeTable（ArkTS 调用名：recognizeTable）：单次表格结构识别，
//   返回 Promise<string>（结果 JSON）。参数：args[0]=预览图输出目录
//   （table_frame.jpg/table_boxed.jpg 落盘位置），args[1]=阶段事件回调
//   onEvent(stage, path)（可省略，经 tsfn 跨线程投递）。
//   要点：识别在 napi 工作线程执行；预览图事件由 tableEventTrampoline +
//   TableEventJsCall 中转回 JS 线程
static napi_value RecognizeTable(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr, nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    auto *ctx = new TableRecognizeCtx();
    // 参数 0：预览图输出目录（filesDir，table_frame.jpg/table_boxed.jpg 落这里）
    if (argc > 0) {
        char buf[1024] = {0};
        size_t len = 0;
        if (napi_get_value_string_utf8(env, args[0], buf, sizeof(buf), &len) == napi_ok) {
            ctx->outDir = std::string(buf);
        }
    }
    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    // 参数 1：阶段事件回调 onEvent(stage, path) → tsfn 跨线程投递
    if (argc > 1) {
        napi_value eventRes = nullptr;
        napi_create_string_utf8(env, "tableEvent", NAPI_AUTO_LENGTH, &eventRes);
        napi_create_threadsafe_function(env, args[1], nullptr, eventRes,
                                        0, 1, nullptr, nullptr, nullptr,
                                        TableEventJsCall, &ctx->tsfn);
    }
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "recognizeTable", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           TableRecognizeExecute, TableRecognizeComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ═══════════ AI 旧照片高清彩色修复（相册图库 → restorePhoto）═══════════
// 处理流水线（全部端侧本地，无云端上传）：
//   ① 字节序统一：部分设备 readPixelsToBuffer 输出 BGRA_8888，按调用方标志转 RGBA
//   ② 边缘保持去噪：cv::bilateralFilter（5 邻域 / sigma 40）抑制老照片颗粒与扫描噪点
//   ②.5 AI 上色（DDColor-L 约 241MB）：Lab 色度离散度检测近单色（黑白/棕褐/泛黄）
//       且模型就绪时才调上色——模型预测 Lab a/b 色度与原图亮度 L 合成自然彩色
//       （真黑白上色）；彩色照片绝不被模型颜色覆盖，模型未就绪/失败优雅跳过
//   ③ AI 超分 ×N：quicksrnetmedium.onnx（libocr.so 内 onnxruntime CPU 推理），
//      模型未就绪或推理失败时优雅回退原尺寸（修复流程不中断）
//   ④ 褪色修复（纯手写两遍像素循环，无额外依赖）：
//      4a. 灰世界自动白平衡——R/G/B 通道均值拉齐到总均值，增益夹 [0.8,1.3]
//          防极端场景色相翻转（典型老照片黄褐偏色由此去除）
//      4b. 亮度 0.5%~99.5% 百分位裁剪 + 线性拉伸——褪色发灰照片恢复黑白场对比度
//          （直方图区间 <30 灰阶时放弃拉伸，避免放大平坦图噪声）
//      4c. 饱和度 ×1.25——围绕亮度轴推离，色彩回鲜但保留明暗层次
// 输出恒为 RGBA_8888 紧凑像素，供 ArkTS createPixelMapSync + JPEG 编码入库。

// ★ RestorePhotoCtx：修复任务上下文——JS 线程拷入输入像素，napi 工作线程
//   执行全流水线，Complete 回调（JS 线程）回传输出像素并释放资源
struct RestorePhotoCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::vector<uint8_t> in;    // 输入像素（bgra=true 时为 BGRA 字节序，否则 RGBA）
    int w = 0;
    int h = 0;
    bool bgra = false;
    std::vector<uint8_t> out;   // 输出像素（恒 RGBA_8888）
    int outW = 0;
    int outH = 0;
    bool ok = false;
};

// ★ RestorePhotoExecute：修复执行体（napi 工作线程，数秒级耗时，不阻塞 UI）
static void RestorePhotoExecute(napi_env env, void *data) {
    auto *ctx = static_cast<RestorePhotoCtx *>(data);
    ctx->ok = false;
    const int w = ctx->w;
    const int h = ctx->h;
    if (w < 2 || h < 2 || ctx->in.size() != static_cast<size_t>(w) * h * 4) {
        return;
    }

    // ① BGRA → RGBA 统一字节序（仅交换 R/B 两个字节，A 不动）
    if (ctx->bgra) {
        for (size_t i = 0; i < ctx->in.size(); i += 4) {
            std::swap(ctx->in[i], ctx->in[i + 2]);
        }
    }

    // ② 边缘保持去噪：RGBA→BGR → bilateralFilter → 回 RGBA（in 缓冲就地更新）
    cv::Mat rgbaIn(h, w, CV_8UC4, ctx->in.data());
    cv::Mat bgr;
    cv::cvtColor(rgbaIn, bgr, cv::COLOR_RGBA2BGR);
    cv::Mat bgrDenoised;
    cv::bilateralFilter(bgr, bgrDenoised, 5, 40.0, 40.0);
    cv::cvtColor(bgrDenoised, rgbaIn, cv::COLOR_BGR2RGBA);

    // ②.5 黑白/棕褐老照片 AI 上色（DDColor-L）：
    //     降采样 256px 计算 Lab a/b 通道标准差（色度离散度）判定近单色——
    //     黑白照片 ≈0、棕褐色调 3~6、真实彩色 15 以上，阈值 8 只放过彩色；
    //     判定命中且上色模型就绪时原地调用（rgbaIn 同输入输出缓冲），
    //     上色失败静默跳过（后续去噪/超分/褪色修复照常，修复链不中断）
    if (ocr_bridge::colorIsReady()) {
        cv::Mat small;
        cv::resize(bgrDenoised, small, cv::Size(256, 256), 0, 0, cv::INTER_AREA);
        cv::Mat smallLab;
        cv::cvtColor(small, smallLab, cv::COLOR_BGR2Lab);  // 8U Lab：a/b +128 偏移，标准差不变
        cv::Mat labChans[3];
        cv::split(smallLab, labChans);
        cv::Scalar meanA, stdA, meanB, stdB;
        cv::meanStdDev(labChans[1], meanA, stdA);
        cv::meanStdDev(labChans[2], meanB, stdB);
        const double chromaSpread = std::sqrt(stdA[0] * stdA[0] + stdB[0] * stdB[0]);
        if (chromaSpread < 8.0 &&
            ocr_bridge::colorize(rgbaIn.data, w, h, rgbaIn.data, w, h)) {
            OH_LOG_INFO(LOG_APP, "restorePhoto: colorized (chromaSpread=%{public}.1f)",
                        chromaSpread);
        }
    }

    // ③ AI 超分 ×N：模型就绪时放大，失败/未就绪回退原尺寸
    const int scale = ocr_bridge::srIsReady() ? ocr_bridge::srScale() : 0;
    if (scale >= 2) {
        ctx->outW = w * scale;
        ctx->outH = h * scale;
        ctx->out.resize(static_cast<size_t>(ctx->outW) * ctx->outH * 4);
        if (!ocr_bridge::srUpscale(rgbaIn.data, w, h, ctx->out.data(),
                                   ctx->outW, ctx->outH)) {
            OH_LOG_WARN(LOG_APP, "restorePhoto: srUpscale failed, fallback to original size");
            ctx->outW = w;
            ctx->outH = h;
            ctx->out.assign(ctx->in.begin(), ctx->in.end());
        }
    } else {
        ctx->outW = w;
        ctx->outH = h;
        ctx->out.assign(ctx->in.begin(), ctx->in.end());
    }

    // ④ 褪色修复——第一遍：统计直方图 + 三通道均值（灰世界白平衡与百分位拉伸依据）
    const size_t px = static_cast<size_t>(ctx->outW) * ctx->outH;
    uint8_t *p = ctx->out.data();
    int hist[256] = {0};
    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    for (size_t i = 0; i < px; i++, p += 4) {
        // 整数近似亮度（R30 G59 B11 /100），避免浮点直方图
        hist[(p[0] * 30 + p[1] * 59 + p[2] * 11) / 100]++;
        sumR += p[0];
        sumG += p[1];
        sumB += p[2];
    }
    const double meanAll = (sumR + sumG + sumB) / (3.0 * static_cast<double>(px));
    // 4a. 灰世界增益：meanAll / 通道均值，夹 [0.8, 1.3]（lambda 内联钳制）
    auto clampGain = [meanAll, px](double chSum) -> float {
        double g = meanAll / (chSum / static_cast<double>(px) + 1e-6);
        if (g < 0.8) g = 0.8;
        if (g > 1.3) g = 1.3;
        return static_cast<float>(g);
    };
    const float gainR = clampGain(sumR);
    const float gainG = clampGain(sumG);
    const float gainB = clampGain(sumB);
    // 4b. 亮度百分位：0.5% 低分位与 99.5% 高分位作为裁剪点
    const long clipLoCount = static_cast<long>(static_cast<double>(px) * 0.005);
    const long clipHiCount = static_cast<long>(static_cast<double>(px) * 0.995);
    int lo = 0;
    int hi = 255;
    long acc = 0;
    for (int i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc >= clipLoCount) { lo = i; break; }
    }
    acc = 0;
    for (int i = 255; i >= 0; i--) {
        acc += hist[i];
        if (acc >= px - clipHiCount) { hi = i; break; }
    }
    if (hi - lo < 30) {
        // 拉伸空间不足（近乎纯色/极端曝光图）：放弃拉伸与增益，仅做饱和度微调
        lo = 0;
        hi = 255;
    }
    const float stretch = 255.0f / static_cast<float>(hi - lo);
    const float loF = static_cast<float>(lo);
    // 4c. 第二遍：白平衡 → 亮度拉伸 → 饱和度 1.25x → 钳制写回
    p = ctx->out.data();
    for (size_t i = 0; i < px; i++, p += 4) {
        float r = p[0] * gainR;
        float g = p[1] * gainG;
        float b = p[2] * gainB;
        r = (r - loF) * stretch;
        g = (g - loF) * stretch;
        b = (b - loF) * stretch;
        const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        r = lum + (r - lum) * 1.25f;
        g = lum + (g - lum) * 1.25f;
        b = lum + (b - lum) * 1.25f;
        p[0] = r < 0.0f ? 0 : (r > 255.0f ? 255 : static_cast<uint8_t>(r + 0.5f));
        p[1] = g < 0.0f ? 0 : (g > 255.0f ? 255 : static_cast<uint8_t>(g + 0.5f));
        p[2] = b < 0.0f ? 0 : (b > 255.0f ? 255 : static_cast<uint8_t>(b + 0.5f));
    }
    ctx->ok = true;
}

// ★ RestorePhotoComplete：完成回调（JS 线程）——resolve {buf,w,h}；
//   失败 resolve null（调用方按 null 兜底提示），按序清理 async work 与 ctx
static void RestorePhotoComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<RestorePhotoCtx *>(data);
    napi_value result = nullptr;
    if (ctx->ok) {
        napi_create_object(env, &result);
        void *abData = nullptr;
        napi_value ab = nullptr;
        napi_create_arraybuffer(env, ctx->out.size(), &abData, &ab);
        if (!ctx->out.empty()) {
            memcpy(abData, ctx->out.data(), ctx->out.size());
        }
        napi_value wv = nullptr;
        napi_value hv = nullptr;
        napi_create_int32(env, ctx->outW, &wv);
        napi_create_int32(env, ctx->outH, &hv);
        napi_set_named_property(env, result, "buf", ab);
        napi_set_named_property(env, result, "w", wv);
        napi_set_named_property(env, result, "h", hv);
    } else {
        napi_get_null(env, &result);
    }
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ RestorePhoto（ArkTS 调用名：restorePhoto）：单张照片 AI 修复，返回
//   Promise<{buf: ArrayBuffer, w: number, h: number} | null>。
//   参数：args[0]=输入像素 ArrayBuffer（w*h*4 字节），args[1]=宽，args[2]=高，
//   args[3]=是否 BGRA 字节序（image.ImageInfo.pixelFormat === BGRA_8888 时 true）
static napi_value RestorePhoto(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t w = 0;
    int32_t h = 0;
    bool bgra = false;
    void *data = nullptr;
    size_t len = 0;
    if (argc > 1) napi_get_value_int32(env, args[1], &w);
    if (argc > 2) napi_get_value_int32(env, args[2], &h);
    if (argc > 3) napi_get_value_bool(env, args[3], &bgra);

    // 参数校验失败：resolve null（ArkTS 侧按修复失败兜底），不 reject 简化调用方
    if (argc < 3 || w < 2 || h < 2 ||
        napi_get_arraybuffer_info(env, args[0], &data, &len) != napi_ok || !data ||
        len != static_cast<size_t>(w) * h * 4) {
        napi_deferred deferred = nullptr;
        napi_value promise = nullptr;
        napi_value nullRes = nullptr;
        napi_create_promise(env, &deferred, &promise);
        napi_get_null(env, &nullRes);
        napi_resolve_deferred(env, deferred, nullRes);
        return promise;
    }

    auto *ctx = new RestorePhotoCtx();
    ctx->w = w;
    ctx->h = h;
    ctx->bgra = bgra;
    // JS 线程内完成输入拷贝（arraybuffer 在工作线程期间可能被调用方释放）
    ctx->in.assign(static_cast<const uint8_t *>(data),
                   static_cast<const uint8_t *>(data) + len);

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "restorePhoto", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           RestorePhotoExecute, RestorePhotoComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ═══════════ AI 黑白照片上色模型加载（DDColor-L）═══════════

// 异步加载 241MB 上色模型（对齐 InitFace 模式：同步加载会阻塞 UI 线程数秒致卡死）
// ★ ColorInitCtx：异步工作上下文——模型路径 + 推理线程数 + 结果标志；
//   JS 入口 new、Complete 回调 delete（napi_async_work 固定资源生命周期，防泄漏）
struct ColorInitCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string path;
    int threads = 4;
    bool ok = false;
};

// ★ ColorInitExecute：异步加载执行体（napi 工作线程）——先确保 libocr.so
//   已加载（内部 dlopen+dlsym），再 colorInit 建 DDColor-L 会话（241MB 级耗时数秒）
static void ColorInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<ColorInitCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "color init: ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::colorInit(ctx->path, ctx->threads);
}

// ★ ColorInitComplete：加载完成回调（JS 线程）——resolve Promise<boolean>，
//   按序清理 async work 与上下文（napi_delete_async_work → delete ctx）
static void ColorInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<ColorInitCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

// ★ InitColor（ArkTS 调用名：initColor）：异步加载 AI 上色模型，返回 Promise<boolean>。
//   参数：args[0]=ONNX 模型路径，args[1]=推理线程数（默认 4）。上色实现在
//   libocr.so（ColorizeNet.cpp），经 ocr_bridge 桥接；restorePhoto 流水线内按
//   Lab 色度离散度自动判定黑白/棕褐照片并调用
static napi_value InitColor(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    char path[1024] = {0};
    size_t len = 0;
    int32_t threads = 4;
    if (argc > 0) napi_get_value_string_utf8(env, args[0], path, sizeof(path), &len);
    if (argc > 1) napi_get_value_int32(env, args[1], &threads);

    auto *ctx = new ColorInitCtx();
    ctx->path = std::string(path);
    ctx->threads = (int)threads;

    napi_value promise = nullptr;
    napi_create_promise(env, &ctx->deferred, &promise);

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "initColor", NAPI_AUTO_LENGTH, &resourceName);
    napi_create_async_work(env, nullptr, resourceName,
                           ColorInitExecute, ColorInitComplete, ctx, &ctx->work);
    napi_queue_async_work(env, ctx->work);
    return promise;
}

// ★ Init：entry 模块初始化回调——把全部导出函数经 napi_property_descriptor
//   挂载到 exports 对象。每项第一个字符串即 ArkTS 侧 import 'libentry.so' 后
//   直接调用的方法名（下表为全部 native 能力的唯一权威清单，按功能分组）
EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"initYolo",          nullptr, InitYolo,          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"detect",            nullptr, Detect,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseYolo",       nullptr, ReleaseYolo,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createYuvReceiver", nullptr, CreateYuvReceiver, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startPipeline",     nullptr, StartPipeline,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"updateFrameSize",   nullptr, UpdateFrameSizeNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopPipeline",      nullptr, StopPipeline,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLatestResults",  nullptr, GetLatestResults,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getInferCount",     nullptr, GetInferCount,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setConfThresh",     nullptr, SetConfThresh,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"captureLatestFrame",nullptr, CaptureLatestFrame,nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFrameSize",      nullptr, GetFrameSize,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setYoloEnabled",    nullptr, SetYoloEnabledNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDrawBoxesEnabled", nullptr, SetDrawBoxesNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setUsbEnhance",     nullptr, SetUsbEnhanceNapi,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setMotionEnabled",  nullptr, SetMotionEnabledNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getMotionLevel",    nullptr, GetMotionLevelNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDepthEnabled",   nullptr, SetDepthEnabledNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getDepthCm",        nullptr, GetDepthCmNapi,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDepthParams",    nullptr, SetDepthParamsNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachPreviewSurface", nullptr, AttachPreviewSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachRecordSurface",  nullptr, AttachRecordSurfaceNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"detachRecordSurface",  nullptr, DetachRecordSurfaceNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFontPath",          nullptr, SetFontPath,          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setClassNames",        nullptr, SetClassNames,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRotation",          nullptr, SetRotationNapi,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setMirror",            nullptr, SetMirrorNapi,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearSurface",         nullptr, ClearSurfaceNapi,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setUvcZoom",           nullptr, SetUvcZoomNapi,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLocationText",      nullptr, SetLocationTextNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLocationBottomRatio", nullptr, SetLocationBottomRatioNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setupExternalPipeline", nullptr, SetupExternalPipeline, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcConnect",           nullptr, UvcConnectNapi,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcStartStreaming",    nullptr, UvcStartStreamingNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcStopStreaming",     nullptr, UvcStopStreamingNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcDisconnect",        nullptr, UvcDisconnectNapi,     nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcGetLastError",      nullptr, UvcGetLastError,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcDumpFormats",       nullptr, UvcDumpFormatsNapi,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcGetFrameCount",     nullptr, UvcGetFrameCountNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcGetDecodeFailCount", nullptr, UvcGetDecodeFailCountNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"uvcGetFrameInfo",       nullptr, UvcGetFrameInfoNapi,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initOcr",              nullptr, InitOcr,              nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initOcrPaths",         nullptr, InitOcrPaths,         nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseOcr",           nullptr, ReleaseOcr,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOcrEnabled",        nullptr, SetOcrEnabledNapi,    nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getOcrText",           nullptr, GetOcrTextNapi,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getOcrProgress",       nullptr, GetOcrProgressNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPipelineTiming",    nullptr, SetPipelineTimingNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setYoloThreads",       nullptr, SetYoloThreadsNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOcrTriggerInterval", nullptr, SetOcrTriggerIntervalNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOcrDetParams",      nullptr, SetOcrDetParamsNapi,  nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setOcrNumThreads",     nullptr, SetOcrNumThreadsNapi, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initTable",            nullptr, InitTable,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initSr",               nullptr, InitSrNapi,           nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initColor",            nullptr, InitColor,            nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseSr",            nullptr, ReleaseSrNapi,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initFace",             nullptr, InitFace,             nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseFace",          nullptr, ReleaseFace,          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFaceEnabled",       nullptr, SetFaceEnabledNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFaceInfo",          nullptr, GetFaceInfoNapi,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"recognizeTable",       nullptr, RecognizeTable,       nullptr, nullptr, nullptr, napi_default, nullptr},
        {"restorePhoto",         nullptr, RestorePhoto,         nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

// ★ entryModule：NAPI 模块描述——modname "entry" 与 ArkTS 侧
//   import xxx from 'libentry.so' 一一对应；加载 .so 时系统回调 Init 完成导出挂载
static napi_module entryModule = {
    .nm_version = 1, .nm_flags = 0, .nm_filename = nullptr,
    .nm_register_func = Init, .nm_modname = "entry",
    .nm_priv = nullptr, .reserved = { 0 },
};
// ★ RegisterEntryModule：.so 加载时自动注册模块（constructor 属性）——
//   无需外部主动调用，模块注册在动态库装载阶段即完成
extern "C" __attribute__((constructor)) void RegisterEntryModule() { napi_module_register(&entryModule); }