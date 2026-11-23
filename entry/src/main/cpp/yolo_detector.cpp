// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ YoloDetector 实现（yolo_detector.cpp）—— ncnn YOLO 鱼类检测全流程              ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】单帧 RGBA → letterbox 前处理 → ncnn 前向 → YOLOv8 解码 → NMS 去重。    ║
// ║   结果为归一化坐标，napi 侧 NativeDetectOnRgba 直接回传 ArkTS 叠框。           ║
// ║                                                                                ║
// ║ 【推理管线四步】                                                                ║
// ║   1. Letterbox：长边缩放到 inputSize_、短边补 114 灰并 32 对齐                 ║
// ║      （保持宽高比不形变，114 是 YOLO 约定的中性填充灰度）                        ║
// ║   2. 归一化：substract_mean_normalize(0, 1/255)，等价于像素 / 255               ║
// ║   3. 前向：blob "in0" → "out0"；FP16 存储/计算 + 多线程加速                     ║
// ║   4. 解码 + NMS：out0 按 [4+num_labels, N] 布局逐 anchor 取 top-1 类别分，      ║
// ║      过 confThresh 后反解框坐标（扣 padding → 除 scale → 归一化），             ║
// ║      最后 NMS（IoU + IoM 包含抑制）去重                                          ║
// ║                                                                                ║
// ║ 【线程模型】无内部锁：Detect 纯读 net_，可与 SetNumThreads 并发；              ║
// ║   g_num_threads 为原子量，napi 线程写、init/运行中读，无锁安全。                ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
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
//   用 atomic 的原因：napi 线程写、检测线程读，且运行中可热改（SetNumThreads）
static std::atomic<int> g_num_threads(8);

// ★ ~YoloDetector：析构即释放模型权重，保证异常路径（未手动 Release）也不泄漏
YoloDetector::~YoloDetector() {
    Release();
}
// ★ InitFromFile：加载 .param/.bin 并按当前性能档位配置 ncnn 选项。
//   失败语义：任一步加载失败立即 delete 并返回 false，不留半初始化状态
bool YoloDetector::InitFromFile(const std::string &paramPath, const std::string &binPath, int inputSize) {
    inputSize_ = inputSize;
    Release(); // 释放旧的模型

    ncnn::Net* net = new ncnn::Net();
    // Vulkan 关闭：设备 GPU 驱动兼容性参差，CPU + FP16 已满足实时性，走 GPU 反而引入排队延迟
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

// ★ Release：释放 ncnn::Net（权重内存）并置空指针；置空保证重复调用安全，
//   也让 Detect 在"释放后/加载前"窗口期直接返回空结果而不是野指针崩溃
void YoloDetector::Release() {
    if (net_) {
        delete static_cast<ncnn::Net*>(net_);
        net_ = nullptr;
    }
}

// ───────── 推理主流程：letterbox → ncnn 前向 → YOLOv8 解码 → NMS ─────────
// ★ Detect：整帧检测。全流程围绕两个几何量展开——
//   scale（letterbox 缩放比）与 (wpad, hpad)（补边像素）：解码时反向
//   "扣 padding → 除 scale"即可从模型坐标回到原图坐标
std::vector<Detection> YoloDetector::Detect(const uint8_t *rgba, int width, int height,
                                            float confThresh, float iouThresh) {
    std::vector<Detection> result;
    if (!net_ || !rgba || width <= 0 || height <= 0) return result;

    ncnn::Net* net = static_cast<ncnn::Net*>(net_);

    // 1. Letterbox 预处理：长边压到 inputSize_、短边等比缩，保持宽高比；
    //    后续再补边到 32 的倍数（ncnn 尺寸对齐利于 SIMD，检测精度不受补边影响）
    int w = width, h = height;
    float scale;
    if (w > h) { scale = (float)inputSize_ / w; w = inputSize_; h = h * scale; }
    else       { scale = (float)inputSize_ / h; h = inputSize_; w = w * scale; }

    // ncnn 原生支持直接从 RGBA 裸流提取并转为 RGB
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(rgba, ncnn::Mat::PIXEL_RGBA2RGB, width, height, w, h);

    // 补边量：把 inputSize_ 向上对齐到 32 的倍数后与缩放图的差值；
    // 除 2 分摊到上下/左右两侧让内容居中，与 YOLO 训练 letterbox 的习惯一致
    int wpad = (inputSize_ + 31) / 32 * 32 - w;
    int hpad = (inputSize_ + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2,
                           ncnn::BORDER_CONSTANT, 114.f);
    // 归一化：mean=0、norm=1/255，等价像素值 /255 压到 [0,1]（与训练导出约定一致）
    in_pad.substract_mean_normalize(meanVals_, normVals_);

    // 2. 推理
    ncnn::Extractor ex = net->create_extractor();
    ex.input("in0", in_pad); 
    
    ncnn::Mat out;
    ex.extract("out0", out); 

    if (out.empty()) return result;

    // 3. 解析输出（兼容 YOLOv8 结构）：ncnn 输出 Mat 的 w/h 哪维是通道并不固定，
    //    取较小维当通道维（4+num_labels 通常远小于 anchor 数 N）；
    //    数据等价 [4+num_labels, N] 布局：第 0~3 行是 cx/cy/w/h，第 4+c 行是类别 c 分数
    int ch_dim, anchor_dim;
    if (out.h < out.w) { ch_dim = out.h; anchor_dim = out.w; }
    else               { ch_dim = out.w; anchor_dim = out.h; }

    int num_labels = ch_dim - 4;
    if (num_labels <= 0) return result;

    std::vector<Detection> proposals;
    const float* data = (const float*)out.data;

    // 逐 anchor 解码：只取 top-1 类别（YOLOv8 解耦头无 objectness，类别最大分即置信度），
    // 相比把所有类别都展开成候选框，框数少一个数量级、NMS 也快得多
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

            // 还原到原图坐标并归一化：模型输出在 letterbox 图坐标系，
            // 先扣掉 pad/2（padding 均分在两侧）、再除 scale 回到原图像素，
            // 最后除以原帧宽高 → [0,1] 归一化框（与分辨率彻底解耦）
            float x1px = (cx - bw * 0.5f - wpad / 2.f) / scale;
            float y1px = (cy - bh * 0.5f - hpad / 2.f) / scale;
            float bx = x1px / width;
            float by = y1px / height;
            float nw = bw / scale / width;
            float nh = bh / scale / height;

            // 裁剪越界：目标贴图边时反解会出现负坐标/超 1 边界，
            // 钳回 [0,1] 并同步收缩宽高，保证框始终完整落在画面内
            if (bx < 0) { nw += bx; bx = 0; }
            if (by < 0) { nh += by; by = 0; }
            if (bx + nw > 1) nw = 1 - bx;
            if (by + nh > 1) nh = 1 - by;

            if (nw > 0.001f && nh > 0.001f) {
                proposals.push_back({bx, by, nw, nh, maxScore, maxClass});
            }
        }
    }

    // 4. NMS 过滤：同一目标被多个 anchor 重复检出，按 IoU + IoM 包含关系去重（见 nms.cpp）
    if (!proposals.empty()) {
        result = NMS(proposals, iouThresh);
    }

    return result;
}