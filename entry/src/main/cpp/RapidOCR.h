// ★ RapidOCR 头文件：OCR 引擎 C ABI 声明
#ifndef RAPID_OCR_WRAPPER_H
#define RAPID_OCR_WRAPPER_H

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

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

    /* ★ 表格结构识别（PP-Structure SLANet）：单帧同步识别，
     * 返回 JSON（{"rows","cols","cells":[{r,c,rs,cs,h,t}],"html","ms"} 或 {"error"}） */
    bool initTable(const std::string& tableModelPath);
    bool isTableReady();
    std::string recognizeTable(const cv::Mat& bgr);

}  // namespace rapidocr_wrapper

#endif
