// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrLiteImpl.h —— ocrlite 实现层（三网编排核心）                               ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】持有三网对象并编排完整流水线：padding→缩放→DbNet 检测→                ║
// ║   detStageCallback 先行回报框→裁行小图→AngleNet 方向分类                      ║
// ║   （doAngle=false 时跳过）→CrnnNet 识别→聚合 OcrResult。                      ║
// ║   OcrLite 门面的全部接口在此落地。                                            ║
// ║ 【流水线位置】中间编排层：上接 OcrLite 门面，下调三网与 getPartImages。       ║
// ║   实现见 src/OcrLiteImpl.cpp。                                                ║
// ║ 【与 Android 原版差异】新增 detStageCallback 两阶段回调成员及注入；           ║
// ║   其余流程一致。                                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_LITE_IMPL_H__
#define __OCR_LITE_IMPL_H__

#include "opencv2/core.hpp"
#include <functional>
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include "OcrStruct.h"
#include "DbNet.h"
#include "AngleNet.h"
#include "CrnnNet.h"
#include "OcrLite.h"

// ★ OcrLiteImpl：编排实现类（由 OcrLite 持有）
class OcrLiteImpl {
public:
    OcrLiteImpl();

    ~OcrLiteImpl();

    // ★ setNumThread：三网统一设置推理线程数
    void setNumThread(int numOfThread);

    // ★ initLogger：日志开关落地（控制台/中间图/结果图）
    void initLogger(bool isConsole, bool isPartImg, bool isResultImg);

    // ★ enableResultTxt：结果 txt 落盘开关（探鱼未启用）
    void enableResultTxt(const char *path, const char *imgName);

    // ★ setGpuIndex：三网统一挂 GPU（CPU 构建为空操作）
    void setGpuIndex(int gpuIndex);

    // det 阶段完成回调（原始坐标）
    void setDetStageCallback(std::function<void(const std::vector<std::vector<cv::Point>> &)> cb) {
        detStageCallback = std::move(cb);
    }

    // ★ initModels：依次初始化 det/cls/rec 三网与 keys 字表
    bool initModels(const std::string &detPath, const std::string &clsPath,
                    const std::string &recPath, const std::string &keysPath);

    // ★ Logger：OCRLITE 宏日志落点（vsnprintf 格式化后按开关输出）
    void Logger(const char *format, ...);

    // ★ detect（路径版）：读图后委托 Mat 版 detect
    OcrResult detect(const char *path, const char *imgName,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

    // ★ detect（Mat 版）：完整流水线主入口
    OcrResult detect(const cv::Mat &mat,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

    // ★ detectImageBytes：字节流入口（按 grey 标志解码灰度/彩色）
    OcrResult detectImageBytes(const uint8_t *data, long dataLength, int grey, int padding, int maxSideLen,
                               float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);
    // ★ detectBitmap：裸位图入口（不解码、直接包装成 Mat）
    OcrResult detectBitmap(uint8_t *bitmapData, int width, int height,int channels, int padding, int maxSideLen,
                           float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

private:
    // 门面 Logger 的友元授权（门面需访问私有日志缓冲）
    friend void OcrLite::Logger(const char *format, ...);
    // 日志开关：输出到控制台
    bool isOutputConsole = false;
    // 日志开关：行小图落盘
    bool isOutputPartImg = false;
    // 日志开关：结果 txt 落盘
    bool isOutputResultTxt = false;
    // 日志开关：画框结果图落盘
    bool isOutputResultImg = false;
    // 结果 txt 句柄（enableResultTxt 时打开）
    FILE *resultTxt;
    // 检测网
    DbNet dbNet;
    // 方向分类网
    AngleNet angleNet;
    // 识别网
    CrnnNet crnnNet;
    // 日志格式化缓冲（构造时 malloc）
    char *loggerBuffer;
    // det 阶段回调（detect 内 DbNet 出框后立即触发）
    std::function<void(const std::vector<std::vector<cv::Point>> &)> detStageCallback;

    // ★ getPartImages：按 TextBox 四点透视裁剪行小图（getPerspectiveTransform）
    std::vector<cv::Mat> getPartImages(cv::Mat &src, std::vector<TextBox> &textBoxes,
                                       const char *path, const char *imgName);

    // ★ detect（内部统一版）：路径/Mat 两个入口都汇聚到此，
    //   依次执行 padding→缩放→det→回调→裁行→cls→rec→聚合
    OcrResult detect(const char *path, const char *imgName,
                     cv::Mat &src, cv::Rect &originRect, ScaleParam &scale,
                     float boxScoreThresh = 0.6f, float boxThresh = 0.3f,
                     float unClipRatio = 2.0f, bool doAngle = true, bool mostAngle = true);
};

#endif //__OCR_LITE_IMPL_H__
