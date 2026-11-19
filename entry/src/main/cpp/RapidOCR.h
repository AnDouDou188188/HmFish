// ★ RapidOCR 头文件：OCR 引擎 C ABI 声明
#ifndef RAPID_OCR_WRAPPER_H
#define RAPID_OCR_WRAPPER_H

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "ocr_c_api.h"

namespace rapidocr_wrapper {

    struct OcrBox {
        std::vector<cv::Point> pts;
        std::string text;
        float score;
        // true = det 阶段先发布的灰色虚线框（文字识别中）
        bool pending = false;
    };

    bool init(const std::string& modelDir);
    void release();
    bool isReady();
    bool isOCREnabled();
    void setOCREnabled(bool enabled);

    void maybeTrigger(const cv::Mat& rgba);

    std::string getResult();
    void getLastBoxes(std::vector<OcrBox>& out);
    void drawLastBoxes(cv::Mat& rgba);

    /* ★ 表格结构识别（PP-Structure SLANet）：单帧同步识别（增强版）——
     * 工作图降采样 1024 + 表格区域裁剪 OCR 提速；预览图落 outDir 并经 cb 阶段回调；
     * structureFound 回写是否识别到表格结构（0=无框，供换帧重试）；
     * 返回 JSON（含 "boxed"/"t_struct"/"t_ocr"，或 {"error"}） */
    bool initTable(const std::string& tableModelPath);
    bool isTableReady();
    std::string recognizeTableEx(const cv::Mat& bgrFull, const std::string& outDir,
                                 ocr_table_event_cb cb, void* userdata,
                                 int& structureFound, std::string& boxedPathOut);

}  // namespace rapidocr_wrapper

#endif
