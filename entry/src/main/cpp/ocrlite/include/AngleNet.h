// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ AngleNet.h —— 文本方向分类网封装（cls 阶段，0/180 度二分类）                  ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】判断 det 裁出的行小图是否整体倒置（旋转 180 度），输出二分类          ║
// ║   下标（0=正向 / 1=倒置）与置信度；doAngle=false 时整网跳过推理，             ║
// ║   直接返回 index=-1 占位。实现见 src/AngleNet.cpp。                           ║
// ║ 【流水线位置】三网居中：DbNet 出框、裁小图之后，CrnnNet 识别之前——            ║
// ║   判为倒置的行先旋转 180 度再送识别，可显著减少倒置行乱码。                   ║
// ║   注意 HmFish 调用方（RapidOCR.cpp）固定传 doAngle=false/mostAngle=false，    ║
// ║   即本网在探鱼相机场景完全不推理，换取更低的单帧时延。                        ║
// ║ 【输入规格】定尺寸 192x48（dstWidth x dstHeight），NCHW float32，             ║
// ║   mean/norm 127.5；输出 1x2 概率（正向/倒置）。                               ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_ANGLENET_H__
#define __OCR_ANGLENET_H__

#include "OcrStruct.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

// ★ AngleNet：方向二分类器。会话生命周期同 DbNet/CrnnNet
class AngleNet {

public:

    // ★ 析构：释放 ONNX 会话与输入/输出名
    ~AngleNet();

    // ★ setNumThread：设置推理线程数（须在 initModel 前调）
    void setNumThread(int numOfThread);

    // ★ setGpuIndex：挂 CUDA/DirectML 执行提供者（CPU 构建为空操作）
    void setGpuIndex(int gpuIndex);

    // ★ initModel：从 onnx 文件构建方向分类会话，提取输入/输出节点名
    void initModel(const std::string &pathStr);

    // ★ getAngles：批量方向判定入口——doAngle=true 时逐行缩放到 192x48 推理；
    //   mostAngle=true 时按"多数行的方向"统一覆盖所有行；doAngle=false 时
    //   整网跳过并返回 -1 占位
    std::vector<Angle> getAngles(std::vector<cv::Mat> &partImgs, const char *path,
                                 const char *imgName, bool doAngle, bool mostAngle);

private:
    // 调试开关：分类输入图落盘（恒 false，无置 true 入口）
    bool isOutputAngleImg = false;

    // ONNX 推理会话
    Ort::Session *session = nullptr;
    // ORT 环境
    Ort::Env env = Ort::Env(ORT_LOGGING_LEVEL_ERROR, "AngleNet");
    // 会话配置
    Ort::SessionOptions sessionOptions = Ort::SessionOptions();
    // 推理线程数（记录用）
    int numThread = 0;

    // 输入节点名
    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    // 输出节点名
    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;

    // 归一化参数：与 CrnnNet 相同的 (pix-127.5)/127.5 → [-1,1]
    const float meanValues[3] = {127.5, 127.5, 127.5};
    const float normValues[3] = {1.0 / 127.5, 1.0 / 127.5, 1.0 / 127.5};
    // 分类网输入固定宽 192px（不足处 adjustTargetImg 填白）
    const int dstWidth = 192;
    // 分类网输入固定高 48px
    const int dstHeight = 48;

    // ★ getAngle：单行方向分类全流程（前处理→推理→argmax 取下标与得分）
    Angle getAngle(cv::Mat &src);
};


#endif //__OCR_ANGLENET_H__
