// ★ 渲染管线核心实现：内置相机帧接收转换（NV21→RGBA/旋转/镜像）、
//   UVC 外部供帧路径、YOLO 异步推理调度、叠加绘制（框/水印/OCR）、
//   预览 surface 与录像直供推流、USB 画质增强（AI 超分细节注入架构）
#include "yuv_pipeline.h"
#include "OcrBridge.h"
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <libyuv/convert.h>
#include <sys/mman.h>
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>
#include <libyuv/row.h>
#include <libyuv/scale.h>
#include <libyuv/rotate_argb.h>
#include <pthread.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <libyuv/scale_argb.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "YuvPipeline"

extern bool NativeDetectOnRgba(const uint8_t *rgba, int w, int h, std::vector<Detection> &out);

static std::atomic<bool> g_yolo_enabled(true);

// ★ 识别框/标签画面显示开关（false 时检测照常，仅不画框和文字）
static std::atomic<bool> g_draw_boxes(true);
// ★ USB 画面增强（仅外部供帧路径生效）：
//   mode 0=关闭 1=非AI增强 2=AI细节注入超分(QuickSRNetMedium)
//   非AI：亮度 -50~+50 / 锐度 0~100（unsharp mask）/ 对比度+饱和度 + 固定基线锐化
//   AI：out = 原图 + β×(SR(小图)−双线性(小图))，β=细节强度滑条 0~100%
static std::atomic<int>  g_usb_mode(0);
static std::atomic<int>  g_usb_brightness(0);
static std::atomic<bool> g_usb_enhance(false);
static std::atomic<int>  g_usb_sharpness(0);
static std::atomic<int>  g_usb_sr_strength(100);
// ★ SR 细节网格降采样因子（原图宽/factor = SR 输入宽；factor 越大推理越快、
//   细节增量越粗）。按实测推理耗时自适应：慢了 +1 档（上限 8），快了回 4。
//   factor 与 SR 倍率(4) 对齐时 SR 输出与原图 1:1 像素对齐，零重采样
static std::atomic<int>  g_sr_factor(4);

// ───────── 运动 & 水深 ─────────
static pthread_mutex_t g_motion_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::vector<uint8_t> g_prev_gray;
static std::atomic<float> g_motion_level(0.0f);
static std::atomic<bool>  g_motion_enabled(false);

static pthread_mutex_t g_depth_mutex = PTHREAD_MUTEX_INITIALIZER;
static float g_depth_cm = 0.0f;
static std::atomic<bool>  g_depth_enabled(false);
static std::atomic<bool>  g_is_water(false);
// ★ 入水→触底积分模型参数：重量/浑浊度决定下沉速度（用户在弹窗里设置）
static std::atomic<float> g_depth_weight_g(30.0f);   // 摄像头重量(g)
static std::atomic<int>   g_depth_turbidity(1);      // 浑浊度: 0清澈 1微浑 2浑浊
// ★ 能耗模式可调节奏（PowerSheet 三档预设+滑条微调，立即生效）：
//   预览渲染节流 / YOLO 有目标与空闲推理间隔 / 水深采样间隔
static std::atomic<int> g_render_interval_ms(40);
static std::atomic<int> g_yolo_active_ms(300);
static std::atomic<int> g_yolo_idle_ms(800);
static std::atomic<int> g_sensor_interval_ms(250);
// 积分状态机：IDLE(未入水) → SINKING(下沉积分) → AT_BOTTOM(触底锁定)
static pthread_mutex_t g_depth_state_mutex = PTHREAD_MUTEX_INITIALIZER;
enum class DepthPhase { IDLE, SINKING, AT_BOTTOM };
static DepthPhase g_depth_phase = DepthPhase::IDLE;
static std::chrono::steady_clock::time_point g_depth_last_t{};
static float g_depth_integrated_cm = 0.0f;
static int g_depth_still_count = 0;      // 连续静止帧计数（触底判定）
static int g_depth_sink_samples = 0;     // 已下沉采样数（防入水瞬间误判触底）
static int g_depth_out_water_count = 0;  // 连续离水帧计数（被拉出水面复位）
// ★ 运动状态综合判定：入水需"运动激增或持续在水中"确认，防纯色误判；
//   触底需"运动静止 + 亮度稳定"双确认，防浑水无纹理噪声早锁
static int g_depth_water_run_count = 0;      // 连续在水中帧计数（入水确认）
static float g_depth_prev_brightness = -1.0f; // 上一帧平均亮度（触底亮度稳定性）
static std::vector<uint8_t> g_depth_prev_gray;
// ★ 入水确认时刻的亮度基准：下沉中亮度只降不升（光衰减），
//   亮度比入水时明显回升 = 被拉出/根本没下水（陆地晃动对着蓝色景物），据此复位
static float g_depth_entry_brightness = -1.0f;
static int g_depth_bright_rise_count = 0;    // 连续亮度回升采样数

// outWaterPx：采样点中"蓝显著高于红"的水体特征像素数（水下红光被吸收，蓝占优；
//   陆地无论明暗红蓝接近 → 晃动镜头不会误判入水）
static void analyzeFrame(const uint8_t* rgba, int w, int h,
                         std::vector<uint8_t>& grayOut,
                         float& outB, float& outG, float& outR,
                         int* outWaterPx = nullptr) {
    int targetW = 160, targetH = 120;
    grayOut.resize(targetW * targetH);
    int stepX = w / targetW, stepY = h / targetH;
    if (stepX <= 0) stepX = 1;
    if (stepY <= 0) stepY = 1;
    long long sumB = 0, sumG = 0, sumR = 0;
    int waterPx = 0;
    int count = targetW * targetH;
    for (int y = 0; y < targetH; ++y) {
        for (int x = 0; x < targetW; ++x) {
            const uint8_t* p = rgba + ((y * stepY) * w + (x * stepX)) * 4;
            const int pr = p[0], pg = p[1], pb = p[2];
            sumR += pr; sumG += pg; sumB += pb;
            // 水体特征：蓝比红高 8 以上，且不是纯黑噪点/高亮反光
            if (pb > pr + 8 && pb >= 40 && pb <= 200) waterPx++;
            grayOut[y * targetW + x] = (uint8_t)((pr * 299 + pg * 587 + pb * 114) / 1000);
        }
    }
    outR = (float)sumR / count;
    outG = (float)sumG / count;
    outB = (float)sumB / count;
    if (outWaterPx) *outWaterPx = waterPx;
}

static void update_motion_level(const uint8_t* rgba, int w, int h) {
    if (!g_motion_enabled.load()) {
        g_motion_level.store(0.0f);
        pthread_mutex_lock(&g_motion_mutex);
        g_prev_gray.clear();
        pthread_mutex_unlock(&g_motion_mutex);
        return;
    }
    std::vector<uint8_t> gray;
    float r, g, b;
    analyzeFrame(rgba, w, h, gray, b, g, r);
    pthread_mutex_lock(&g_motion_mutex);
    if (g_prev_gray.empty()) {
        g_prev_gray = gray;
        pthread_mutex_unlock(&g_motion_mutex);
        return;
    }
    int changed = 0;
    for (size_t i = 0; i < gray.size(); ++i) {
        if (std::abs((int)gray[i] - (int)g_prev_gray[i]) > 15) changed++;
    }
    g_prev_gray = gray;
    pthread_mutex_unlock(&g_motion_mutex);
    float level = (float)changed / gray.size();
    float old = g_motion_level.load();
    g_motion_level.store((level > old) ? (old * 0.4f + level * 0.6f) : (old * 0.7f + level * 0.3f));
}

// ★ 水深 = 入水→触底过程积分：
//   1) 入水检测（蓝绿占优/画面变暗）后开始计时积分；
//   2) 下沉速度 = 基础速度(重量越大越快、浑浊水密度/粘滞阻力越大越慢)，
//      数据线跟摄像头一起抛入 → 入水线随深度增长、阻力增大 → 持续减速；
//   3) 画面连续 ~1.5s 静止判定触底，锁定深度；被拉出水面后复位重新测量
static void update_depth_cm(const uint8_t* rgba, int w, int h) {
    if (!g_depth_enabled.load()) {
        pthread_mutex_lock(&g_depth_state_mutex);
        g_depth_phase = DepthPhase::IDLE;
        g_depth_integrated_cm = 0.0f;
        g_depth_still_count = 0; g_depth_sink_samples = 0; g_depth_out_water_count = 0;
        g_depth_water_run_count = 0; g_depth_prev_brightness = -1.0f;
        g_depth_entry_brightness = -1.0f; g_depth_bright_rise_count = 0;
        g_depth_prev_gray.clear();
        pthread_mutex_unlock(&g_depth_state_mutex);
        pthread_mutex_lock(&g_depth_mutex);
        g_depth_cm = 0.0f;
        pthread_mutex_unlock(&g_depth_mutex);
        g_is_water.store(false);
        return;
    }
    std::vector<uint8_t> gray;
    float r, g, b;
    int waterPx = 0;
    analyzeFrame(rgba, w, h, gray, b, g, r, &waterPx);
    float brightness = (r + g + b) / 3.0f;
    // ★ 水体判定 v2：采样像素"蓝显著高于红"占比过半才算水中
    //   （旧版"整体偏蓝 或 画面偏暗"在陆地暗光/室内晃动镜头会误判入水开始积分，
    //    亮着屏幕拍桌面都能出水深；水下红光被吸收红蓝差异大，陆地红蓝接近）
    const float waterRatio = (float)waterPx / (float)gray.size();
    bool isWater = waterRatio >= 0.55f;
    g_is_water.store(isWater);
    // 帧间运动比例：下沉中画面持续流动，触底后静止
    float motion01 = 0.0f;
    pthread_mutex_lock(&g_depth_state_mutex);
    if (!g_depth_prev_gray.empty() && g_depth_prev_gray.size() == gray.size()) {
        int changed = 0;
        for (size_t i = 0; i < gray.size(); ++i) {
            if (std::abs((int)gray[i] - (int)g_depth_prev_gray[i]) > 15) changed++;
        }
        motion01 = (float)changed / (float)gray.size();
    }
    g_depth_prev_gray = gray;
    auto now = std::chrono::steady_clock::now();
    switch (g_depth_phase) {
        case DepthPhase::IDLE:
            g_depth_integrated_cm = 0.0f;
            g_depth_still_count = 0; g_depth_sink_samples = 0; g_depth_out_water_count = 0;
            g_depth_bright_rise_count = 0;
            if (isWater) {
                // ★ 入水确认：连续 5 次（250ms/次 ≈1.25s）采样都呈水体特征才入水
                //   （去掉旧版"运动激增 OR"分支：陆地晃动镜头运动激增但非水体，不该触发）
                if (++g_depth_water_run_count >= 5) {
                    g_depth_phase = DepthPhase::SINKING;
                    g_depth_last_t = now;
                    g_depth_prev_brightness = brightness;
                    g_depth_entry_brightness = brightness;
                    g_depth_water_run_count = 0;
                    OH_LOG_INFO(LOG_APP, "[depth] WATER-IN confirmed: wr=%{public}.2f br=%{public}.1f",
                                waterRatio, brightness);
                }
            } else {
                g_depth_water_run_count = 0;
            }
            break;
        case DepthPhase::SINKING: {
            if (!isWater) {
                // 短暂离水（跃出/反光误判）容忍，连续离水才复位
                if (++g_depth_out_water_count >= 4) {
                    OH_LOG_INFO(LOG_APP, "[depth] SINKING reset: out-of-water x4, cm=%{public}.1f",
                                g_depth_integrated_cm);
                    g_depth_phase = DepthPhase::IDLE;
                    g_depth_integrated_cm = 0.0f;
                    g_depth_out_water_count = 0;
                    g_depth_water_run_count = 0;
                }
                break;
            }
            g_depth_out_water_count = 0;
            // ★ 亮度回升复位：下沉中光衰减亮度只降不升；亮度比入水时刻明显回升
            //   （连续 3 次 > 入水时 +18）说明被拉出水面或对着蓝天/蓝色景物晃动，复位
            if (g_depth_entry_brightness >= 0.0f &&
                brightness > g_depth_entry_brightness + 18.0f) {
                if (++g_depth_bright_rise_count >= 3) {
                    OH_LOG_INFO(LOG_APP, "[depth] SINKING reset: brightness rise (entry=%{public}.1f now=%{public}.1f)",
                                g_depth_entry_brightness, brightness);
                    g_depth_phase = DepthPhase::IDLE;
                    g_depth_integrated_cm = 0.0f;
                    g_depth_bright_rise_count = 0;
                    g_depth_water_run_count = 0;
                    break;
                }
            } else {
                g_depth_bright_rise_count = 0;
            }
            g_depth_sink_samples++;
            float dt = std::chrono::duration<float>(now - g_depth_last_t).count();
            g_depth_last_t = now;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 1.0f) dt = 1.0f;
            // ★ 积分仅当画面在变化：下沉中画面持续流动；手持静止/悬浮（画面不动）不累计深度
            if (motion01 > 0.005f) {
                // 基础下沉速度：越重越快；浑浊度越大（悬浮物多、密度/粘滞阻力大）越慢
                static const float TURB_FACTOR[3] = { 1.0f, 0.8f, 0.6f };
                float weight = g_depth_weight_g.load();
                int turb = g_depth_turbidity.load();
                float v0 = 55.0f * std::sqrt(std::max(1.0f, weight) / 30.0f) * TURB_FACTOR[turb];
                // 数据线一起抛入：入水线随深度增长、阻力增大 → 下沉持续减速
                float depthM = g_depth_integrated_cm / 100.0f;
                float v = v0 / (1.0f + 0.10f * depthM);
                g_depth_integrated_cm += v * dt;
                if (g_depth_integrated_cm > 3000.0f) g_depth_integrated_cm = 3000.0f;  // 上限 30m
            }
            // ★ 触底判定：运动基本停止 且 亮度稳定（|Δbright|<2.5）双确认，
            //   连续 6 帧（250ms/帧 ≈1.5s）且已下沉 ≥8 采样，防止浑水无纹理噪声早锁
            bool motionStill = motion01 < 0.02f;
            bool brightStable = (g_depth_prev_brightness >= 0.0f &&
                                 std::fabs(brightness - g_depth_prev_brightness) < 2.5f);
            g_depth_prev_brightness = brightness;
            if (motionStill && brightStable) g_depth_still_count++;
            else g_depth_still_count = 0;
            if (g_depth_still_count >= 6 && g_depth_sink_samples >= 8) {
                g_depth_phase = DepthPhase::AT_BOTTOM;
                OH_LOG_INFO(LOG_APP, "[depth] BOTTOM locked: cm=%{public}.1f samples=%{public}d br=%{public}.1f",
                            g_depth_integrated_cm, g_depth_sink_samples, brightness);
            }
            break;
        }
        case DepthPhase::AT_BOTTOM:
            // 触底锁定深度，不再积分；离开水面后复位重新测量
            if (!isWater) {
                if (++g_depth_out_water_count >= 4) {
                    OH_LOG_INFO(LOG_APP, "[depth] BOTTOM reset: out-of-water x4, cm=%{public}.1f",
                                g_depth_integrated_cm);
                    g_depth_phase = DepthPhase::IDLE;
                    g_depth_integrated_cm = 0.0f;
                    g_depth_out_water_count = 0;
                }
            } else {
                g_depth_out_water_count = 0;
            }
            break;
    }
    pthread_mutex_unlock(&g_depth_state_mutex);
    pthread_mutex_lock(&g_depth_mutex);
    g_depth_cm = g_depth_integrated_cm;
    pthread_mutex_unlock(&g_depth_mutex);
}

extern "C" {
    void SetYoloEnabled(bool enabled) { g_yolo_enabled.store(enabled); }
    // ★ 识别框/标签画面显示开关：检测/自动录像/朗读不受影响，仅控制画面绘制
    void SetDrawBoxesEnabled(bool enabled) {
        g_draw_boxes.store(enabled);
        OH_LOG_INFO(LOG_APP, "[yolo] drawBoxes=%{public}d", (int)enabled);
    }
    // ★ USB 画质增强模式 0~2：0 关 / 1 非AI(亮度+锐度+对比度饱和度) /
    //   2 AI超分(QuickSRNetMedium x4 + 精修锐化)；切换 AI 模式时重置自适应输入宽度
    void SetUsbEnhance(int mode, int brightness, int sharpness, int enhance, int strength) {
        if (mode < 0) mode = 0;
        if (mode > 2) mode = 2;
        if (brightness < -50) brightness = -50;
        if (brightness > 50) brightness = 50;
        if (sharpness < 0) sharpness = 0;
        if (sharpness > 100) sharpness = 100;
        if (strength < 0) strength = 0;
        if (strength > 100) strength = 100;
        const int prevMode = g_usb_mode.load();
        g_usb_mode.store(mode);
        g_usb_brightness.store(brightness);
        g_usb_sharpness.store(sharpness);
        g_usb_enhance.store(enhance != 0);
        g_usb_sr_strength.store(strength);
        // 切换/进入 AI 模式 → 重置降采样因子（随后按推理耗时自适应）
        if (mode != prevMode && mode == 2) {
            g_sr_factor.store(4);
        }
        OH_LOG_INFO(LOG_APP, "[usb] enhance mode=%{public}d bright=%{public}d sharp=%{public}d "
                    "quality=%{public}d srStrength=%{public}d",
                    mode, brightness, sharpness, (int)(enhance != 0), strength);
    }

    void SetMotionEnabled(bool enabled) {
        g_motion_enabled.store(enabled);
        OH_LOG_INFO(LOG_APP, "[motion] enabled=%{public}d", (int)enabled);
        if (!enabled) {
            g_motion_level.store(0.0f);
            pthread_mutex_lock(&g_motion_mutex); g_prev_gray.clear(); pthread_mutex_unlock(&g_motion_mutex);
        }
    }
    float GetMotionLevel() { return g_motion_level.load(); }
    void SetDepthEnabled(bool enabled) {
        g_depth_enabled.store(enabled);
        OH_LOG_INFO(LOG_APP, "[depth] enabled=%{public}d", (int)enabled);
        // ★ 开关切换/关闭时复位积分状态机，重新测量
        pthread_mutex_lock(&g_depth_state_mutex);
        g_depth_phase = DepthPhase::IDLE;
        g_depth_integrated_cm = 0.0f;
        g_depth_still_count = 0; g_depth_sink_samples = 0; g_depth_out_water_count = 0;
        g_depth_water_run_count = 0; g_depth_prev_brightness = -1.0f;
        g_depth_entry_brightness = -1.0f; g_depth_bright_rise_count = 0;
        g_depth_prev_gray.clear();
        pthread_mutex_unlock(&g_depth_state_mutex);
        pthread_mutex_lock(&g_depth_mutex);
        g_depth_cm = 0.0f;
        pthread_mutex_unlock(&g_depth_mutex);
        if (!enabled) g_is_water.store(false);
    }
    // ★ 用户设置：摄像头重量(g) + 水的浑浊度（0清澈/1微浑/2浑浊），影响下沉速度
    void SetDepthParams(float weightG, int turbidity) {
        if (weightG >= 1.0f && weightG <= 2000.0f) g_depth_weight_g.store(weightG);
        if (turbidity >= 0 && turbidity <= 2) g_depth_turbidity.store(turbidity);
    }
    // ★ 能耗模式：节奏参数统一入口（渲染节流/YOLO 推理间隔/水深采样），滑条拖动立即生效
    void SetPipelineTiming(int renderMs, int yoloActiveMs, int yoloIdleMs, int sensorMs) {
        if (renderMs >= 16 && renderMs <= 200) g_render_interval_ms.store(renderMs);
        if (yoloActiveMs >= 50 && yoloActiveMs <= 2000) g_yolo_active_ms.store(yoloActiveMs);
        if (yoloIdleMs >= 100 && yoloIdleMs <= 5000) g_yolo_idle_ms.store(yoloIdleMs);
        if (sensorMs >= 50 && sensorMs <= 2000) g_sensor_interval_ms.store(sensorMs);
    }
    float GetDepthCm() {
        pthread_mutex_lock(&g_depth_mutex);
        float d = g_depth_cm;
        pthread_mutex_unlock(&g_depth_mutex);
        return d;
    }
}

static YuvPipeline *gActivePipeline = nullptr;

YuvPipeline::~YuvPipeline() {
    Stop();
    {
        std::lock_guard<std::mutex> lock(previewWindowMutex_);
        if (previewWindow_) {
            OH_NativeWindow_DestroyNativeWindow(previewWindow_);
            previewWindow_ = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lock(ftMutex_);
        if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
        if (ftLib_)  { FT_Done_FreeType(ftLib_); ftLib_ = nullptr; }
    }
}

void YuvPipeline::SetRotation(int degrees) {
    degrees = ((degrees % 360) + 360) % 360;
    if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) {
        OH_LOG_WARN(LOG_APP, "SetRotation: invalid %{public}d, fallback to 0", degrees);
        degrees = 0;
    }
    rotationDegrees_ = degrees;
    UpdateDisplaySize();
}

// ★ 前置摄像头镜像：对齐 Android 平台行为（前摄预览默认水平镜像/自拍镜像视图）。
//   ImageReceiver 输出不会自动镜像，需在渲染侧手动翻转
void YuvPipeline::SetMirror(bool mirror) {
    mirror_.store(mirror);
    OH_LOG_INFO(LOG_APP, "SetMirror: %{public}d", (int)mirror);
}

void YuvPipeline::UpdateDisplaySize() {
    if (rotationDegrees_ == 90 || rotationDegrees_ == 270) {
        displayWidth_ = height_;
        displayHeight_ = width_;
    } else {
        displayWidth_ = width_;
        displayHeight_ = height_;
    }
    if (displayWidth_ > 0 && displayHeight_ > 0) {
        rgbaBuf_.resize((size_t)displayWidth_ * displayHeight_ * 4);
    }
}

bool YuvPipeline::CreateReceiver(int width, int height, std::string &outSurfaceId) {
    width_ = width; height_ = height;
    tempBuf_.resize((size_t)width * height * 4);
    UpdateDisplaySize();

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        latestResults_.clear();
    }

    OH_ImageReceiverOptions *options = nullptr;
    OH_ImageReceiverOptions_Create(&options);
    Image_Size size;
    size.width = static_cast<uint32_t>(width);
    size.height = static_cast<uint32_t>(height);
    OH_ImageReceiverOptions_SetSize(options, size);
    OH_ImageReceiverOptions_SetCapacity(options, 8);
    OH_ImageReceiverNative_Create(options, &receiver_);
    OH_ImageReceiverOptions_Release(options);
    uint64_t surfaceId = 0;
    OH_ImageReceiverNative_GetReceivingSurfaceId(receiver_, &surfaceId);
    outSurfaceId = std::to_string(surfaceId);
    return true;
}

void YuvPipeline::Start() {
    if (!receiver_ || running_) return;
    gActivePipeline = this;

    OH_ImageReceiverNative_On(receiver_, [](OH_ImageReceiverNative *) {
        if (gActivePipeline) gActivePipeline->OnImageArrival();
    });
    running_ = true;

    inferRunning_.store(true);
    inferThread_ = std::thread(&YuvPipeline::InferenceLoop, this);

    OH_LOG_INFO(LOG_APP, "Pipeline started (async inference)");
}

void YuvPipeline::Stop() {
    running_ = false;

    // ★ 先停 SR 工作线程（可能在长推理中，join 等它退出）
    srRunning_.store(false);
    srCv_.notify_all();
    if (srThread_.joinable()) {
        srThread_.join();
    }
    srBusy_.store(false);

    inferRunning_.store(false);
    inferCV_.notify_all();
    if (inferThread_.joinable()) {
        inferThread_.join();
    }

    if (receiver_) {
        OH_ImageReceiverNative_Off(receiver_);
    }

    std::lock_guard<std::mutex> cbLock(callbackMutex_);

    ClearSurface();
    DetachRecordSurface();

    if (receiver_) {
        OH_ImageReceiverNative_Release(receiver_);
        receiver_ = nullptr;
    }
    if (gActivePipeline == this) gActivePipeline = nullptr;

    OH_LOG_INFO(LOG_APP, "Pipeline stopped (fully released)");
}

void YuvPipeline::SetPreviewWindow(OHNativeWindow *win) {
    std::lock_guard<std::mutex> lock(previewWindowMutex_);
    if (previewWindow_ && previewWindow_ != win) {
        OH_NativeWindow_DestroyNativeWindow(previewWindow_);
    }
    previewWindow_ = win;
    if (win) {
        OH_NativeWindow_NativeWindowHandleOpt(win, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
        OH_NativeWindow_NativeWindowHandleOpt(win, SET_BUFFER_GEOMETRY, displayWidth_, displayHeight_);
    }
}

void YuvPipeline::SetYoloEnabled(bool enabled) {
    yoloEnabled_.store(enabled);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        latestResults_.clear();
    }
}

void YuvPipeline::SetClassNames(const std::vector<std::string> &names) {
    std::lock_guard<std::mutex> lock(classNamesMutex_);
    classNames_ = names;
}

void YuvPipeline::LoadFont(const std::string &path) {
    std::lock_guard<std::mutex> lock(ftMutex_);
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    if (ftLib_)  { FT_Done_FreeType(ftLib_); ftLib_ = nullptr; }

    if (path.empty()) { OH_LOG_WARN(LOG_APP, "font path empty"); return; }
    if (FT_Init_FreeType(&ftLib_)) {
        OH_LOG_ERROR(LOG_APP, "FT_Init_FreeType failed");
        return;
    }
    if (FT_New_Face(ftLib_, path.c_str(), 0, &ftFace_)) {
        OH_LOG_ERROR(LOG_APP, "FT_New_Face failed: %{public}s", path.c_str());
        FT_Done_FreeType(ftLib_); ftLib_ = nullptr;
        return;
    }
    if (FT_HAS_MULTIPLE_MASTERS(ftFace_)) {
        FT_MM_Var *mm = nullptr;
        if (FT_Get_MM_Var(ftFace_, &mm) == 0 && mm) {
            if (mm->num_namedstyles > 0) {
                std::vector<FT_Fixed> coords(mm->num_axis);
                for (FT_UInt a = 0; a < mm->num_axis; ++a)
                    coords[a] = mm->namedstyle[0].coords[a];
                FT_Set_Var_Design_Coordinates(ftFace_, mm->num_axis, coords.data());
            }
            FT_Done_MM_Var(ftLib_, mm);
        }
    }
}



void YuvPipeline::RequestInference(const uint8_t *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    size_t sz = (size_t)w * h * 4;
    {
        std::lock_guard<std::mutex> lk(inferMutex_);
        
        // ★ 核心提速 3：如果上一帧还在排队没被取走，直接丢弃本帧！
        // 这样永远保证处理的都是最新画面，绝不会有老画面排队导致的延迟感
        if (inferPending_) {
            return;
        }
        
        inferFrame_.resize(sz);
        memcpy(inferFrame_.data(), rgba, sz);
        inferW_ = w;
        inferH_ = h;
        inferPending_ = true;
    }
    inferCV_.notify_one();
}
void YuvPipeline::InferenceLoop() {
    while (inferRunning_.load()) {
        std::vector<uint8_t> frame;
        int w = 0, h = 0;

        {
            std::unique_lock<std::mutex> lk(inferMutex_);
            inferCV_.wait(lk, [&] {
                return inferPending_ || !inferRunning_.load();
            });
            if (!inferRunning_.load()) break;

            w = inferW_;
            h = inferH_;
            
            // ★ 使用 swap 交换，避免 vector 内部频繁的申请/释放内存
            frame.swap(inferFrame_);
            inferFrame_.clear();
            inferPending_ = false;
        }

        if (frame.empty() || w <= 0 || h <= 0) continue;
        
        // 如果关闭了直接跳过推理，不浪费性能
        if (!yoloEnabled_.load()) continue; 

        std::vector<Detection> dets;
        if (NativeDetectOnRgba(frame.data(), w, h, dets)) {
            std::vector<DetectItem> items;
            items.reserve(dets.size());
            for (const auto &d : dets) {
                items.push_back({d.x, d.y, d.w, d.h, d.score, d.label});
            }
            StoreResults(items);
            inferCount_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
void YuvPipeline::StoreResults(const std::vector<DetectItem> &items) {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    
    // 如果此时 YOLO 已经被关闭，则直接丢弃本次结果并清空
    if (!yoloEnabled_.load()) {
        latestResults_.clear();
        return;
    }
    
    latestResults_ = items;
    if (!items.empty()) {
        lastNonEmptyResultsTime_ = std::chrono::steady_clock::now();
    }
}

std::vector<DetectItem> YuvPipeline::GetLatestResults() {
    std::lock_guard<std::mutex> lock(resultsMutex_);
    return latestResults_;
}

// ★ 懒快照：直接在相机互斥锁下拷贝当前帧（return by value）。
//   旧版每帧无条件整帧拷贝 snapshotBuf_（1080p 下 8MB×25fps=200MB/s 纯浪费 memcpy），
//   而唯一消费方是"拍照"（CaptureLatestFrame，低频操作）——改为按需拷贝零损耗
std::vector<uint8_t> YuvPipeline::GetLatestRgba() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    return rgbaBuf_;
}

void YuvPipeline::RequestCleanSnapshot() {
    {
        // 清掉上一次未消费的旧帧，防止等待方拿到过期画面
        std::lock_guard<std::mutex> lock(cleanSnapMutex_);
        cleanSnapReady_ = false;
    }
    cleanSnapRequested_.store(true);
}

bool YuvPipeline::WaitCleanSnapshot(int timeoutMs, std::vector<uint8_t> &out,
                                    int &w, int &h) {
    std::unique_lock<std::mutex> lock(cleanSnapMutex_);
    if (!cleanSnapCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return cleanSnapReady_; })) {
        return false;
    }
    out = cleanSnapBuf_;
    cleanSnapReady_ = false;
    w = displayWidth_;
    h = displayHeight_;
    return !out.empty();
}

void YuvPipeline::GetFrameSize(int &w, int &h) {
    w = displayWidth_;
    h = displayHeight_;
}

void YuvPipeline::OnImageArrival() {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (!running_ || !receiver_) return;

    // ★ ReadLatestImage 一次取最新帧（框架内部丢弃积压旧帧）：
    //   此前"ReadNextImage 读到失败为止"的排空循环每帧必产生一次失败读帧，
    //   触发 Bufferqueue "no dirty buffer" + ImageCode "Bad data" E 级刷屏（30fps×3 行），
    //   且失败读帧本身有队列锁开销。事件回调里必有新帧，ReadLatestImage 不会失败
    OH_ImageNative *image = nullptr;
    if (OH_ImageReceiverNative_ReadLatestImage(receiver_, &image) != IMAGE_SUCCESS || image == nullptr) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - lastRenderTime_).count();
    if (elapsedMs < g_render_interval_ms.load()) {
        OH_ImageNative_Release(image);
        return;
    }
    lastRenderTime_ = now;

    ProcessFrame(image);
    OH_ImageNative_Release(image);
}

void YuvPipeline::ProcessFrame(OH_ImageNative *image) {
    uint32_t *typesPtr = nullptr;
    size_t typeSize = 0;
    Image_ErrorCode err = OH_ImageNative_GetComponentTypes(image, &typesPtr, &typeSize);
    if (err != IMAGE_SUCCESS || typeSize == 0) return;

    OH_NativeBuffer *yBuf = nullptr;
    if (OH_ImageNative_GetByteBuffer(image, 0, &yBuf) != IMAGE_SUCCESS || yBuf == nullptr) return;

    int32_t yStride = 0;
    OH_ImageNative_GetRowStride(image, 0, &yStride);
    if (yStride <= 0) yStride = width_;

    size_t yBufSize = 0;
    OH_ImageNative_GetBufferSize(image, 0, &yBufSize);

    void *yPtr = nullptr;
    OH_NativeBuffer_Map(yBuf, &yPtr);
    if (yPtr == nullptr) return;

    const uint8_t *yPlane = static_cast<const uint8_t *>(yPtr);
    const uint8_t *uvPlane = nullptr;
    int32_t uvStride = yStride;
    OH_NativeBuffer *uvBuf = nullptr;
    void *uvPtr = nullptr;

    size_t nv21FullSize = (size_t)yStride * height_ * 3 / 2;
    bool singleBuffer = (yBufSize >= nv21FullSize);

    if (singleBuffer) {
        // ★ 高度对齐补位布局（如 1080 行补齐到 1088）：UV 平面实际起始于
        //   yStride×对齐高，按 yStride×height_ 会读进 Y 平面末尾 → 颜色错乱。
        //   完整 NV21 buffer 的 UV 起始 = 总大小×2/3；取较大者且要求按 stride
        //   对齐，兼容紧凑（1440x1440）与补位（1920x1080→1088）两种布局
        size_t uvOffset = (size_t)yStride * height_;
        size_t uvFromTotal = yBufSize / 3 * 2;
        if (uvFromTotal > uvOffset && uvFromTotal % (size_t)yStride == 0) {
            uvOffset = uvFromTotal;
        }
        uvPlane = yPlane + uvOffset;
    } else if (typeSize >= 2) {
        if (OH_ImageNative_GetByteBuffer(image, 1, &uvBuf) == IMAGE_SUCCESS && uvBuf) {
            OH_ImageNative_GetRowStride(image, 1, &uvStride);
            OH_NativeBuffer_Map(uvBuf, &uvPtr);
            if (uvPtr) uvPlane = static_cast<const uint8_t *>(uvPtr);
        }
    }

    if (!uvPlane) {
        OH_NativeBuffer_Unmap(yBuf);
        return;
    }

    libyuv::NV21ToARGB(yPlane, yStride, uvPlane, uvStride,
                       tempBuf_.data(), width_ * 4, width_, height_);

    // ★ 手写 2M 像素交换循环（1080p 实测 ~15-25ms，占帧预算 1/3）换成
    //   libyuv SIMD 原地交换（~2-4ms）：FOURCC_ARGB(mem BGRA) → mem RGBA，
    //   ARGBShuffleRow_C 注释明确支持 in-place
    libyuv::ARGBToBGRA(tempBuf_.data(), width_ * 4,
                       tempBuf_.data(), width_ * 4, width_, height_);

    if (uvPtr) OH_NativeBuffer_Unmap(uvBuf);
    OH_NativeBuffer_Unmap(yBuf);

    if (rotationDegrees_ == 0) {
        memcpy(rgbaBuf_.data(), tempBuf_.data(), (size_t)width_ * height_ * 4);
    } else {
        libyuv::RotationMode mode = libyuv::kRotate0;
        if (rotationDegrees_ == 90)       mode = libyuv::kRotate90;
        else if (rotationDegrees_ == 180) mode = libyuv::kRotate180;
        else if (rotationDegrees_ == 270) mode = libyuv::kRotate270;

        libyuv::ARGBRotate(tempBuf_.data(), width_ * 4,
                           rgbaBuf_.data(), displayWidth_ * 4,
                           width_, height_, mode);
    }

    // ★ 前置摄像头镜像：旋转到正立后再水平翻转（ARGBMirror 不支持原地，经 tempBuf_ 中转）
    if (mirror_.load()) {
        libyuv::ARGBMirror(rgbaBuf_.data(), displayWidth_ * 4,
                           tempBuf_.data(), displayWidth_ * 4,
                           displayWidth_, displayHeight_);
        memcpy(rgbaBuf_.data(), tempBuf_.data(),
               (size_t)displayWidth_ * displayHeight_ * 4);
    }

    ProcessCommonTail();
}

void YuvPipeline::ProcessCommonTail() {
    // OCR：在绘制任何叠加内容（YOLO 框/标签、时间戳水印）之前，
    // 用干净的相机帧触发识别，避免把自绘水印识别成文字
    ocr_bridge::maybeTrigger(rgbaBuf_.data(), displayWidth_, displayHeight_);

    // ★ 表格识别干净帧抓取点：与 OCR 触发同点（叠加绘制前），
    //   请求方（napi recognizeTable）在条件变量上同步等待
    if (cleanSnapRequested_.exchange(false)) {
        std::lock_guard<std::mutex> lock(cleanSnapMutex_);
        cleanSnapBuf_ = rgbaBuf_;
        cleanSnapReady_ = true;
        cleanSnapCv_.notify_all();
    }

    static int s_motion_tick = 0;
    if ((++s_motion_tick & 1) == 0) {
        update_motion_level(rgbaBuf_.data(), displayWidth_, displayHeight_);
    }

    static std::chrono::steady_clock::time_point s_last_depth_time;
    auto now_depth = std::chrono::steady_clock::now();
    // ★ 水深采样间隔可配（能耗模式）：触底判定与入水积分的时间分辨率
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now_depth - s_last_depth_time).count() >= g_sensor_interval_ms.load()) {
        s_last_depth_time = now_depth;
        update_depth_cm(rgbaBuf_.data(), displayWidth_, displayHeight_);
    }

    int dispW = (rotationDegrees_ == 90 || rotationDegrees_ == 270) ? height_ : width_;
    int dispH = (rotationDegrees_ == 90 || rotationDegrees_ == 270) ? width_  : height_;

    auto now = std::chrono::steady_clock::now();
    // ★ 对齐 Android 端节奏：有目标/空闲两档推理间隔（能耗模式可调）；
    //   RequestInference 忙时丢帧不积压，处理的永远是最新画面
    int64_t intervalMs = g_yolo_active_ms.load();
    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        if (latestResults_.empty()) intervalMs = g_yolo_idle_ms.load();
    }

    // 如果启用了才触发推理
    if (yoloEnabled_.load() &&
        dispW > 0 && dispH > 0 &&
        !rgbaBuf_.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastInferTime_).count() >= intervalMs) {
        lastInferTime_ = now;
        RequestInference(rgbaBuf_.data(), dispW, dispH);
    }

    DrawOverlayOnRgba();
    DrawOcrOverlay();

    // ★ 用户要求：录像画面必须包含时间水印、识别框等叠加内容——
    //   推流挪到叠加绘制之后（此前推的是干净帧）。BGRAToARGB 通道修正
    //   对水印/框同样生效，颜色保持正确
    PushToRecordSurface(rgbaBuf_.data(), displayWidth_, displayHeight_);

    // （懒快照：不再每帧整帧拷贝，拍照时 GetLatestRgba 按需取）
    PushToSurface();
}

bool YuvPipeline::UpdateFrameSize(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == width_ && height == height_) return true;
    OH_LOG_INFO(LOG_APP, "UpdateFrameSize: %{public}dx%{public}d -> %{public}dx%{public}d "
                 "(rotation=%{public}d preserved)",
                 width_, height_, width, height, rotationDegrees_);
    width_ = width;
    height_ = height;
    tempBuf_.resize((size_t)width * height * 4);
    UpdateDisplaySize();

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        latestResults_.clear();
    }
    // ★ 尺寸变化后同步预览 surface 的 buffer geometry（同 ConfigureExternal）
    {
        std::lock_guard<std::mutex> lock(previewWindowMutex_);
        if (previewWindow_ && displayWidth_ > 0 && displayHeight_ > 0) {
            OH_NativeWindow_NativeWindowHandleOpt(previewWindow_,
                SET_BUFFER_GEOMETRY, displayWidth_, displayHeight_);
        }
    }
    return true;
}

bool YuvPipeline::ConfigureExternal(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    width_ = width;
    height_ = height;
    rotationDegrees_ = 0;
    tempBuf_.resize((size_t)width * height * 4);
    UpdateDisplaySize();

    {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        latestResults_.clear();
    }
    // ★ 尺寸变化后同步预览 surface 的 buffer geometry，
    //   否则 PushPixelsToSurface 按旧 buffer 尺寸 CenterCrop（16:9 被裁掉 2/3 宽度）
    {
        std::lock_guard<std::mutex> lock(previewWindowMutex_);
        if (previewWindow_ && displayWidth_ > 0 && displayHeight_ > 0) {
            OH_NativeWindow_NativeWindowHandleOpt(previewWindow_,
                SET_BUFFER_GEOMETRY, displayWidth_, displayHeight_);
        }
    }
    return true;
}

void YuvPipeline::StartExternal() {
    if (running_) return;
    gActivePipeline = this;
    running_ = true;

    inferRunning_.store(true);
    inferThread_ = std::thread(&YuvPipeline::InferenceLoop, this);

    // ★ AI 超分工作线程（USB 画质增强 mode 2/3；无任务时在条件变量上休眠）
    srBusy_.store(false);
    srRunning_.store(true);
    srThread_ = std::thread(&YuvPipeline::SrWorkerLoop, this);

    OH_LOG_INFO(LOG_APP, "Pipeline started (external/UVC mode, async inference)");
}

// ★ USB 摄像头数字变焦（照抄 Android UVCPreview.cpp nativeSetZoomScale）：
//   1.0~5.0 clamp；>1 时逐帧中心裁剪 1/zoom 再放大回原尺寸，
//   时间水印/识别/运动/水深均在变焦后的画面上进行
void YuvPipeline::SetZoomScale(float scale) {
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 5.0f) scale = 5.0f;
    zoomScale_.store(scale);
}

void YuvPipeline::SetLocationText(const std::string &text) {
    std::lock_guard<std::mutex> lock(locationMutex_);
    locationText_ = text;
}

void YuvPipeline::SetLocationBottomRatio(float ratio) {
    if (ratio < 0.4f) ratio = 0.4f;
    if (ratio > 1.0f) ratio = 1.0f;
    std::lock_guard<std::mutex> lock(locationMutex_);
    locBottomRatio_ = ratio;
}

// ★ unsharp mask 锐化（非AI增强"锐度"滑条）：out = clamp(orig + amount*(orig - blur3x3))
//   分离两趟 3tap 盒滤波，边界 clamp；缓冲 static 复用（仅相机线程调用）
static void sharpenRgba(uint8_t* rgba, int w, int h, float amount) {
    if (amount <= 0.001f || w < 3 || h < 3) return;
    static std::vector<uint16_t> hbuf;
    static std::vector<uint8_t> blur;
    const size_t n = (size_t)w * h;
    hbuf.resize(n * 3);
    blur.resize(n * 4);
    // 水平 3tap：hsum[x] = p[x-1]+p[x]+p[x+1]
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + (size_t)y * w * 4;
        uint16_t* hr = hbuf.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int xm = x > 0 ? x - 1 : 0;
            const int xp = x < w - 1 ? x + 1 : w - 1;
            hr[x * 3 + 0] = (uint16_t)(row[xm * 4] + row[x * 4] + row[xp * 4]);
            hr[x * 3 + 1] = (uint16_t)(row[xm * 4 + 1] + row[x * 4 + 1] + row[xp * 4 + 1]);
            hr[x * 3 + 2] = (uint16_t)(row[xm * 4 + 2] + row[x * 4 + 2] + row[xp * 4 + 2]);
        }
    }
    // 垂直 3tap → blur = 9 邻域均值
    for (int y = 0; y < h; ++y) {
        const int ym = y > 0 ? y - 1 : 0;
        const int yp = y < h - 1 ? y + 1 : h - 1;
        const uint16_t* r0 = hbuf.data() + (size_t)ym * w * 3;
        const uint16_t* r1 = hbuf.data() + (size_t)y * w * 3;
        const uint16_t* r2 = hbuf.data() + (size_t)yp * w * 3;
        uint8_t* d = blur.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            d[0] = (uint8_t)((r0[x * 3] + r1[x * 3] + r2[x * 3]) / 9);
            d[1] = (uint8_t)((r0[x * 3 + 1] + r1[x * 3 + 1] + r2[x * 3 + 1]) / 9);
            d[2] = (uint8_t)((r0[x * 3 + 2] + r1[x * 3 + 2] + r2[x * 3 + 2]) / 9);
        }
    }
    const int amt1000 = (int)(amount * 1000.0f);
    uint8_t* q = rgba;
    const uint8_t* b = blur.data();
    for (size_t i = 0; i < n; ++i, q += 4, b += 4) {
        for (int c = 0; c < 3; ++c) {
            int v = q[c] + ((q[c] - (int)b[c]) * amt1000) / 1000;
            q[c] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
        }
    }
}

// ★ Clarity 局部对比增强（粗尺度 unsharp，半径 2 的 5tap 分离盒滤波）：
//   与 sharpenRgba 的 3x3 细锐不同，clarity 提升中频对比（物体轮廓/纹理块
//   与邻域的明暗差），是肉眼"清晰感"最敏感的频段；两者叠加=专业修图
//   的 Clarity+Sharpen 双重锐化
static void clarityRgba(uint8_t* rgba, int w, int h, float amount) {
    if (amount <= 0.001f || w < 5 || h < 5) return;
    static std::vector<uint16_t> hbuf;
    static std::vector<uint8_t> blur;
    const size_t n = (size_t)w * h;
    hbuf.resize(n * 3);
    blur.resize(n * 4);
    // 水平 5tap：hsum[x] = p[x-2]+p[x-1]+p[x]+p[x+1]+p[x+2]
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgba + (size_t)y * w * 4;
        uint16_t* hr = hbuf.data() + (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            const int xm2 = x > 1 ? x - 2 : 0;
            const int xm1 = x > 0 ? x - 1 : 0;
            const int xp1 = x < w - 1 ? x + 1 : w - 1;
            const int xp2 = x < w - 2 ? x + 2 : w - 1;
            for (int c = 0; c < 3; ++c) {
                hr[x * 3 + c] = (uint16_t)(row[xm2 * 4 + c] + row[xm1 * 4 + c] +
                                           row[x * 4 + c] + row[xp1 * 4 + c] + row[xp2 * 4 + c]);
            }
        }
    }
    // 垂直 5tap → blur = 25 邻域均值
    for (int y = 0; y < h; ++y) {
        const int ym2 = y > 1 ? y - 2 : 0;
        const int ym1 = y > 0 ? y - 1 : 0;
        const int yp1 = y < h - 1 ? y + 1 : h - 1;
        const int yp2 = y < h - 2 ? y + 2 : h - 1;
        const uint16_t* r0 = hbuf.data() + (size_t)ym2 * w * 3;
        const uint16_t* r1 = hbuf.data() + (size_t)ym1 * w * 3;
        const uint16_t* r2 = hbuf.data() + (size_t)y * w * 3;
        const uint16_t* r3 = hbuf.data() + (size_t)yp1 * w * 3;
        const uint16_t* r4 = hbuf.data() + (size_t)yp2 * w * 3;
        uint8_t* d = blur.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, d += 4) {
            for (int c = 0; c < 3; ++c) {
                d[c] = (uint8_t)((r0[x * 3 + c] + r1[x * 3 + c] + r2[x * 3 + c] +
                                  r3[x * 3 + c] + r4[x * 3 + c]) / 25);
            }
        }
    }
    const int amt1000 = (int)(amount * 1000.0f);
    uint8_t* q = rgba;
    const uint8_t* b = blur.data();
    for (size_t i = 0; i < n; ++i, q += 4, b += 4) {
        for (int c = 0; c < 3; ++c) {
            int v = q[c] + ((q[c] - (int)b[c]) * amt1000) / 1000;
            q[c] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
        }
    }
}

// ★ USB 非AI画面增强（mode 1，仅外部供帧路径调用）：
//   亮度偏移 ±50 直接加；画质增强 = 对比度围绕中值拉伸 1.12 倍 + 饱和度围绕灰度基准推远 1.15 倍；
//   锐度 0~100 → unsharp mask 强度 0~1.6；
//   在缩放入 rgbaBuf_ 之后、叠加绘制之前执行 → 时间/位置水印与识别框不被增强污染
static void applyUsbEnhance(uint8_t* rgba, int w, int h) {
    // ★ mode 0 = 完全关闭（亮度/锐度/对比度饱和度全部不生效）
    if (g_usb_mode.load() == 0) return;
    const int bright = g_usb_brightness.load();
    const bool enhance = g_usb_enhance.load();
    const int sharp = g_usb_sharpness.load();
    if (bright == 0 && !enhance && sharp == 0) return;
    // ★ 分支外提：仅锐度开启时跳过逐像素循环（旧版白跑一遍空循环）
    if (bright != 0 || enhance) {
        const size_t n = (size_t)w * h;
        uint8_t* p = rgba;
        for (size_t i = 0; i < n; ++i, p += 4) {
            int r = p[0], g = p[1], b = p[2];
            if (enhance) {
                r = (r - 128) * 112 / 100 + 128;
                g = (g - 128) * 112 / 100 + 128;
                b = (b - 128) * 112 / 100 + 128;
                const int gray = (r * 299 + g * 587 + b * 114) / 1000;
                r = gray + (r - gray) * 115 / 100;
                g = gray + (g - gray) * 115 / 100;
                b = gray + (b - gray) * 115 / 100;
            }
            if (bright != 0) { r += bright; g += bright; b += bright; }
            p[0] = r < 0 ? 0 : (r > 255 ? 255 : (uint8_t)r);
            p[1] = g < 0 ? 0 : (g > 255 ? 255 : (uint8_t)g);
            p[2] = b < 0 ? 0 : (b > 255 ? 255 : (uint8_t)b);
        }
    }
    if (sharp > 0) {
        sharpenRgba(rgba, w, h, sharp / 100.0f * 1.6f);
    }
}

// ★ AI 超分工作线程（细节注入架构）：
//   等待 pending → SR 推理（重活）→ out = 原图 + β×(SR(小图)−双线性(小图))
//   → 双重锐化 → 用户滑条链 → 上屏 → 复位 busy。
//   相机线程只负责缩放入队，推理期间到达的帧直接丢弃，屏幕保持上一帧结果
void YuvPipeline::SrWorkerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lk(srMutex_);
        srCv_.wait(lk, [this] { return srPending_ || !srRunning_.load(); });
        if (!srRunning_.load()) break;
        if (!srPending_) continue;
        std::swap(srWorkIn_, srInPending_);
        std::swap(srWorkRaw_, srRawPending_);
        const int inW = srInW_, inH = srInH_;
        const int rawW = srRawW_, rawH = srRawH_;
        srPending_ = false;
        lk.unlock();

        auto t0 = std::chrono::steady_clock::now();
        // ★ 倍率由模型运行时探测（x2/x3/x4），不再固定 x4
        const int scale = ocr_bridge::srScale();
        if (scale < 2) {
            OH_LOG_WARN(LOG_APP, "[sr] scale not detected, keep plain frames");
            srBusy_.store(false);
            continue;
        }
        srOutBuf_.resize((size_t)inW * scale * inH * scale * 4);
        const bool ok = ocr_bridge::srUpscale(srWorkIn_.data(), inW, inH,
                                              srOutBuf_.data(), inW * scale, inH * scale);
        if (!ok) {
            OH_LOG_WARN(LOG_APP, "[sr] upscale failed, keep plain frames until ready");
            srBusy_.store(false);
            continue;
        }
        const int64_t inferMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // ★ 推理太慢 → 降采样因子 +1（SR 网格更小更快，上限 8）；快了逐步回 4
        {
            int f = g_sr_factor.load();
            if (inferMs > 1200 && f < 8) {
                g_sr_factor.store(f + 1);
                OH_LOG_INFO(LOG_APP, "[sr] slow(%{public}lldms) factor=%{public}d",
                            (long long)inferMs, f + 1);
            } else if (inferMs < 400 && f > 4) {
                g_sr_factor.store(f - 1);
            }
        }

        const int srW = inW * scale, srH = inH * scale;
        // ★ 同一低分辨率网格的双线性放大：细节增量的"普通放大"基线。
        //   delta = SR − bilinear 恰为模型学到的"超出插值的高频细节"
        srUpBuf_.resize((size_t)srW * srH * 4);
        libyuv::ARGBScale(srWorkIn_.data(), (size_t)inW * 4, inW, inH,
                          srUpBuf_.data(), (size_t)srW * 4, srW, srH,
                          libyuv::kFilterBilinear);

        // 与相机回调互斥，接管渲染
        std::lock_guard<std::mutex> cbLock(callbackMutex_);
        if (!running_ || rgbaBuf_.empty()) { srBusy_.store(false); continue; }
        const int W = displayWidth_, H = displayHeight_;
        if (W > 0 && H > 0) {
            // ① 基底 = 原始裁切帧（真实传感器细节，零损失）缩放到显示尺寸
            srBlendBuf_.resize((size_t)W * H * 4);
            if (rawW == W && rawH == H) {
                memcpy(srBlendBuf_.data(), srWorkRaw_.data(), (size_t)W * H * 4);
            } else {
                libyuv::ARGBScale(srWorkRaw_.data(), (size_t)rawW * 4, rawW, rawH,
                                  srBlendBuf_.data(), (size_t)W * 4, W, H,
                                  libyuv::kFilterBilinear);
            }
            // ② SR 输出与双线性基线对齐到显示尺寸（factor 与倍率对齐时零重采样）
            const uint8_t *srF = nullptr;
            const uint8_t *upF = nullptr;
            if (srW == W && srH == H) {
                srF = srOutBuf_.data();
                upF = srUpBuf_.data();
            } else {
                srDetailBuf_.resize((size_t)W * H * 4);
                srUpScaledBuf_.resize((size_t)W * H * 4);
                libyuv::ARGBScale(srOutBuf_.data(), (size_t)srW * 4, srW, srH,
                                  srDetailBuf_.data(), (size_t)W * 4, W, H,
                                  libyuv::kFilterBilinear);
                libyuv::ARGBScale(srUpBuf_.data(), (size_t)srW * 4, srW, srH,
                                  srUpScaledBuf_.data(), (size_t)W * 4, W, H,
                                  libyuv::kFilterBilinear);
                srF = srDetailBuf_.data();
                upF = srUpScaledBuf_.data();
            }
            // ③ 细节注入：out = 原图 + β×(SR − 双线性)，β = 细节强度滑条 0~100%
            const int strength = g_usb_sr_strength.load();
            memcpy(rgbaBuf_.data(), srBlendBuf_.data(), (size_t)W * H * 4);
            if (strength > 0) {
                uint8_t *d = rgbaBuf_.data();
                const size_t n = (size_t)W * H;
                for (size_t i = 0; i < n; ++i, d += 4, srF += 4, upF += 4) {
                    for (int c = 0; c < 3; ++c) {
                        int v = d[c] + ((int)srF[c] - (int)upF[c]) * strength / 100;
                        d[c] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
                    }
                }
            }
            // ④ 双重精修锐化：细锐（3x3 unsharp）+ Clarity（中频局部对比），
            //    肉眼"清晰感"最敏感的两个频段同时拉满
            sharpenRgba(rgbaBuf_.data(), W, H, 0.50f);
            clarityRgba(rgbaBuf_.data(), W, H, 0.35f);
            // ⑤ 用户滑条链：亮度/锐度/对比度饱和度对 AI 模式同样生效；叠加层在其后绘制不受污染
            applyUsbEnhance(rgbaBuf_.data(), W, H);
            ProcessCommonTail();
        }
        srBusy_.store(false);
    }
}

void YuvPipeline::ProcessExternalRgba(const uint8_t *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;

    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (!running_ || rgbaBuf_.empty()) return;

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastRenderTime_).count() < g_render_interval_ms.load()) {
        return;
    }
    lastRenderTime_ = now;

    // ★ 数字变焦：中心裁剪 1/zoom 区域（照抄 Android UVCPreview zoom 逻辑）
    const float zoom = zoomScale_.load();
    int sx = 0, sy = 0, sw = w, sh = h;
    if (zoom > 1.001f) {
        const int cropW = std::max(16, std::min(w, (int)(w / zoom)));
        const int cropH = std::max(16, std::min(h, (int)(h / zoom)));
        sx = (w - cropW) / 2;
        sy = (h - cropH) / 2;
        sw = cropW;
        sh = cropH;
    }

    const uint8_t *src = rgba + ((size_t)sy * w + sx) * 4;
    const int mode = g_usb_mode.load();

    // ★ AI 超分模式（mode 2，细节注入架构）：相机线程只做 Box 降采样入队
    //   （SR 细节网格 = 原图/factor），原始帧整帧备份作基底；推理在工作线程，
    //   推理忙时丢帧保持画面稳定（上一帧结果留在屏上）
    if (mode == 2 && ocr_bridge::srIsReady()) {
        if (srBusy_.load()) return;
        int f = g_sr_factor.load();
        if (f < 4) f = 4;
        if (f > 8) f = 8;
        int inW = (sw / f) & ~1;
        int inH = (sh / f) & ~1;
        if (inW < 32) inW = 32;
        if (inH < 32) inH = 32;
        {
            std::lock_guard<std::mutex> lk(srMutex_);
            srInPending_.resize((size_t)inW * inH * 4);
            libyuv::ARGBScale(src, w * 4, sw, sh,
                              srInPending_.data(), inW * 4, inW, inH,
                              libyuv::kFilterBox);
            // 原始裁切帧整帧备份：细节注入的基底（真实传感器细节零损失）
            srRawPending_.assign(src, src + (size_t)sw * sh * 4);
            srInW_ = inW; srInH_ = inH;
            srRawW_ = sw; srRawH_ = sh;
            srPending_ = true;
        }
        srBusy_.store(true);
        srCv_.notify_all();
        return;
    }

    // 普通路径（mode 0/1，或 AI 模式未就绪时兜底）
    if (zoom <= 1.001f && w == width_ && h == height_) {
        memcpy(rgbaBuf_.data(), rgba, (size_t)w * h * 4);
    } else {
        libyuv::ARGBScale(src, w * 4, sw, sh,
                          rgbaBuf_.data(), width_ * 4, width_, height_,
                          libyuv::kFilterBilinear);
    }

    // ★ 非AI基线锐化（mode≥1 固定生效，滑条全 0 也能看出增强开启；
    //   否则"非AI增强"与"关闭"肉眼无差别）。用户锐度滑条在其后叠加
    if (mode >= 1) {
        sharpenRgba(rgbaBuf_.data(), width_, height_, 0.45f);
    }

    // ★ USB 画面增强（亮度/锐度/画质）：作用于最终画面，叠加层在其后绘制不受影响
    applyUsbEnhance(rgbaBuf_.data(), width_, height_);

    ProcessCommonTail();
}

void YuvPipeline::DrawOverlayOnRgba() {
    uint8_t *rgba = rgbaBuf_.data();
    int W = displayWidth_, H = displayHeight_;
    if (W <= 0 || H <= 0) return;

    std::vector<DetectItem> dets;
    
    // 如果 YOLO 关了，强制不要读取旧数据，保证画面干净
    if (yoloEnabled_.load()) {
        std::lock_guard<std::mutex> lock(resultsMutex_);
        dets = latestResults_;
    } else {
        dets.clear();
    }

    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(classNamesMutex_);
        names = classNames_;
    }

    // ★ 照抄 Android yolo.h 的 19 色表，按 label % 19 取色
    static const uint8_t palette[19][3] = {
        {54, 67, 244}, {99, 30, 233}, {176, 39, 156}, {183, 58, 103}, {181, 81, 63},
        {243, 150, 33}, {244, 169, 3}, {212, 188, 0}, {136, 150, 0}, {80, 175, 76},
        {74, 195, 139}, {57, 220, 205}, {59, 235, 255}, {7, 193, 255}, {0, 152, 255},
        {34, 87, 255}, {72, 85, 121}, {158, 158, 158}, {139, 125, 96}
    };
    const int TH = std::max(2, W / 450);   // ★ 边框线宽随分辨率等比（1080p→2px，更细不糊）

    auto fillRect = [&](int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (w <= 0 || h <= 0) return;
        int x1 = std::min(x0 + w, W), y1 = std::min(y0 + h, H);
        int sx = std::max(x0, 0), sy = std::max(y0, 0);
        for (int y = sy; y < y1; ++y) {
            uint8_t *row = rgba + (y * W + sx) * 4;
            for (int x = sx; x < x1; ++x, row += 4) {
                if (a >= 255) { row[0]=r; row[1]=g; row[2]=b; row[3]=255; }
                else {
                    row[0] = (uint8_t)((row[0] * (255 - a) + r * a) / 255);
                    row[1] = (uint8_t)((row[1] * (255 - a) + g * a) / 255);
                    row[2] = (uint8_t)((row[2] * (255 - a) + b * a) / 255);
                    row[3] = 255;
                }
            }
        }
    };

    // ★ 识别框显示开关：关闭时不画框/标签（检测、自动录像、朗读不受影响）
    if (g_draw_boxes.load()) {
    for (size_t i = 0; i < dets.size(); ++i) {
        const auto &d = dets[i];
        int ci = d.label % 19;
        if (ci < 0) ci += 19;
        const uint8_t *col = palette[ci];
        int x = (int)(d.x * W);
        int y = (int)(d.y * H);
        int w = (int)(d.w * W);
        int h = (int)(d.h * H);
        if (w < 4 || h < 4) continue;

        fillRect(x, y, w, TH, col[0], col[1], col[2], 255);
        fillRect(x, y + h - TH, w, TH, col[0], col[1], col[2], 255);
        fillRect(x, y, TH, h, col[0], col[1], col[2], 255);
        fillRect(x + w - TH, y, TH, h, col[0], col[1], col[2], 255);

        std::string labelStr;
        if (d.label >= 0 && d.label < (int)names.size()) {
            char buf[160];
            snprintf(buf, sizeof(buf), "%s %.1f%%", names[d.label].c_str(), d.score * 100);
            labelStr = buf;
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%.1f%%", d.label, d.score * 100);
            labelStr = buf;
        }

        // ★ 对齐 Android draw_detections 缩小版：标签字号 W/40 等比（1080p→27px），
        //   高分辨率下标签/底色/间距同步放大，预览上视觉大小恒定
        const int labelFontSize = std::max(14, W / 40);
        const int labelPad = std::max(4, labelFontSize / 3);
        int textW = measureTextCN(labelStr, labelFontSize);
        int textH = labelFontSize;
        if (textW <= 0) textW = (int)labelStr.size() * labelFontSize * 2;

        int labelTop = y - textH - labelPad;
        int baselineY;
        if (labelTop < 2) {
            labelTop = y + 4;
            baselineY = labelTop + textH;
        } else {
            baselineY = y - labelPad / 2;
        }

        fillRect(x, labelTop, textW + labelPad * 2, textH + labelPad * 2, col[0], col[1], col[2], 255);

        // 自动计算文字颜色，完全对齐 Android 端逻辑
        uint8_t tr = 255, tg = 255, tb = 255;
        if (col[0] + col[1] + col[2] >= 381) {
            tr = 0; tg = 0; tb = 0;
        }

        drawTextCN(rgba, W, H, labelStr, x + labelPad, baselineY,
                   labelFontSize, tr, tg, tb);
    }
    }

    // ★ 时间水印：对齐 Android 原版 AbstractRender.getBitmap()——绿色(Color.GREEN)、
    // 无背景、左上角；字号按画面宽度等比缩放，满屏拉伸后与 HUD 文字(12vp)视觉一致
    static time_t s_lastSec = 0;
    static std::string s_lastLine;
    time_t now = time(nullptr);
    if (now != s_lastSec || s_lastLine.empty()) {
        s_lastSec = now;
        struct tm t;
        localtime_r(&now, &t);
        static const char *kWeekCn[] = {"周日","周一","周二","周三","周四","周五","周六"};
        // 注意：中文不能直接放在 % 转换符后面（printf 会把多字节首字节当转换符），
        // 一律用 %s 传入
        static const char *kYearCn = "年";
        static const char *kMonCn = "月";
        static const char *kDayCn = "日";
        char timeStr[128];
        snprintf(timeStr, sizeof(timeStr), "%04d%s%02d%s%02d%s %s %02d:%02d:%02d",
                 t.tm_year + 1900, kYearCn, t.tm_mon + 1, kMonCn, t.tm_mday, kDayCn,
                 kWeekCn[t.tm_wday], t.tm_hour, t.tm_min, t.tm_sec);
        s_lastLine = timeStr;
    }
    if (!s_lastLine.empty()) {
        // 屏宽 1080px、密度 3x：画面宽 W 拉伸满屏 → 字号 = W/40（缩小版，视觉 ~11vp）
        const int wmFont = std::max(14, W / 40);
        const int margin = 12;
        const int baselineY = margin + (int)(wmFont * 0.82f);
        drawTextCN(rgba, W, H, s_lastLine, margin, baselineY, wmFont, 0, 255, 0);
    }

    // ★ 位置水印：当前地理位置名，图片/录像底部居中（半透明黑底白字，任何画面可读）
    {
        std::string loc;
        {
            std::lock_guard<std::mutex> lock(locationMutex_);
            loc = locationText_;
        }
        if (!loc.empty()) {
            const int locFont = std::max(12, W / 44);
            int tw = measureTextCN(loc, locFont);
            if (tw <= 0) tw = (int)loc.size() * locFont;
            if (tw + 16 < W) {
                float ratio = 1.0f;
                {
                    std::lock_guard<std::mutex> lock(locationMutex_);
                    ratio = locBottomRatio_;
                }
                const int pad = 6;
                const int barW = tw + pad * 4;
                const int barH = locFont + pad * 2;
                const int barX = (W - barW) / 2;
                // ★ 按 ArkTS 传入的可见比例定位：预览底部被面板遮挡时上移，
                //   保证画面内可见；ratio=1 时退化为帧最底部
                const int barY = (int)(H * ratio) - barH - 10;
                fillRect(barX, barY, barW, barH, 0, 0, 0, 120);
                drawTextCN(rgba, W, H, loc, barX + pad * 2,
                           barY + pad + (int)(locFont * 0.85f), locFont, 255, 255, 255);
            }
        }
    }
}

// ───────── OCR 叠加：触发识别 + 画框 + 画文字 ─────────
static std::string truncateUtf8(const std::string &text, int maxChars) {
    int chars = 0;
    size_t bytes = 0;
    const unsigned char *p = (const unsigned char *) text.c_str();
    while (bytes < text.size() && chars < maxChars) {
        size_t len = 1;
        if ((*p & 0xF8) == 0xF0) len = 4;
        else if ((*p & 0xF0) == 0xE0) len = 3;
        else if ((*p & 0xE0) == 0xC0) len = 2;
        if (bytes + len > text.size()) break;
        bytes += len;
        p += len;
        chars++;
    }
    if (bytes >= text.size()) return text;
    return text.substr(0, bytes) + "…";
}

// ★ 对齐 Android fillRectAlpha：带 alpha 混合的实心矩形（供 OCR 标签/进度条底色用）
static void fillRectAlpha(uint8_t *rgba, int W, int H, int x0, int y0, int w, int h,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (w <= 0 || h <= 0) return;
    int x1 = std::min(x0 + w, W), y1 = std::min(y0 + h, H);
    int sx = std::max(x0, 0), sy = std::max(y0, 0);
    for (int y = sy; y < y1; ++y) {
        uint8_t *row = rgba + (y * W + sx) * 4;
        for (int x = sx; x < x1; ++x, row += 4) {
            if (a >= 255) { row[0]=r; row[1]=g; row[2]=b; row[3]=255; }
            else {
                row[0] = (uint8_t)((row[0] * (255 - a) + r * a) / 255);
                row[1] = (uint8_t)((row[1] * (255 - a) + g * a) / 255);
                row[2] = (uint8_t)((row[2] * (255 - a) + b * a) / 255);
                row[3] = 255;
            }
        }
    }
}

void YuvPipeline::DrawOcrOverlay() {
    if (displayWidth_ <= 0 || displayHeight_ <= 0 || rgbaBuf_.empty()) return;
    if (!ocr_bridge::isReady()) return;

    uint8_t *buf = rgbaBuf_.data();
    int W = displayWidth_, H = displayHeight_;
    ocr_bridge::drawLastBoxes(buf, W, H);

    constexpr int kMaxBoxes = 30;
    float pts[kMaxBoxes * 8];
    int n = ocr_bridge::getBoxes(pts, kMaxBoxes);
    if (n <= 0) return;

    // ★ 完全对齐 Android copyToSurface 的 OCR 绘制：
    //   5 色轮换（与 RapidOCR drawLastBoxes 同一套颜色）、字号 28、最多 30 字
    static const uint8_t kOcrColors[5][3] = {
        {0, 255, 0}, {0, 200, 255}, {255, 120, 0}, {255, 0, 255}, {0, 255, 255}
    };
    const int kOcrFontSize = std::max(12, W / 48);   // ★ 28@1440p 等比：高分辨率下文字同步放大（缩小版）
    const int kOcrPad = std::max(4, kOcrFontSize / 3);
    const int kMaxChars = 30;

    for (int i = 0; i < n; ++i) {
        std::string text = ocr_bridge::getBoxText(i);
        const bool hasText = !text.empty();
        const float *p = pts + i * 8;
        int minX = (int) p[0], minY = (int) p[1];
        for (int k = 1; k < 4; ++k) {
            minX = std::min(minX, (int) p[k * 2]);
            minY = std::min(minY, (int) p[k * 2 + 1]);
        }
        if (minX < 0) minX = 0;

        std::string label = hasText ? truncateUtf8(text, kMaxChars) : "…";
        int textW = measureTextCN(label, kOcrFontSize);
        int textH = kOcrFontSize;
        if (textW <= 0) textW = (int)label.size() * kOcrFontSize * 2;

        // Android：labelTop = minY - textH - 8，越界则放到框顶部内侧
        int labelTop = minY - textH - kOcrPad;
        if (labelTop < 0) labelTop = minY + 2;

        if (hasText) {
            // 已识别：彩色底 + 自动黑/白字
            const uint8_t *c = kOcrColors[i % 5];
            fillRectAlpha(buf, W, H, minX, labelTop, textW + kOcrPad * 2, textH + kOcrPad, c[0], c[1], c[2], 255);
            uint8_t tr = 255, tg = 255, tb = 255;
            if (c[0] + c[1] + c[2] > 380) { tr = 0; tg = 0; tb = 0; }
            drawTextCN(buf, W, H, label, minX + kOcrPad / 2, labelTop + textH, kOcrFontSize, tr, tg, tb);
        } else {
            // 待识别：灰底 "…"，深灰字
            fillRectAlpha(buf, W, H, minX, labelTop, textW + kOcrPad * 2, textH + kOcrPad, 180, 180, 180, 255);
            drawTextCN(buf, W, H, label, minX + kOcrPad / 2, labelTop + textH, kOcrFontSize, 60, 60, 60);
        }
    }
    // ★ 识别进度（识别中 N/M）不再画在画面上，改为通过 getOcrProgress
    //   供 ArkTS 侧 HUD 的"识别中"徽标显示
}

// ★ 核心：完全恢复与 Android 端一致的纯正抗锯齿渲染逻辑
void YuvPipeline::drawTextCN(uint8_t *rgba, int W, int H,
                             const std::string &text, int x, int y,
                             int fontHeight, uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lock(ftMutex_);
    if (!ftFace_) return;
    FT_Set_Pixel_Sizes(ftFace_, 0, fontHeight);

    int penX = x;
    const unsigned char *p = (const unsigned char *)text.c_str();
    const unsigned char *end = p + text.size();

    while (p < end) {
        FT_ULong code = 0;
        if (*p < 0x80) { code = *p++; }
        else if ((*p & 0xE0) == 0xC0) { code = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { code = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0) { code = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { p++; continue; }

        if (FT_Load_Char(ftFace_, code, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot slot = ftFace_->glyph;
        FT_Bitmap &bmp = slot->bitmap;

        int x0 = penX + slot->bitmap_left;
        int y0 = y - slot->bitmap_top;

        for (int row = 0; row < (int)bmp.rows; ++row) {
            int py = y0 + row;
            if (py < 0 || py >= H) continue;
            const unsigned char *src = bmp.buffer + row * bmp.pitch;
            for (int col = 0; col < (int)bmp.width; ++col) {
                int px = x0 + col;
                if (px < 0 || px >= W) continue;
                unsigned char alpha = src[col];
                if (alpha == 0) continue;
                uint8_t *dst = rgba + (py * W + px) * 4;
                float a = alpha / 255.0f;
                dst[0] = (uint8_t)(dst[0] * (1 - a) + r * a);
                dst[1] = (uint8_t)(dst[1] * (1 - a) + g * a);
                dst[2] = (uint8_t)(dst[2] * (1 - a) + b * a);
                dst[3] = 255;
            }
        }
        penX += slot->advance.x >> 6;
    }
}

int YuvPipeline::measureTextCN(const std::string &text, int fontHeight) {
    std::lock_guard<std::mutex> lock(ftMutex_);
    if (!ftFace_) return 0;
    FT_Set_Pixel_Sizes(ftFace_, 0, fontHeight);
    int w = 0;
    const unsigned char *p = (const unsigned char *)text.c_str();
    const unsigned char *end = p + text.size();
    while (p < end) {
        FT_ULong code = 0;
        if (*p < 0x80) { code = *p++; }
        else if ((*p & 0xE0) == 0xC0) { code = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { code = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0) { code = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        else { p++; continue; }
        if (FT_Load_Char(ftFace_, code, FT_LOAD_DEFAULT)) continue;
        w += ftFace_->glyph->advance.x >> 6;
    }
    return w;
}

bool YuvPipeline::PushPixelsToSurface(const uint8_t *src, int srcW, int srcH) {
    std::lock_guard<std::mutex> lock(previewWindowMutex_);
    if (!previewWindow_ || !src || srcW <= 0 || srcH <= 0) return false;

    OHNativeWindowBuffer *buffer = nullptr;
    int fenceFd = -1;
    int ret = OH_NativeWindow_NativeWindowRequestBuffer(previewWindow_, &buffer, &fenceFd);
    if (ret != 0 || buffer == nullptr) {
        return false;
    }

    BufferHandle *handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (handle == nullptr) {
        OH_NativeWindow_NativeWindowAbortBuffer(previewWindow_, buffer);
        return false;
    }

    int dstW = (int)handle->width;
    int dstH = (int)handle->height;
    int dstStride = (int)handle->stride;
    if (dstW <= 0 || dstH <= 0 || dstStride <= 0) {
        OH_NativeWindow_NativeWindowAbortBuffer(previewWindow_, buffer);
        return false;
    }

    size_t rgbaNeedSize = (size_t)dstStride * dstH;
    size_t nv12TotalSize = rgbaNeedSize + rgbaNeedSize / 2;

    size_t mapSize = (size_t)handle->size;
    if (mapSize == 0) {
        mapSize = (handle->format == 12) ? nv12TotalSize : rgbaNeedSize;
    }

    void *mapped = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (mapped == MAP_FAILED) {
        OH_NativeWindow_NativeWindowAbortBuffer(previewWindow_, buffer);
        return false;
    }

    const uint8_t *srcPixels = src;
    if (dstW != srcW || dstH != srcH) {
        size_t need = (size_t)dstW * dstH * 4;
        if (scaledRgbaBuf_.size() != need) scaledRgbaBuf_.resize(need);

        // CenterCrop 逻辑，保持不变以保证填满
        float srcAspect = (float)srcW / srcH;
        float dstAspect = (float)dstW / dstH;

        int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;

        if (srcAspect > dstAspect) {
            cropW = (int)(srcH * dstAspect);
            cropX = (srcW - cropW) / 2;
        } else {
            cropH = (int)(srcW / dstAspect);
            cropY = (srcH - cropH) / 2;
        }

        const uint8_t* cropPtr = src + (cropY * srcW + cropX) * 4;
        
        libyuv::ARGBScale(cropPtr, srcW * 4, cropW, cropH,
                          scaledRgbaBuf_.data(), dstW * 4, dstW, dstH,
                          libyuv::kFilterBilinear);
                          
        srcPixels = scaledRgbaBuf_.data();
    }

    bool wroteOk = false;
    if (handle->format == 12 && mapSize >= nv12TotalSize) {
        uint8_t *yPlane = (uint8_t *)mapped;
        uint8_t *uvPlane = yPlane + rgbaNeedSize;
        libyuv::ARGBToNV12(srcPixels, dstW * 4, yPlane, dstStride,
                           uvPlane, dstStride, dstW, dstH);
        wroteOk = true;
    } else if (mapSize >= rgbaNeedSize) {
        uint8_t *dst = (uint8_t *)mapped;
        if (dstStride == dstW * 4) {
            memcpy(dst, srcPixels, (size_t)dstW * dstH * 4);
        } else {
            for (int y = 0; y < dstH; ++y) {
                memcpy(dst + (size_t)y * dstStride,
                       srcPixels + (size_t)y * dstW * 4,
                       (size_t)dstW * 4);
            }
        }
        wroteOk = true;
    }

    munmap(mapped, mapSize);

    if (!wroteOk) {
        OH_NativeWindow_NativeWindowAbortBuffer(previewWindow_, buffer);
        return false;
    }

    Region region = {nullptr, 0};
    ret = OH_NativeWindow_NativeWindowFlushBuffer(previewWindow_, buffer, fenceFd, region);
    if (ret != 0) return false;
    return true;
}

bool YuvPipeline::PushToSurface() {
    return PushPixelsToSurface(rgbaBuf_.data(), displayWidth_, displayHeight_);
}

// ═══════════ 录像直供（AVRecorder 输入 surface）═══════════
// ★ 录像期间摄像头画面不中断：AVRecorder.getInputSurface 的消费者 surface
//   由 native 持有，现有帧路径（AI ImageReceiver / USB 外部供帧）每帧把
//   干净相机画面（已旋转/镜像/变焦、未叠加框与水印）转 NV12 直接喂编码器；
//   相机与预览全程不重配，对齐 Android 录像架构
bool YuvPipeline::AttachRecordSurface(uint64_t surfaceId, int width, int height) {
    std::lock_guard<std::mutex> lock(recordWindowMutex_);
    if (recordWindow_) {
        OH_NativeWindow_DestroyNativeWindow(recordWindow_);
        recordWindow_ = nullptr;
    }
    OHNativeWindow *win = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &win) != 0 || win == nullptr) {
        OH_LOG_ERROR(LOG_APP, "AttachRecordSurface: create window from %{public}llu failed",
                     (unsigned long long)surfaceId);
        return false;
    }
    recordWindow_ = win;
    // ★ 修复推流 0 帧：CreateNativeWindowFromSurfaceId 后窗口请求配置全为 0，
    //   不设置 geometry/format 时服务端按 0x0 分配 buffer 报 INVALID_ARGUMENTS(40001000)，
    //   客户端表现为 RequestBuffer 一直返回 50002000（buffer_queue.cpp SetupNewBufferLocked Fail）
    if (width > 0 && height > 0) {
        int geoRet = OH_NativeWindow_NativeWindowHandleOpt(recordWindow_, SET_BUFFER_GEOMETRY, width, height);
        int fmtRet = OH_NativeWindow_NativeWindowHandleOpt(recordWindow_, SET_FORMAT,
                                                           NATIVEBUFFER_PIXEL_FMT_YCBCR_420_SP);
        int usageRet = OH_NativeWindow_NativeWindowHandleOpt(recordWindow_, SET_USAGE,
            (uint64_t)(NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                       NATIVEBUFFER_USAGE_MEM_DMA));
        OH_LOG_INFO(LOG_APP, "AttachRecordSurface config %{public}dx%{public}d: geo=%{public}d fmt=%{public}d usage=%{public}d",
                    width, height, geoRet, fmtRet, usageRet);
    } else {
        OH_LOG_ERROR(LOG_APP, "AttachRecordSurface: invalid size %{public}dx%{public}d", width, height);
    }
    recordPushTotal_ = 0;
    recordPushOk_ = 0;
    recordFailLogged_ = 0;
    OH_LOG_INFO(LOG_APP, "AttachRecordSurface ok: %{public}llu", (unsigned long long)surfaceId);
    return true;
}

void YuvPipeline::DetachRecordSurface() {
    std::lock_guard<std::mutex> lock(recordWindowMutex_);
    if (recordWindow_) {
        OH_LOG_INFO(LOG_APP, "DetachRecordSurface: pushed %{public}d / %{public}d frames",
                    recordPushOk_, recordPushTotal_);
        OH_NativeWindow_DestroyNativeWindow(recordWindow_);
        recordWindow_ = nullptr;
    }
}

bool YuvPipeline::PushToRecordSurface(const uint8_t *src, int srcW, int srcH) {
    std::lock_guard<std::mutex> lock(recordWindowMutex_);
    if (!recordWindow_ || !src || srcW <= 0 || srcH <= 0) return false;

    // 诊断日志：每步返回码节流打印（首失败 + 之后每 25 次一报），
    //   成功时打印 buffer 参数并每 50 帧报一次进度
    recordPushTotal_++;

    // ★ 关键修复：给编码器真实采集时间戳（steady_clock ns，与音频采集同时基）。
    //   不设置时 buffer pts 恒为 0，合成器按帧序号伪造 PTS（实测每帧只 +11，
    //   50 帧实拍 3.3s 被压成 ~0.55s 视频）→ 播放快进数倍、与音轨完全错位。
    //   SET_UI_TIMESTAMP 作用于窗口上下一次 request/flush 的 buffer，
    //   必须在 RequestBuffer 之前设置
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    OH_NativeWindow_NativeWindowHandleOpt(recordWindow_, SET_UI_TIMESTAMP,
                                          (uint64_t)nowNs);

    OHNativeWindowBuffer *buffer = nullptr;
    int fenceFd = -1;
    int reqRet = OH_NativeWindow_NativeWindowRequestBuffer(recordWindow_, &buffer, &fenceFd);
    if (reqRet != 0 || buffer == nullptr) {
        if (recordFailLogged_ == 0 || recordFailLogged_ % 25 == 0) {
            OH_LOG_ERROR(LOG_APP, "RecPush: request buffer FAILED ret=%{public}d total=%{public}d",
                         reqRet, recordPushTotal_);
        }
        recordFailLogged_++;
        return false;
    }

    BufferHandle *handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (handle == nullptr) {
        if (recordFailLogged_ == 0 || recordFailLogged_ % 25 == 0) {
            OH_LOG_ERROR(LOG_APP, "RecPush: get handle null, total=%{public}d", recordPushTotal_);
        }
        recordFailLogged_++;
        OH_NativeWindow_NativeWindowAbortBuffer(recordWindow_, buffer);
        return false;
    }

    int dstW = (int)handle->width;
    int dstH = (int)handle->height;
    int dstStride = (int)handle->stride;
    if (dstW <= 0 || dstH <= 0 || dstStride <= 0) {
        if (recordFailLogged_ == 0 || recordFailLogged_ % 25 == 0) {
            OH_LOG_ERROR(LOG_APP, "RecPush: bad handle w=%{public}d h=%{public}d stride=%{public}d",
                         dstW, dstH, dstStride);
        }
        recordFailLogged_++;
        OH_NativeWindow_NativeWindowAbortBuffer(recordWindow_, buffer);
        return false;
    }

    // NV12 布局：Y 平面 stride*dstH + UV 交错平面 stride*dstH/2
    size_t ySize = (size_t)dstStride * dstH;
    size_t nv12Total = ySize + ySize / 2;
    size_t mapSize = (size_t)handle->size;
    if (mapSize < nv12Total) mapSize = nv12Total;

    void *mapped = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
    if (mapped == MAP_FAILED) {
        if (recordFailLogged_ == 0 || recordFailLogged_ % 25 == 0) {
            OH_LOG_ERROR(LOG_APP, "RecPush: mmap FAILED errno=%{public}d size=%{public}zu "
                         "fmt=%{public}d w=%{public}d h=%{public}d stride=%{public}d handleSize=%{public}zu",
                         errno, mapSize, handle->format, dstW, dstH, dstStride, (size_t)handle->size);
        }
        recordFailLogged_++;
        OH_NativeWindow_NativeWindowAbortBuffer(recordWindow_, buffer);
        return false;
    }

    if (recordPushOk_ == 0) {
        OH_LOG_INFO(LOG_APP, "RecPush: first buffer ok fmt=%{public}d %{public}dx%{public}d "
                     "stride=%{public}d handleSize=%{public}zu",
                     handle->format, dstW, dstH, dstStride, (size_t)handle->size);
    }

    // ★ R/B 通道修正：rgbaBuf_ 是内存序 RGBA（libyuv FOURCC_BGRA），而
    //   ARGBToNV12/ARGBScale 后续按 FOURCC_ARGB（mem BGRA）取色——直接喂会
    //   红蓝互换。先 SIMD 交换到 recordBgraBuf_（mem BGRA），再走缩放/NV12 转换
    const uint8_t *srcPixels = src;
    size_t srcBytes = (size_t)srcW * srcH * 4;
    if (recordBgraBuf_.size() < srcBytes) recordBgraBuf_.resize(srcBytes);
    libyuv::BGRAToARGB(src, srcW * 4, recordBgraBuf_.data(), srcW * 4, srcW, srcH);
    srcPixels = recordBgraBuf_.data();

    // 编码器 buffer 尺寸与画面不一致时 CenterCrop 补齐（正常情况下二者一致，直通）
    if (dstW != srcW || dstH != srcH) {
        size_t need = (size_t)dstW * dstH * 4;
        if (recordScaledBuf_.size() != need) recordScaledBuf_.resize(need);

        float srcAspect = (float)srcW / srcH;
        float dstAspect = (float)dstW / dstH;
        int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
        if (srcAspect > dstAspect) {
            cropW = (int)(srcH * dstAspect);
            cropX = (srcW - cropW) / 2;
        } else {
            cropH = (int)(srcW / dstAspect);
            cropY = (srcH - cropH) / 2;
        }
        const uint8_t *cropPtr = srcPixels + ((size_t)cropY * srcW + cropX) * 4;
        libyuv::ARGBScale(cropPtr, srcW * 4, cropW, cropH,
                          recordScaledBuf_.data(), dstW * 4, dstW, dstH,
                          libyuv::kFilterBilinear);
        srcPixels = recordScaledBuf_.data();
    }

    // VIDEO_SOURCE_TYPE_SURFACE_YUV → 编码器输入为 NV12
    uint8_t *yPlane = (uint8_t *)mapped;
    uint8_t *uvPlane = yPlane + ySize;
    libyuv::ARGBToNV12(srcPixels, dstW * 4,
                       yPlane, dstStride, uvPlane, dstStride, dstW, dstH);

    munmap(mapped, mapSize);

    Region region = {nullptr, 0};
    int flushRet = OH_NativeWindow_NativeWindowFlushBuffer(recordWindow_, buffer, fenceFd, region);
    recordPushOk_++;
    if (flushRet != 0) {
        if (recordFailLogged_ == 0 || recordFailLogged_ % 25 == 0) {
            OH_LOG_ERROR(LOG_APP, "RecPush: flush FAILED ret=%{public}d total=%{public}d",
                         flushRet, recordPushTotal_);
        }
        recordFailLogged_++;
        return false;
    }
    if (recordPushOk_ == 1 || recordPushOk_ % 50 == 0) {
        OH_LOG_INFO(LOG_APP, "RecPush: pushed %{public}d frames (total try %{public}d)",
                    recordPushOk_, recordPushTotal_);
    }
    return true;
}

void YuvPipeline::ClearSurface() {
    if (displayWidth_ <= 0 || displayHeight_ <= 0) return;
    size_t sz = (size_t)displayWidth_ * displayHeight_ * 4;
    if (blackBuf_.size() != sz) {
        blackBuf_.assign(sz, 0);
        for (size_t i = 3; i < sz; i += 4) blackBuf_[i] = 255;
    }
    PushPixelsToSurface(blackBuf_.data(), displayWidth_, displayHeight_);
}