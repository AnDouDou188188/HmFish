// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ YoloDetector 头文件（yolo_detector.h）—— YOLO 鱼类检测封装的对外接口          ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】声明单帧 RGBA 目标检测接口：InitFromFile 加载 ncnn 模型，               ║
// ║   Detect 把一帧 RGBA 裸流变成归一化检测框列表（鱼种类别 + 置信度）。            ║
// ║   napi 侧的 NativeDetectOnRgba 入口在 libentry 中转调本类。                    ║
// ║                                                                                ║
// ║ 【模型输入输出规格】                                                            ║
// ║   输入：letterbox 到 inputSize_（默认 640）的正方形 RGB 图；                    ║
// ║         预处理仅乘 1/255 归一化（mean=0），输入 blob 名 "in0"。                 ║
// ║   输出：blob "out0"，YOLOv8 解耦头布局 [4+num_labels, N]：                     ║
// ║         前 4 行为 cx/cy/w/h，其余每行一个类别分数（无 objectness 分支）。       ║
// ║                                                                                ║
// ║ 【线程模型】Detect 可与其他线程并发调用（ncnn Extractor 每次推理独立创建，       ║
// ║   无共享可变状态）；线程数经 SetNumThreads 原子量运行中热调。                   ║
// ║                                                                                ║
// ║ 【与其他模块关系】实现见 yolo_detector.cpp（ncnn 后端）；                        ║
// ║   Detection/NMS 来自 nms.h；net_ 用 void* 隐藏 ncnn::Net*，                    ║
// ║   不向 include 方传染 ncnn 头文件。                                            ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
// ★ YOLO 检测头文件：NativeDetectOnRgba 等 RGBA 帧检测接口与 Detection 结构
#ifndef YOLO_DETECTOR_H
#define YOLO_DETECTOR_H

#include <string>
#include <vector>
#include <cstdint>
#include "nms.h" // 引入 Detection 和 NMS 声明

// ★ YoloDetector：ncnn 后端的 YOLO 检测器封装。
//   设计要点：net_ 用 void* 藏起 ncnn::Net*，本头文件不暴露任何 ncnn 类型，
//   避免所有 include 方依赖 ncnn 头；实例可跨线程共享（推理状态在 Extractor 里，
//   每次独立创建，实例本身无可变状态）
class YoloDetector {
public:
    YoloDetector() = default;
    // ★ ~YoloDetector：析构自动 Release，杜绝忘记手动释放导致的权重内存泄漏
    ~YoloDetector();

    // ★ InitFromFile：加载 ncnn 模型对（.param=网络结构文本 + .bin=权重二进制）。
    //   inputSize 为 letterbox 目标边长（如 640）；可重复调用（内部先释放旧模型）
    // 改为接收 param 和 bin 路径
    bool InitFromFile(const std::string &paramPath, const std::string &binPath, int inputSize);
    // ★ Release：立即释放模型权重内存；之后可再 InitFromFile 换模型
    void Release();

    // ★ SetNumThreads：热调推理线程数（能耗模式档位映射，内部夹紧 1~8）。
    //   立即生效：ncnn 每次 create_extractor 都读 net->opt.num_threads，
    //   不需要重载模型
    // thread count configurable (power mode preset), applied at init and on the fly
    void SetNumThreads(int n);

    // ★ Detect：单帧检测主入口（NativeDetectOnRgba 的核心实现体）。
    //   rgba：tightly-packed 的 RGBA8888 裸流（width×height，行间无对齐填充）；
    //   confThresh：类别分数阈值（anchor 最高类别分低于此值直接丢弃）；
    //   iouThresh：NMS 的 IoU 抑制阈值（越大保留框越多）。
    //   返回：归一化检测框（x/y=左上角、w/h=宽高，均除以原帧宽高），
    //   上层乘以任意目标帧（预览/录像/截图）的宽高即可直接叠框
    std::vector<Detection> Detect(const uint8_t *rgba, int width, int height,
                                  float confThresh = 0.25f, float iouThresh = 0.45f);

private:
    void *net_ = nullptr; // 隐藏 ncnn::Net 指针，避免头文件暴露 ncnn（实现文件里 static_cast 还原）
    // letterbox 目标边长（正方形输入的边长；InitFromFile 传入，Detect 使用）
    int inputSize_ = 640;
    // 预处理减均值（全 0：不做均值中心化，与模型导出时的预处理约定一致）
    float meanVals_[3] = {0.f, 0.f, 0.f};
    // 预处理归一化系数（1/255：像素值压到 [0,1]）
    float normVals_[3] = {1/255.f, 1/255.f, 1/255.f};
};

#endif // YOLO_DETECTOR_H