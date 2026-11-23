// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ DbNet.h —— 文本检测网封装（det 阶段，DB 论文）                                ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】从整图输出文本区域的概率图并还原四点框，是 OCR 的第一网。             ║
// ║   实现见 src/DbNet.cpp；DB 后处理流程：阈值二值化→连通域提取→                 ║
// ║   boxScoreFast 打分→unClip 扩边（ClipperOffset）→minAreaRect 旋转框。         ║
// ║ 【流水线位置】三网之首：接收 makeBorderPadding 后的缩放图，输出的 TextBox 被  ║
// ║   getPartImages 裁成行小图再送 cls/rec 两网。                                 ║
// ║ 【输入规格】动态尺寸 NCHW float32，ImageNet 三通道 mean/std 归一化；          ║
// ║   输出 1x1xH/4xW/4 概率图（H/W 为输入高宽，DB 结构下采样 4 倍）。             ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_DBNET_H__
#define __OCR_DBNET_H__

#include "OcrStruct.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

// ★ DbNet：检测网。会话生命周期同 AngleNet/CrnnNet
class DbNet {
public:
    // ★ 析构：释放 ONNX 会话与输入/输出名
    ~DbNet();

    // ★ setNumThread：设置推理线程数（须在 initModel 前调）
    void setNumThread(int numOfThread);

    // ★ setGpuIndex：挂 CUDA/DirectML 执行提供者（CPU 构建为空操作）
    void setGpuIndex(int gpuIndex);

    // ★ initModel：从 onnx 文件构建检测会话，提取输入/输出节点名
    void initModel(const std::string &pathStr);

    // ★ getTextBoxes：检测入口——推理概率图后跑 DB 后处理六步，
    //   返回原图坐标系下的文本框（内部已除回缩放比并扣 padding）
    std::vector<TextBox> getTextBoxes(cv::Mat &src, ScaleParam &s, float boxScoreThresh,
                                      float boxThresh, float unClipRatio);

private:
    // ONNX 推理会话
    Ort::Session *session = nullptr;
    // ORT 环境
    Ort::Env env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "DbNet");
    // 会话配置
    Ort::SessionOptions sessionOptions = Ort::SessionOptions();
    // 推理线程数（记录用）
    int numThread = 0;

    // 输入节点名
    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    // 输出节点名
    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;

    // ImageNet BGR 均值（乘 255 后的绝对值）
    const float meanValues[3] = {0.485 * 255, 0.456 * 255, 0.406 * 255};
    // ImageNet BGR 标准差倒数（含 /255）
    const float normValues[3] = {1.0 / 0.229 / 255.0, 1.0 / 0.224 / 255.0, 1.0 / 0.225 / 255.0};
};


#endif //__OCR_DBNET_H__
