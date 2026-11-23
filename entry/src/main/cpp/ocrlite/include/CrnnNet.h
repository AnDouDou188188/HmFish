// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ CrnnNet.h —— 文本识别网封装（rec 阶段，CRNN+CTC）                             ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】把 det+cls 裁出的行小图逐张识别成文字串（含逐字符置信度）。           ║
// ║   实现见 src/CrnnNet.cpp；CTC 解码规则：相邻重复帧合并 + blank 帧             ║
// ║   （keys 表下标 0）丢弃。                                                     ║
// ║ 【流水线位置】三网末位：接收 getPartImages 裁出的行小图，输出 TextLine        ║
// ║   聚合进 TextBlock——是 OCR 结果文字内容的唯一来源。                           ║
// ║ 【输入规格】高固定 48px、宽随行宽等比缩放（不设上限），                       ║
// ║   NCHW float32，mean/norm 127.5；输出 1xTxC 概率矩阵（C=keyset+1）。          ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_CRNNNET_H__
#define __OCR_CRNNNET_H__

#include "OcrStruct.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

// ★ CrnnNet：识别网。会话生命周期同 DbNet/AngleNet
class CrnnNet {
public:

    // ★ 析构：释放 ONNX 会话与输入/输出名
    ~CrnnNet();

    // ★ setNumThread：设置推理线程数（须在 initModel 前调）
    void setNumThread(int numOfThread);

    // ★ setGpuIndex：挂 CUDA/DirectML 执行提供者（CPU 构建为空操作）
    void setGpuIndex(int gpuIndex);

    // ★ initModel：加载识别 onnx 模型与 keys 字表（keys[0] 恒为 CTC blank）
    void initModel(const std::string &pathStr, const std::string &keysPath);

    // ★ getTextLines：批量识别入口——逐行前处理→推理→CTC 解码
    std::vector<TextLine> getTextLines(std::vector<cv::Mat> &partImg, const char *path, const char *imgName);

private:
    // 调试开关：识别中间图落盘（恒 false）
    bool isOutputDebugImg = false;
    // ONNX 推理会话
    Ort::Session *session = nullptr;
    // ORT 环境
    Ort::Env env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "CrnnNet");
    // 会话配置
    Ort::SessionOptions sessionOptions = Ort::SessionOptions();
    // 推理线程数（记录用）
    int numThread = 0;

    // 输入节点名
    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    // 输出节点名
    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;

    // 归一化参数：(pix-127.5)/127.5 → [-1,1]
    const float meanValues[3] = {127.5, 127.5, 127.5};
    const float normValues[3] = {1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5};
    // 识别网输入固定高 48px（宽自适应）
    const int dstHeight = 48;

    // keys 字表（keys[0] 为 CTC blank 占位，解码时跳过）
    std::vector<std::string> keys;

    // ★ scoreToTextLine：CTC 解码核心——按 T 帧取 argmax，相邻同类帧去重、
    //   blank 帧丢弃，同步收集逐字符置信度
    TextLine scoreToTextLine(const std::vector<float> &outputData, size_t h, size_t w);

    // ★ getTextLine：单行识别全流程（缩放→归一化→推理→解码）
    TextLine getTextLine(const cv::Mat &src);
};


#endif //__OCR_CRNNNET_H__
