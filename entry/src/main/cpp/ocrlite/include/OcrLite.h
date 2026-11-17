#ifndef __OCR_LITE_H__
#define __OCR_LITE_H__

#include <string>
#include <functional>
#include "OcrStruct.h"

class OcrLiteImpl;

class OcrLite{
public:
    OcrLite();

    ~OcrLite();

    void setNumThread(int numOfThread);

    void initLogger(bool isConsole, bool isPartImg, bool isResultImg);

    void enableResultTxt(const char *path, const char *imgName);

    void setGpuIndex(int gpuIndex);

    bool initModels(const std::string &detPath, const std::string &clsPath,
                    const std::string &recPath, const std::string &keysPath);

    // 两阶段懒加载：文本检测(det)完成后立即回调检测框（原始坐标，已扣除 padding），
    // 供 UI 先绘制灰色虚线框，识别(rec)完成后再补全文字
    void setDetStageCallback(std::function<void(const std::vector<std::vector<cv::Point>> &)> cb);

    void Logger(const char *format, ...);

    OcrResult detect(const char *path, const char *imgName,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

    OcrResult detect(const cv::Mat &mat,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

private:
    OcrLiteImpl* pImpl;
};

#endif //__OCR_LITE_H__
