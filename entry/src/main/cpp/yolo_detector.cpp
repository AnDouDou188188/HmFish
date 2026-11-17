// ★ YOLO 检测实现：ncnn 前处理（letterbox/归一化）→ 推理 → 解码 + NMS 后处理
#include "yolo_detector.h"
#include <hilog/log.h>
#include <net.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "YoloNcnn"

// ★ 推理线程数：会话级状态（napi 侧 SetYoloThreads 更新，InitFromFile 时应用）
static std::atomic<int> g_num_threads(8);

YoloDetector::~YoloDetector() {
    Release();
}
bool YoloDetector::InitFromFile(const std::string &paramPath, const std::string &binPath, int inputSize) {
    inputSize_ = inputSize;
    Release(); // 释放旧的模型

    ncnn::Net* net = new ncnn::Net();
    net->opt.use_vulkan_compute = false;

    // ★ 核心提速 1：线程数可配（能耗模式 2~8，默认 8 跟 Android 一样跑满大小核）
    net->opt.num_threads = g_num_threads;

    // ★ 核心提速 2：开启 FP16 硬件加速，速度再提升 30%
    net->opt.use_fp16_packed = true;
    net->opt.use_fp16_storage = true;
    net->opt.use_fp16_arithmetic = true;

    if (net->load_param(paramPath.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "ncnn load_param failed: %{public}s", paramPath.c_str());
        delete net;
        return false;
    }
    if (net->load_model(binPath.c_str()) != 0) {
        OH_LOG_ERROR(LOG_APP, "ncnn load_model failed: %{public}s", binPath.c_str());
        delete net;
        return false;
    }

    net_ = net;
    OH_LOG_INFO(LOG_APP, "ncnn init OK, threads=%{public}d, fp16=ON, size=%{public}d",
                g_num_threads.load(), inputSize_);
    return true;
}

// ★ 运行时改线程数：ncnn create_extractor 每次取 net->opt.num_threads，改完立即生效
void YoloDetector::SetNumThreads(int n) {
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    g_num_threads.store(n);
    if (net_) {
        static_cast<ncnn::Net*>(net_)->opt.num_threads = n;
        OH_LOG_INFO(LOG_APP, "yolo threads -> %{public}d", n);
    }
}

void YoloDetector::Release() {
    if (net_) {
        delete static_cast<ncnn::Net*>(net_);
        net_ = nullptr;
    }
}

std::vector<Detection> YoloDetector::Detect(const uint8_t *rgba, int width, int height,
                                            float confThresh, float iouThresh) {
    std::vector<Detection> result;
    if (!net_ || !rgba || width <= 0 || height <= 0) return result;

    ncnn::Net* net = static_cast<ncnn::Net*>(net_);

    // 1. Letterbox 预处理
    int w = width, h = height;
    float scale;
    if (w > h) { scale = (float)inputSize_ / w; w = inputSize_; h = h * scale; }
    else       { scale = (float)inputSize_ / h; h = inputSize_; w = w * scale; }

    // ncnn 原生支持直接从 RGBA 裸流提取并转为 RGB
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgba, ncnn::Mat::PIXEL_RGBA2RGB, width, height, w, h);

    int wpad = (inputSize_ + 31) / 32 * 32 - w;
    int hpad = (inputSize_ + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2,
                           ncnn::BORDER_CONSTANT, 114.f);
    in_pad.substract_mean_normalize(meanVals_, normVals_);

    // 2. 推理
    ncnn::Extractor ex = net->create_extractor();
    ex.input("in0", in_pad); 
    
    ncnn::Mat out;
    ex.extract("out0", out); 

    if (out.empty()) return result;

    // 3. 解析输出 (兼容 YOLOv8 结构)
    int ch_dim, anchor_dim;
    if (out.h < out.w) { ch_dim = out.h; anchor_dim = out.w; }
    else               { ch_dim = out.w; anchor_dim = out.h; }

    int num_labels = ch_dim - 4;
    if (num_labels <= 0) return result;

    std::vector<Detection> proposals;
    const float* data = (const float*)out.data;

    for (int a = 0; a < anchor_dim; a++) {
        float maxScore = 0.f;
        int maxClass = -1;
        for (int c = 0; c < num_labels; c++) {
            float score = data[(4 + c) * anchor_dim + a];
            if (score > maxScore) { maxScore = score; maxClass = c; }
        }

        if (maxScore >= confThresh) {
            float cx = data[0 * anchor_dim + a];
            float cy = data[1 * anchor_dim + a];
            float bw = data[2 * anchor_dim + a];
            float bh = data[3 * anchor_dim + a];

            // 还原到原图坐标并归一化
            float x1px = (cx - bw * 0.5f - wpad / 2.f) / scale;
            float y1px = (cy - bh * 0.5f - hpad / 2.f) / scale;
            float bx = x1px / width;
            float by = y1px / height;
            float nw = bw / scale / width;
            float nh = bh / scale / height;

            // 裁剪越界
            if (bx < 0) { nw += bx; bx = 0; }
            if (by < 0) { nh += by; by = 0; }
            if (bx + nw > 1) nw = 1 - bx;
            if (by + nh > 1) nh = 1 - by;

            if (nw > 0.001f && nh > 0.001f) {
                proposals.push_back({bx, by, nw, nh, maxScore, maxClass});
            }
        }
    }

    // 4. NMS 过滤
    if (!proposals.empty()) {
        result = NMS(proposals, iouThresh);
    }

    return result;
}