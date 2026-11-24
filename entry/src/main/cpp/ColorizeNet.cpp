// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ ColorizeNet —— AI 黑白照片上色引擎（libocr.so 内部实现，ColorizeNet.cpp）    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】DDColor-L（ICCV 2023, Apache-2.0）onnxruntime CPU 推理：            ║
// ║   黑白/棕褐色老照片 → 自然彩色。输入为"等亮度中性灰"（Lab a=b=0 的灰，       ║
// ║   即逐像素亮度 gamma 编码），模型预测 CIE Lab 的 a/b 色度平面，              ║
// ║   再与原图亮度 L 合成彩色。模型仅决定"什么颜色"，细节纹理完全保留原图。      ║
// ║                                                                              ║
// ║ 【模型】ddcolor.onnx（约 241MB，PastVu 生产同款 Android ARM 导出，           ║
// ║   sha256 固定校验）：输入 "grey" [1,1,512,512] float32，                    ║
// ║   输出 "ab" [1,2,512,512] float32（Lab a/b 单位，D65 白点，与 OpenCV         ║
// ║   CV_32F Lab 约定一致）。I/O 名与尺寸在 init 时运行时探测，不硬编码依赖。     ║
// ║                                                                              ║
// ║ 【推理流程 colorize_rgba】（与 PastVuApp PhotoColorizer.kt 逐步对齐）        ║
// ║   ① RGBA→BGR float[0,1]，记录原图（用于最终 L 通道与细节）                  ║
// ║   ② 512×512 降采样 → 逐像素亮度灰（sRGB EOTF 线性化→Y→gamma 编码）          ║
// ║   ③ ORT 推理得 a/b 平面（512²）                                             ║
// ║   ④ 去整体色偏：a/b 各减去 0.8×均值（DDColor 整片偏暖黄，去掉大头保留色彩）  ║
// ║   ⑤ a/b 放大回原尺寸 + 原图 L 引导联合双边边缘对齐（消物体边缘溢色）        ║
// ║   ⑥ Lab→BGR→RGBA（alpha 恒 255），输出分辨率与输入相同                      ║
// ║                                                                              ║
// ║ 【线程/内存】napi 异步工作线程经 OcrBridge dlsym 调用；g_color_mutex         ║
// ║   串行化 init/colorize/release。CPU fp32 推理（GPU/NPU 半精度会数值溢出      ║
// ║   出品红/青斑，PastVu 实测结论），intra 线程数调用方指定；                   ║
// ║   allow_spinning=0 + 关闭 CPU arena：闲置线程休眠省电、峰值内存约省 200MB，  ║
// ║   换来个位数百分比的速度损失（241MB 大模型值得）。                            ║
// ║                                                                              ║
// ║ 【ABI】color_init / color_is_ready / color_release / colorize_rgba           ║
// ║   （声明见 ocr_c_api.h，libentry 经 OcrBridge dlsym 调用）                   ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <hilog/log.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// hilog 域/标签（log.h 默认定义为 0/NULL，先 undef 再定防宏重定义警告）
#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "ColorNet"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace {

    // ───────── 会话与环境 ─────────

    // ★ colorEnv：进程级 Ort::Env 单例（onnxruntime 要求 Env 生命周期覆盖全部
    //   Session；WARNING 级日志避免移动端刷屏）
    Ort::Env &colorEnv() {
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "colornet");
        return env;
    }

    // ★ 全局会话与状态：mutex 串行化生命周期与推理（一次仅一张照片上色，
    //   两份 241MB 模型内存不可接受）；ready 用 atomic 供上层无锁快查
    std::mutex g_color_mutex;
    std::unique_ptr<Ort::Session> g_color_session;
    std::atomic<bool> g_color_ready{false};
    std::string g_color_in_name;
    std::string g_color_out_name;
    int g_color_side = 512;  // 模型输入边长（init 时从输入 shape 读取，动态则 512）

    // ───────── sRGB ↔ 线性光 LUT（与 PastVu 逐位一致）─────────

    // ★ kToLinear：sRGB 字节 → 线性光（0..1）。sRGB EOTF：≤0.04045 线性段，
    //   否则 ((v+0.055)/1.055)^2.4；查表避免逐像素 pow
    const float *toLinearLut() {
        static std::vector<float> lut = [] {
            std::vector<float> v(256);
            for (int i = 0; i < 256; i++) {
                const float c = (float)i / 255.0f;
                v[i] = (c <= 0.04045f) ? c / 12.92f
                                       : std::pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return v;
        }();
        return lut.data();
    }

    // ★ gammaEncode：线性光（0..1）→ sRGB 编码值（0..1）。Lab a=b=0 的"中性灰"
    //   三通道相等且等于亮度 Y 的 gamma 编码——模型输入即此值（单通道）
    inline float gammaEncode(float y) {
        return (y <= 0.0031308f) ? y * 12.92f
                                  : 1.055f * std::pow(y, 1.0f / 2.4f) - 0.055f;
    }

    /* ★ jointBilateralAb：a/b 联合双边精修（5×5 窗口，引导图 = 原图 L）。
     *   纯双线性放大后的色度在物体边缘会"溢色"（背景色渗进前景 2~4px，
     *   发丝/树叶/建筑轮廓一圈模糊色边）。本步对放大后的 a/b 做边缘感知平滑：
     *   权重 = 空间高斯(σs≈1.8，5×5 预计算) × 值域高斯(|ΔL| 查 101 项 LUT，
     *   σL=6，Lab L∈[0,100])——平坦区色度平滑去噪，L 突变处权重骤降，
     *   色度突变对齐亮度突变，色彩边界干净锐利。
     *   复杂度 O(W·H·25)（960 长边约 70 万像素 ≈ 1700 万次查表，数十毫秒级） */
    void jointBilateralAb(const cv::Mat &lab, cv::Mat &planeA, cv::Mat &planeB) {
        const int w = planeA.cols;
        const int h = planeA.rows;
        // 值域权重 LUT：|ΔL| 0..100 → exp(-d²/(2·σL²))，σL=6
        static const std::vector<float> rangeLut = [] {
            std::vector<float> v(101);
            for (int i = 0; i <= 100; i++) {
                v[i] = std::exp(-(float)(i * i) / 72.0f);
            }
            return v;
        }();
        // 空间权重 5×5（σs=1.8）：exp(-(dx²+dy²)/6.48)，窗口越远权重越低
        static const std::vector<float> spatial = [] {
            std::vector<float> v(25);
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    v[(dy + 2) * 5 + (dx + 2)] = std::exp(-(float)(dx * dx + dy * dy) / 6.48f);
                }
            }
            return v;
        }();
        // L 取自 lab 通道 0（CV_32F，0..100）；逐像素加权平均写独立输出
        cv::Mat outA(h, w, CV_32FC1);
        cv::Mat outB(h, w, CV_32FC1);
        for (int y = 0; y < h; y++) {
            const int y0 = std::max(0, y - 2);
            const int y1 = std::min(h - 1, y + 2);
            for (int x = 0; x < w; x++) {
                const float lcx = lab.ptr<float>(y)[x * 3];
                float wSum = 0.0f;
                float vA = 0.0f;
                float vB = 0.0f;
                for (int qy = y0; qy <= y1; qy++) {
                    const float *lRow = lab.ptr<float>(qy);
                    const float *aRow = planeA.ptr<float>(qy);
                    const float *bRow = planeB.ptr<float>(qy);
                    const int x0 = std::max(0, x - 2);
                    const int x1 = std::min(w - 1, x + 2);
                    for (int qx = x0; qx <= x1; qx++) {
                        float dl = lRow[qx * 3] - lcx;
                        if (dl < 0.0f) dl = -dl;
                        int di = (int)dl;
                        if (di > 100) di = 100;
                        const float wgt =
                                spatial[(qy - y + 2) * 5 + (qx - x + 2)] * rangeLut[di];
                        wSum += wgt;
                        vA += wgt * aRow[qx];
                        vB += wgt * bRow[qx];
                    }
                }
                outA.ptr<float>(y)[x] = vA / (wSum + 1e-6f);
                outB.ptr<float>(y)[x] = vB / (wSum + 1e-6f);
            }
        }
        outA.copyTo(planeA);
        outB.copyTo(planeB);
    }

}  // namespace

// ───────── C ABI 实现 ─────────

extern "C" {

/* 加载 DDColor-L ONNX 模型（约 241MB，加载耗时数秒，napi 层已异步化） */
// ★ color_init：建会话（fp32 CPU、线程数、图优化全开、关 arena/关自旋）→
//   探测 I/O 名与输入边长（shape 最后一维；动态尺寸回退 512）→ 置 ready。
//   不做 warmup：DDColor 512² 推理单次 10s 级，warmup 徒增等待；
//   首次真实推理的 JIT/内存池开销由调用方的 loading UI 覆盖
int color_init(const char *onnxPath, int numThreads) {
    std::lock_guard<std::mutex> lock(g_color_mutex);
    g_color_session.reset();
    g_color_ready.store(false);
    if (!onnxPath || !*onnxPath) {
        LOGE("color_init: path empty");
        return 0;
    }
    if (numThreads <= 0) numThreads = 4;
    try {
        Ort::SessionOptions opt;
        opt.SetIntraOpNumThreads(numThreads);
        opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // 闲置线程休眠不自旋：省电、不与相机/OCR 线程互相争抢
        opt.AddConfigEntry("session.intra_op.allow_spinning", "0");
        // 关闭 CPU arena：每步推理完内存即归还，峰值约省 200MB（PastVu 实测结论）
        opt.DisableCpuMemArena();
        g_color_session = std::make_unique<Ort::Session>(colorEnv(), onnxPath, opt);

        // I/O 名与输入边长探测（不硬编码 "grey"/512：换导出版本零改动）
        Ort::AllocatorWithDefaultOptions alloc;
        {
            auto inName = g_color_session->GetInputNameAllocated(0, alloc);
            auto outName = g_color_session->GetOutputNameAllocated(0, alloc);
            g_color_in_name = inName.get();
            g_color_out_name = outName.get();
        }
        int side = 512;
        {
            auto info = g_color_session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            // 输入须为 [N,C,H,W] 且 C=1（单通道亮度灰）；H/W 固定则取实测值
            if (shape.size() != 4 || (shape[1] > 0 && shape[1] != 1)) {
                LOGE("color_init: bad input shape rank/channels");
                g_color_session.reset();
                return 0;
            }
            if (shape[3] > 0) side = (int)shape[3];
            if (shape[2] > 0 && shape[2] != side) {
                LOGE("color_init: non-square input unsupported");
                g_color_session.reset();
                return 0;
            }
        }
        g_color_side = side;
        g_color_ready.store(true);
        LOGI("color model ready: %{public}s io=%{public}s/%{public}s side=%{public}d threads=%{public}d",
             onnxPath, g_color_in_name.c_str(), g_color_out_name.c_str(), side, numThreads);
        return 1;
    } catch (const Ort::Exception &e) {
        LOGE("color_init Ort::Exception: %{public}s", e.what());
    } catch (...) {
        LOGE("color_init: unknown exception");
    }
    g_color_session.reset();
    g_color_ready.store(false);
    return 0;
}

// ★ color_is_ready：无锁快查（napi 层恢复流水线按需探测）
int color_is_ready(void) {
    return g_color_ready.load() ? 1 : 0;
}

// ★ color_release：会话全量释放（回收 241MB 级模型内存）
void color_release(void) {
    std::lock_guard<std::mutex> lock(g_color_mutex);
    g_color_session.reset();
    g_color_ready.store(false);
    LOGI("color model released");
}

/* RGBA → AI 上色 → RGBA（分辨率不变；in/out 等尺寸，允许原地同缓冲）。
 * 返回 1 成功 / 0 失败（调用方保留原图继续后续修复步骤） */
// ★ colorize_rgba：六步流水线（文件头【推理流程】逐步对应）。
//   失败统一返回 0：上色是"增强"而非"必需"，任何异常都不应中断整条修复链
int colorize_rgba(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH) {
    if (!g_color_ready.load() || !in || !out) return 0;
    if (inW < 8 || inH < 8) return 0;
    // 输出尺寸必须与输入相同（上色不改分辨率）
    if (outW != inW || outH != inH) {
        LOGE("colorize: bad out size %{public}dx%{public}d", outW, outH);
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_color_mutex);
    if (!g_color_session) return 0;

    try {
        const int side = g_color_side;
        const float *lut = toLinearLut();

        // ① RGBA → BGR float [0,1]（原图，保留全部细节）
        cv::Mat rgba(inH, inW, CV_8UC4, const_cast<uint8_t *>(in));
        cv::Mat bgr;
        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
        cv::Mat bgrF;
        bgr.convertTo(bgrF, CV_32F, 1.0 / 255.0);

        // ② 降采样 side×side → 逐像素亮度灰（模型输入）：
        //    线性化→亮度 Y=0.2126R+0.7152G+0.0722B→gamma 编码（Lab a=b=0 的灰）
        cv::Mat small;
        cv::resize(bgr, small, cv::Size(side, side), 0, 0, cv::INTER_AREA);
        std::vector<float> grey((size_t)side * side);
        for (int y = 0; y < side; y++) {
            const uint8_t *row = small.ptr<uint8_t>(y);
            float *dst = grey.data() + (size_t)y * side;
            for (int x = 0; x < side; x++) {
                const uint8_t *p = row + x * 3;
                const float yy = 0.2126729f * lut[p[2]] + 0.7151522f * lut[p[1]] +
                                 0.0721750f * lut[p[0]];
                dst[x] = gammaEncode(yy);
            }
        }

        // ③ ORT 推理：[1,1,side,side] → a/b 平面
        const int64_t inShape[4] = {1, 1, side, side};
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
                grey.data(), grey.size(), inShape, 4);
        const char *inNames[] = {g_color_in_name.c_str()};
        const char *outNames[] = {g_color_out_name.c_str()};
        auto outputs = g_color_session->Run(Ort::RunOptions{nullptr},
                                            inNames, &inTensor, 1, outNames, 1);
        if (outputs.empty() || !outputs[0].IsTensor()) {
            LOGE("colorize: no output");
            return 0;
        }
        auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        // 输出须为 [N,2,h,w]（a、b 两个平面）；h/w 与输入同尺寸或 1/4 尺寸均接受
        if (outShape.size() != 4 || outShape[1] != 2 || outShape[2] <= 0 || outShape[3] <= 0) {
            LOGE("colorize: bad out shape rank/channels");
            return 0;
        }
        const int abH = (int)outShape[2];
        const int abW = (int)outShape[3];
        const float *abData = outputs[0].GetTensorData<float>();

        // ④ 去整体色偏：a/b 各减 0.8×均值（DDColor 整片偏暖黄棕，PastVu 同系数；
        //   去掉大头保留场景色彩——灰白/天空回中性，物体色彩仍在）
        double sumA = 0.0, sumB = 0.0;
        const size_t abPx = (size_t)abH * abW;
        for (size_t i = 0; i < abPx; i++) {
            sumA += abData[i];
            sumB += abData[abPx + i];
        }
        const float meanA = (float)(sumA / (double)abPx);
        const float meanB = (float)(sumB / (double)abPx);
        cv::Mat planeA(abH, abW, CV_32FC1);
        cv::Mat planeB(abH, abW, CV_32FC1);
        float *pa = planeA.ptr<float>();
        float *pb = planeB.ptr<float>();
        for (size_t i = 0; i < abPx; i++) {
            pa[i] = abData[i] - 0.8f * meanA;
            pb[i] = abData[abPx + i] - 0.8f * meanB;
        }
        // ④b 轻度色度增益：a/b ×1.16 再钳 ±115——DDColor 输出整体偏"淡雅"，
        //     小幅增益让色彩鲜活而不艳俗（钳制防个别像素过饱和出"荧光色"）
        for (size_t i = 0; i < abPx; i++) {
            float a = pa[i] * 1.16f;
            float b = pb[i] * 1.16f;
            pa[i] = a < -115.0f ? -115.0f : (a > 115.0f ? 115.0f : a);
            pb[i] = b < -115.0f ? -115.0f : (b > 115.0f ? 115.0f : b);
        }

        // ⑤ a/b 双线性放大回原尺寸 → 以原图 L 为引导做联合双边边缘对齐
        //   （消除纯双线性放大的物体边缘"溢色"，见 jointBilateralAb 注释）→
        //   与原图 L 合成 Lab：OpenCV CV_32F Lab 约定 L∈[0,100]、a/b∈[-127,127]
        //   （D65 白点），与模型输出及 PastVu 手写 D65 转换一致，可无转换直拼
        cv::resize(planeA, planeA, cv::Size(inW, inH), 0, 0, cv::INTER_LINEAR);
        cv::resize(planeB, planeB, cv::Size(inW, inH), 0, 0, cv::INTER_LINEAR);
        cv::Mat lab;
        cv::cvtColor(bgrF, lab, cv::COLOR_BGR2Lab);  // L 通道取自原图（细节保留）
        jointBilateralAb(lab, planeA, planeB);
        {
            // 仅 a/b 两平面替换为模型结果，L 保留原图
            cv::Mat chans[3];
            cv::split(lab, chans);
            planeA.copyTo(chans[1]);
            planeB.copyTo(chans[2]);
            cv::merge(chans, 3, lab);
        }

        // ⑥ Lab → BGR → uint8 RGBA 输出（alpha 恒 255）
        cv::Mat outBgrF;
        cv::cvtColor(lab, outBgrF, cv::COLOR_Lab2BGR);
        cv::Mat outBgr;
        outBgrF.convertTo(outBgr, CV_8U, 255.0);
        cv::Mat outRgba(outH, outW, CV_8UC4);
        cv::cvtColor(outBgr, outRgba, cv::COLOR_BGR2RGBA);
        // 整块拷贝（支持 in==out 原地：outRgba 已是独立缓冲）
        memcpy(out, outRgba.data, (size_t)inW * inH * 4);
        return 1;
    } catch (const Ort::Exception &e) {
        LOGE("colorize Ort::Exception: %{public}s", e.what());
    } catch (...) {
        LOGE("colorize: unknown exception");
    }
    return 0;
}

}  // extern "C"
