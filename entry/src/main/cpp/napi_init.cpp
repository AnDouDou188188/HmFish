// ★ NAPI 桥接层：ArkTS ↔ native(libentry.so) 全部导出函数注册与参数转换，
//   含管线生命周期、设置下发、拍照/录像、UVC 连接取流等入口
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

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "NAPI"

#include "yolo_detector.h"
#include "yuv_pipeline.h"
#include "OcrBridge.h"
#include "uvc_camera.h"

static std::unique_ptr<YoloDetector> gDetector;
static std::mutex gDetectorMutex;
static float gConfThresh = 0.25f;
static std::unique_ptr<YuvPipeline> gPipeline;
static std::mutex gPipelineMutex;

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

bool NativeDetectOnRgba(const uint8_t *rgba, int w, int h, std::vector<Detection> &out) {
    std::unique_lock<std::mutex> lock(gDetectorMutex, std::try_to_lock);
    if (!lock.owns_lock() || !gDetector) return false;
    out = gDetector->Detect(rgba, w, h, gConfThresh, 0.25f);
    return true;
}

// ═══════════ YOLO ═══════════
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

static napi_value ReleaseYolo(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(gDetectorMutex);
    if (gDetector) { gDetector->Release(); gDetector.reset(); }
    napi_value u; napi_get_undefined(env, &u); return u;
}

// ═══════════ YuvPipeline (保持原样) ═══════════
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

static napi_value StopPipeline(napi_env env, napi_callback_info) {
    // 若 UVC 尚在推流，先停 UVC 再销毁管线，避免帧回调访问悬垂指针
    UvcCamera::Instance().Stop();
    UvcCamera::Instance().Disconnect();
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) { gPipeline->Stop(); gPipeline.reset(); }
    return nullptr;
}

// ═══════════ UVC（USB 摄像头，fd 注入） ═══════════
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

static napi_value UvcStopStreamingNapi(napi_env env, napi_callback_info) {
    UvcCamera::Instance().Stop();
    return nullptr;
}

static napi_value UvcDisconnectNapi(napi_env env, napi_callback_info) {
    UvcCamera::Instance().Stop();
    UvcCamera::Instance().Disconnect();
    return nullptr;
}

// 取最近一次 UVC Connect/Start 失败的具体原因（阶段+错误码+errno）
static napi_value UvcGetLastError(napi_env env, napi_callback_info) {
    std::string err = UvcCamera::Instance().GetLastError();
    if (err.empty()) err = "no error";
    napi_value r; napi_create_string_utf8(env, err.c_str(), err.size(), &r); return r;
}

// ★ 设备上报的格式/分辨率清单（诊断协商失败）
static napi_value UvcDumpFormatsNapi(napi_env env, napi_callback_info) {
    std::string s = UvcCamera::Instance().DumpFormats();
    napi_value r; napi_create_string_utf8(env, s.c_str(), s.size(), &r); return r;
}

// ★ 取流帧计数（"连接成功但黑屏"看门狗：0=无帧、停增=断流、decodeFail=解码失败）
static napi_value UvcGetFrameCountNapi(napi_env env, napi_callback_info) {
    napi_value r; napi_create_uint32(env, UvcCamera::Instance().FrameCount(), &r); return r;
}

static napi_value UvcGetDecodeFailCountNapi(napi_env env, napi_callback_info) {
    napi_value r; napi_create_uint32(env, UvcCamera::Instance().DecodeFailCount(), &r); return r;
}

// ★ 首帧诊断（帧描述尺寸 vs JPEG 真实尺寸 vs actual_bytes/帧头 + 解码返回码）：
//   连接成功但黑屏时一次定位真因
static napi_value UvcGetFrameInfoNapi(napi_env env, napi_callback_info) {
    std::string s = UvcCamera::Instance().GetFrameInfo();
    napi_value r; napi_create_string_utf8(env, s.c_str(), s.size(), &r); return r;
}

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

static napi_value GetInferCount(napi_env env, napi_callback_info) {
    uint64_t c = 0;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) c = gPipeline->GetInferCount(); }
    napi_value r; napi_create_double(env, (double)c, &r); return r;
}

static napi_value SetConfThresh(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    double v = 0.25; napi_get_value_double(env, args[0], &v);
    gConfThresh = (float)v;
    napi_value u; napi_get_undefined(env, &u); return u;
}

static napi_value CaptureLatestFrame(napi_env env, napi_callback_info) {
    std::vector<uint8_t> data;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) data = gPipeline->GetLatestRgba(); }
    void *out = nullptr; napi_value buf;
    napi_create_arraybuffer(env, data.size(), &out, &buf);
    if (!data.empty() && out) std::memcpy(out, data.data(), data.size());
    return buf;
}

static napi_value GetFrameSize(napi_env env, napi_callback_info) {
    int w = 0, h = 0;
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) gPipeline->GetFrameSize(w, h); }
    napi_value arr; napi_create_array_with_length(env, 2, &arr);
    napi_value n1, n2; napi_create_int32(env, w, &n1); napi_create_int32(env, h, &n2);
    napi_set_element(env, arr, 0, n1); napi_set_element(env, arr, 1, n2);
    return arr;
}

static napi_value SetYoloEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = true; napi_get_value_bool(env, args[0], &enabled);
    gYoloOn.store(enabled);
    SetYoloEnabled(enabled);
    { std::lock_guard<std::mutex> lock(gPipelineMutex); if (gPipeline) gPipeline->SetYoloEnabled(enabled); }
    return nullptr;
}

static napi_value SetMotionEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false; napi_get_value_bool(env, args[0], &enabled);
    SetMotionEnabled(enabled);
    return nullptr;
}

static napi_value GetMotionLevelNapi(napi_env env, napi_callback_info info) {
    float level = GetMotionLevel();
    napi_value result; napi_create_double(env, (double)level, &result); return result;
}

static napi_value SetDepthEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false; napi_get_value_bool(env, args[0], &enabled);
    SetDepthEnabled(enabled);
    return nullptr;
}

static napi_value GetDepthCmNapi(napi_env env, napi_callback_info info) {
    float depth = GetDepthCm();
    napi_value result; napi_create_double(env, (double)depth, &result); return result;
}

// ★ 用户设置水深模型参数：摄像头重量(g)、水的浑浊度(0清澈/1微浑/2浑浊)
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
struct FaceInitCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string detPath;
    std::string lmPath;
    std::string gaPath;
    int threads = 3;
    bool ok = false;
};

static void FaceInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<FaceInitCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "face init: ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::faceInit(ctx->detPath, ctx->lmPath, ctx->gaPath, ctx->threads);
}

static void FaceInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<FaceInitCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

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

static napi_value ReleaseFace(napi_env env, napi_callback_info) {
    ocr_bridge::faceRelease();
    return nullptr;
}

// ★ 人脸识别画面开关：false 时工作线程不再入队，已画连线框 1.5s 过期自动消失
static napi_value SetFaceEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1; napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    if (argc > 0) napi_get_value_bool(env, args[0], &enabled);
    SetFaceEnabled(enabled);
    return nullptr;
}

// ★ HUD 人脸信息（JSON：count/faces[gender,age,beauty,x,y,w,h]，500ms 轮询）
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
static napi_value ReleaseSrNapi(napi_env env, napi_callback_info info) {
    ocr_bridge::srRelease();
    return nullptr;
}

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

static napi_value DetachRecordSurfaceNapi(napi_env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->DetachRecordSurface();
    return nullptr;
}

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

static napi_value ClearSurfaceNapi(napi_env env, napi_callback_info) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    if (gPipeline) gPipeline->ClearSurface();
    return nullptr;
}

// ★ USB 摄像头数字变焦（照抄 Android CameraUVC.setZoomScale：1.0~5.0，中心裁剪放大）
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
struct OcrAsyncCtx {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string modelDir;
    bool ok = false;
};

static void OcrInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<OcrAsyncCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "ocr bridge load failed, see OcrBridge logs");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::init(ctx->modelDir);
}

static void OcrInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<OcrAsyncCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

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

static napi_value ReleaseOcr(napi_env env, napi_callback_info) {
    ocr_bridge::release();
    return nullptr;
}

static napi_value SetOcrEnabledNapi(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    bool enabled = false;
    if (argc > 0) napi_get_value_bool(env, args[0], &enabled);
    ocr_bridge::setEnabled(enabled);
    return nullptr;
}

static napi_value GetOcrTextNapi(napi_env env, napi_callback_info) {
    std::string text = ocr_bridge::getResult();
    napi_value r = nullptr;
    napi_create_string_utf8(env, text.c_str(), text.size(), &r);
    return r;
}

// ★ OCR 识别进度（done/total），供 HUD"识别中"徽标显示（不再画在画面上）
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

static void OcrInitPathsExecute(napi_env env, void *data) {
    auto *ctx = static_cast<OcrInitPathsCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::initPaths(ctx->detPath, ctx->clsPath, ctx->recPath, ctx->keysPath);
}

static void OcrInitPathsComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<OcrInitPathsCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

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

static void TableInitExecute(napi_env env, void *data) {
    auto *ctx = static_cast<TableInitCtx *>(data);
    if (!ocr_bridge::load()) {
        OH_LOG_ERROR(LOG_APP, "table init: ocr bridge load failed");
        ctx->ok = false;
        return;
    }
    ctx->ok = ocr_bridge::initTable(ctx->modelPath);
}

static void TableInitComplete(napi_env env, napi_status status, void *data) {
    auto *ctx = static_cast<TableInitCtx *>(data);
    napi_value result = nullptr;
    napi_get_boolean(env, ctx->ok, &result);
    napi_resolve_deferred(env, ctx->deferred, result);
    napi_delete_async_work(env, ctx->work);
    delete ctx;
}

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
        {"releaseSr",            nullptr, ReleaseSrNapi,        nullptr, nullptr, nullptr, napi_default, nullptr},
        {"initFace",             nullptr, InitFace,             nullptr, nullptr, nullptr, napi_default, nullptr},
        {"releaseFace",          nullptr, ReleaseFace,          nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setFaceEnabled",       nullptr, SetFaceEnabledNapi,   nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getFaceInfo",          nullptr, GetFaceInfoNapi,      nullptr, nullptr, nullptr, napi_default, nullptr},
        {"recognizeTable",       nullptr, RecognizeTable,       nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module entryModule = {
    .nm_version = 1, .nm_flags = 0, .nm_filename = nullptr,
    .nm_register_func = Init, .nm_modname = "entry",
    .nm_priv = nullptr, .reserved = { 0 },
};
extern "C" __attribute__((constructor)) void RegisterEntryModule() { napi_module_register(&entryModule); }