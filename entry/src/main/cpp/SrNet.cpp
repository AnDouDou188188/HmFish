// ★ AI 超分引擎（libocr.so）：onnxruntime CPU 推理
//   支持模型（x2/x3/x4，1 通道 Y 或 3 通道 RGB，动态尺寸，I/O 名自动探测）：
//     quicksrnetmedium.onnx           （QuickSRNetMedium x4，3ch RGB，image/upscaled_image）
//   1ch Y 通路：Y=0.299R+0.587G+0.114B → 模型出 SR-Y → 输入 RGBA 双线性放大后做
//               luma-delta 合并（out = up + (y_sr - y_up)），色度取双线性、亮度取 SR。
//   ABI: sr_init/sr_is_ready/sr_release/sr_upscale_rgba/sr_scale（libentry 经 OcrBridge dlsym 调用）
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <hilog/log.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define LOG_DOMAIN 0x3200
#define LOG_TAG "SrNet"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace {

    Ort::Env &srEnv() {
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "srnet");
        return env;
    }

    std::mutex g_sr_mutex;
    std::unique_ptr<Ort::Session> g_sr_session;
    std::atomic<bool> g_sr_ready{false};
    std::atomic<int> g_sr_scale{0};      // 0=未就绪，否则 2/3/4
    std::atomic<int> g_sr_channels{3};   // 模型输入通道数 1 或 3
    int g_sr_threads = 4;
    std::string g_sr_in_name;
    std::string g_sr_out_name;

    // 复用缓冲（仅推理线程持有，避免每帧 25MB 分配）
    std::vector<float> g_sr_in_planar;    // RGB planar float
    std::vector<float> g_sr_out_planar;   // SR 输出 float
    std::vector<float> g_sr_y_delta;      // Y 通路 luma-delta

    // RGBA 双线性整数倍放大，直接写入 out
    void bilinearRgbaX(const uint8_t *in, int inW, int inH, uint8_t *out, int scale) {
        const int outW = inW * scale;
        const int outH = inH * scale;
        for (int oy = 0; oy < outH; ++oy) {
            const float fy = (oy + 0.5f) / scale - 0.5f;
            int y0 = (int)fy;
            float ty = fy - y0;
            if (y0 < 0) { y0 = 0; ty = 0; }
            if (y0 > inH - 2) { y0 = inH - 2; ty = 1; if (inH == 1) { y0 = 0; ty = 0; } }
            const int y1 = y0 + 1;
            const uint8_t *r0 = in + (size_t)y0 * inW * 4;
            const uint8_t *r1 = in + (size_t)y1 * inW * 4;
            uint8_t *dst = out + (size_t)oy * outW * 4;
            for (int ox = 0; ox < outW; ++ox) {
                const float fx = (ox + 0.5f) / scale - 0.5f;
                int x0 = (int)fx;
                float tx = fx - x0;
                if (x0 < 0) { x0 = 0; tx = 0; }
                if (x0 > inW - 2) { x0 = inW - 2; tx = 1; if (inW == 1) { x0 = 0; tx = 0; } }
                const int x1 = x0 + 1;
                const size_t i00 = (size_t)x0 * 4;
                const size_t i01 = (size_t)x1 * 4;
                for (int c = 0; c < 3; ++c) {
                    const float v = (1 - tx) * ((1 - ty) * r0[i00 + c] + ty * r1[i00 + c]) +
                                    tx * ((1 - ty) * r0[i01 + c] + ty * r1[i01 + c]) + 0.5f;
                    dst[(size_t)ox * 4 + c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
                dst[(size_t)ox * 4 + 3] = 255;
            }
        }
    }

}  // namespace

extern "C" {

/* 加载超分 ONNX 模型并探测 I/O 名 / 通道数 / 放大倍数。numThreads<=0 时用 4 */
int sr_init(const char *onnxPath, int numThreads) {
    std::lock_guard<std::mutex> lock(g_sr_mutex);
    g_sr_session.reset();
    g_sr_ready.store(false);
    g_sr_scale.store(0);
    if (!onnxPath || !*onnxPath) {
        LOGE("sr_init: path empty");
        return 0;
    }
    if (numThreads <= 0) numThreads = 4;
    g_sr_threads = numThreads;
    try {
        Ort::SessionOptions opt;
        opt.SetIntraOpNumThreads(numThreads);
        opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        g_sr_session = std::make_unique<Ort::Session>(srEnv(), onnxPath, opt);

        // I/O 名与输入通道数探测
        Ort::AllocatorWithDefaultOptions alloc;
        {
            auto inName = g_sr_session->GetInputNameAllocated(0, alloc);
            auto outName = g_sr_session->GetOutputNameAllocated(0, alloc);
            g_sr_in_name = inName.get();
            g_sr_out_name = outName.get();
        }
        int channels = 3;
        {
            auto info = g_sr_session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.size() == 4 && shape[1] > 0) channels = (int)shape[1];
        }
        if (channels != 1 && channels != 3) {
            LOGE("sr_init: bad input channels %{public}d", channels);
            g_sr_session.reset();
            return 0;
        }
        g_sr_channels.store(channels);

        // warmup 8x8 探测放大倍数
        const size_t px = 8 * 8;
        std::vector<float> dummy(px * channels, 0.5f);
        const int64_t inShape[4] = {1, channels, 8, 8};
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
            dummy.data(), px * channels, inShape, 4);
        const char *inNames[] = {g_sr_in_name.c_str()};
        const char *outNames[] = {g_sr_out_name.c_str()};
        auto outputs = g_sr_session->Run(Ort::RunOptions{nullptr},
                                         inNames, &inTensor, 1, outNames, 1);
        if (outputs.empty() || !outputs[0].IsTensor()) {
            LOGE("sr_init: warmup no output");
            g_sr_session.reset();
            return 0;
        }
        auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (outShape.size() != 4 || outShape[2] <= 0 || outShape[3] <= 0 ||
            outShape[2] % 8 != 0 || outShape[3] % 8 != 0) {
            LOGE("sr_init: bad warmup out shape");
            g_sr_session.reset();
            return 0;
        }
        const int scale = (int)(outShape[2] / 8);
        if (scale < 2 || scale > 4) {
            LOGE("sr_init: unsupported scale %{public}d", scale);
            g_sr_session.reset();
            return 0;
        }
        g_sr_scale.store(scale);
        g_sr_ready.store(true);
        LOGI("sr model ready: %{public}s io=%{public}s/%{public}s ch=%{public}d scale=%{public}d threads=%{public}d",
             onnxPath, g_sr_in_name.c_str(), g_sr_out_name.c_str(), channels, scale, numThreads);
        return 1;
    } catch (const Ort::Exception &e) {
        LOGE("sr_init Ort::Exception: %{public}s", e.what());
    } catch (...) {
        LOGE("sr_init: unknown exception");
    }
    g_sr_session.reset();
    g_sr_scale.store(0);
    return 0;
}

int sr_is_ready(void) {
    return g_sr_ready.load() ? 1 : 0;
}

/* 当前模型放大倍数；未就绪返回 0 */
int sr_scale(void) {
    return g_sr_scale.load();
}

void sr_release(void) {
    std::lock_guard<std::mutex> lock(g_sr_mutex);
    g_sr_session.reset();
    g_sr_ready.store(false);
    g_sr_scale.store(0);
    g_sr_in_planar.clear();
    g_sr_in_planar.shrink_to_fit();
    g_sr_out_planar.clear();
    g_sr_out_planar.shrink_to_fit();
    g_sr_y_delta.clear();
    g_sr_y_delta.shrink_to_fit();
    LOGI("sr released");
}

/* RGBA → xN 超分 → RGBA。
 * in: inW x inH RGBA，out: inW*scale x inH*scale RGBA（调用方保证容量）
 * 返回 1 成功 / 0 失败 */
int sr_upscale_rgba(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH) {
    const int scale = g_sr_scale.load();
    if (!g_sr_ready.load() || !in || !out || scale < 2) return 0;
    if (inW < 2 || inH < 2) return 0;
    if (outW != inW * scale || outH != inH * scale) {
        LOGE("sr_upscale: bad out size %{public}dx%{public}d, expect %{public}dx%{public}d",
             outW, outH, inW * scale, inH * scale);
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_sr_mutex);
    if (!g_sr_session) return 0;

    const size_t inPx = (size_t)inW * inH;
    const size_t outPx = (size_t)outW * outH;
    const int channels = g_sr_channels.load();

    if (channels == 1) {
        // ---- Y 通路 ----
        g_sr_in_planar.resize(inPx);
        {
            float *yp = g_sr_in_planar.data();
            const uint8_t *p = in;
            for (size_t i = 0; i < inPx; ++i, p += 4) {
                yp[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * (1.0f / 255.0f);
            }
        }
        const int64_t inShape[4] = {1, 1, inH, inW};
        const char *inNames[] = {g_sr_in_name.c_str()};
        const char *outNames[] = {g_sr_out_name.c_str()};
        try {
            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
                g_sr_in_planar.data(), inPx, inShape, 4);
            auto outputs = g_sr_session->Run(Ort::RunOptions{nullptr},
                                             inNames, &inTensor, 1, outNames, 1);
            if (outputs.empty() || !outputs[0].IsTensor()) {
                LOGE("sr_upscale: no output tensor");
                return 0;
            }
            auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            if (outShape.size() != 4 || outShape[2] != (int64_t)outH || outShape[3] != (int64_t)outW) {
                LOGE("sr_upscale: unexpected out shape");
                return 0;
            }
            const float *ySr = outputs[0].GetTensorData<float>();
            // 1) 双线性放大 RGBA 到 out
            bilinearRgbaX(in, inW, inH, out, scale);
            // 2) luma-delta 合并：out += (y_sr - Y(out))
            g_sr_y_delta.resize(outPx);
            {
                const uint8_t *d = out;
                float *dl = g_sr_y_delta.data();
                for (size_t i = 0; i < outPx; ++i, d += 4) {
                    const float yUp = 0.299f * d[0] + 0.587f * d[1] + 0.114f * d[2];
                    dl[i] = ySr[i] * 255.0f - yUp;
                }
            }
            uint8_t *d = out;
            const float *dl = g_sr_y_delta.data();
            for (size_t i = 0; i < outPx; ++i, d += 4) {
                int ri = (int)(d[0] + dl[i] + 0.5f);
                int gi = (int)(d[1] + dl[i] + 0.5f);
                int bi = (int)(d[2] + dl[i] + 0.5f);
                d[0] = ri < 0 ? 0 : (ri > 255 ? 255 : (uint8_t)ri);
                d[1] = gi < 0 ? 0 : (gi > 255 ? 255 : (uint8_t)gi);
                d[2] = bi < 0 ? 0 : (bi > 255 ? 255 : (uint8_t)bi);
                d[3] = 255;
            }
            return 1;
        } catch (const Ort::Exception &e) {
            LOGE("sr_upscale Ort::Exception: %{public}s", e.what());
            g_sr_ready.store(false);
            return 0;
        } catch (...) {
            LOGE("sr_upscale: unknown exception");
            return 0;
        }
    }

    // ---- RGB 通路 ----
    g_sr_in_planar.resize(inPx * 3);
    // RGBA → RGB planar float [0,1]
    {
        float *rp = g_sr_in_planar.data();
        float *gp = rp + inPx;
        float *bp = gp + inPx;
        const uint8_t *p = in;
        for (size_t i = 0; i < inPx; ++i, p += 4) {
            rp[i] = p[0] * (1.0f / 255.0f);
            gp[i] = p[1] * (1.0f / 255.0f);
            bp[i] = p[2] * (1.0f / 255.0f);
        }
    }

    const int64_t inShape[4] = {1, 3, inH, inW};
    const char *inNames[] = {g_sr_in_name.c_str()};
    const char *outNames[] = {g_sr_out_name.c_str()};

    try {
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
            g_sr_in_planar.data(), inPx * 3, inShape, 4);
        auto outputs = g_sr_session->Run(Ort::RunOptions{nullptr},
                                         inNames, &inTensor, 1, outNames, 1);
        if (outputs.empty() || !outputs[0].IsTensor()) {
            LOGE("sr_upscale: no output tensor");
            return 0;
        }
        auto outShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
        if (outShape.size() != 4 || outShape[2] != (int64_t)outH || outShape[3] != (int64_t)outW) {
            LOGE("sr_upscale: unexpected out shape");
            return 0;
        }
        const float *o = outputs[0].GetTensorData<float>();
        // RGB planar float → RGBA uint8
        uint8_t *d = out;
        for (size_t i = 0; i < outPx; ++i, d += 4) {
            float r = o[i];
            float g = o[outPx + i];
            float b = o[outPx * 2 + i];
            int ri = (int)(r * 255.0f + 0.5f);
            int gi = (int)(g * 255.0f + 0.5f);
            int bi = (int)(b * 255.0f + 0.5f);
            d[0] = ri < 0 ? 0 : (ri > 255 ? 255 : (uint8_t)ri);
            d[1] = gi < 0 ? 0 : (gi > 255 ? 255 : (uint8_t)gi);
            d[2] = bi < 0 ? 0 : (bi > 255 ? 255 : (uint8_t)bi);
            d[3] = 255;
        }
        return 1;
    } catch (const Ort::Exception &e) {
        LOGE("sr_upscale Ort::Exception: %{public}s", e.what());
        g_sr_ready.store(false);  // 推理异常后置未就绪，走普通路径兜底
        return 0;
    } catch (...) {
        LOGE("sr_upscale: unknown exception");
        return 0;
    }
}

}  // extern "C"
