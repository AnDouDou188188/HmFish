// ★ YOLO 检测头文件：NativeDetectOnRgba 等 RGBA 帧检测接口与 Detection 结构
#ifndef YOLO_DETECTOR_H
#define YOLO_DETECTOR_H

#include <string>
#include <vector>
#include <cstdint>
#include "nms.h" // 引入 Detection 和 NMS 声明

class YoloDetector {
public:
    YoloDetector() = default;
    ~YoloDetector();

    // 改为接收 param 和 bin 路径
    bool InitFromFile(const std::string &paramPath, const std::string &binPath, int inputSize);
    void Release();

    // thread count configurable (power mode preset), applied at init and on the fly
    void SetNumThreads(int n);

    std::vector<Detection> Detect(const uint8_t *rgba, int width, int height,
                                  float confThresh = 0.25f, float iouThresh = 0.45f);

private:
    void *net_ = nullptr; // 隐藏 ncnn::Net 指针，避免头文件暴露 ncnn
    int inputSize_ = 640;
    float meanVals_[3] = {0.f, 0.f, 0.f};
    float normVals_[3] = {1/255.f, 1/255.f, 1/255.f};
};

#endif // YOLO_DETECTOR_H