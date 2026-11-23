// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrUtils.h —— ocrlite 通用工具函数声明（实现见 src/OcrUtils.cpp）             ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】为三网与编排层提供跨阶段公共工具：                                    ║
// ║   ① 缩放参数计算（getScaleParam）      ② DB 后处理几何（getMinBoxes/          ║
// ║   boxScoreFast/unClip）                ③ 小图透视校正（getRotateCropImage）   ║
// ║   ④ 旋转/尺寸适配（matRotate*/adjustTargetImg）                               ║
// ║   ⑤ 张量前处理（substractMeanNormalize：HWC→CHW + 去均值归一化）              ║
// ║   ⑥ ONNX 输入输出名提取（getInputNames/getOutputNames）                       ║
// ║   ⑦ 调试落盘与路径拼接（saveImg/getXxxFilePath）                              ║
// ║ 【流水线位置】被 DbNet/CrnnNet/AngleNet/OcrLiteImpl 按需包含；                ║
// ║   其中 DB 后处理与透视校正是"检测框 → 可识别小图"链路的核心。                 ║
// ║ 【与 Android 原版差异】接口签名一致；OpenCV 5 下 getBox 等                    ║
// ║   依赖的 minAreaRect/getPerspectiveTransform 已迁至 geometry 模块             ║
// ║   （见 OcrUtils.cpp 顶部 include），逻辑本身无差异。                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_UTILS_H__
#define __OCR_UTILS_H__

#include <opencv2/core.hpp>
#include "OcrStruct.h"
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <numeric>
#include <sys/stat.h>

// ★ makeUnique：std::make_unique 的等价封装（原版为兼容低版本 C++ 而写，
//   C++17 下可直接用 std::make_unique，保留是为了与上游保持一致）
template<typename T, typename... Ts>
static std::unique_ptr<T> makeUnique(Ts &&... params) {
    return std::unique_ptr<T>(new T(std::forward<Ts>(params)...));
}

// ★ getMean：求容器均值（用于统计类调试输出）
template<typename T>
static double getMean(std::vector<T> &input) {
    auto sum = accumulate(input.begin(), input.end(), 0.0);
    return sum / input.size();
}

// ★ getStdev：求容器样本标准差（除以 n-1；元素数 ≤1 时返回 0 防除零）
template<typename T>
static double getStdev(std::vector<T> &input, double mean) {
    if (input.size() <= 1) return 0;
    double accum = 0.0;
    for_each(input.begin(), input.end(), [&](const double d) {
        accum += (d - mean) * (d - mean);
    });
    double stdev = sqrt(accum / (input.size() - 1));
    return stdev;
}

// ★ clamp：区间钳制（DB 后处理把坐标限制在图内时大量使用）
template<class T>
inline T clamp(T x, T min, T max) {
    if (x > max)
        return max;
    if (x < min)
        return min;
    return x;
}

// ★ getCurrentTime：取当前时刻（ms）——基于 OpenCV 高精度计时器，
//   各网络阶段用两次调用差值统计耗时
double getCurrentTime();

// ★ isFileExists：文件存在性检查（stat 实现，避免依赖 C++17 filesystem）
inline bool isFileExists(const std::string &name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

// ★ strToWstr：窄字符串转宽字符串——Windows 下 Ort::Session 构造需要 wchar 路径，
//   实现为逐字符赋值的简化转换（仅适合 ASCII 路径），非 Windows 平台不使用
std::wstring strToWstr(std::string str);

// ★ getScaleParam（按倍率）：以 scale 系数缩放并取整到 32 的倍数，返回缩放参数
ScaleParam getScaleParam(cv::Mat &src, const float scale);

// ★ getScaleParam（按目标长边）：等比缩放使长边不超过 targetSize，再对齐 32 倍数；
//   HmFish 实际走此重载（maxSideLen=1024）
ScaleParam getScaleParam(cv::Mat &src, const int targetSize);

// ★ getBox：取旋转矩形的四个顶点（顶点顺序为绕矩形一周）
std::vector<cv::Point2f> getBox(const cv::RotatedRect &rect);

// ★ getThickness：按图尺寸自适应的画框线宽（小图约 2px，大图加粗）
int getThickness(cv::Mat &boxImg);

// ★ drawTextBox（旋转矩形版）：画红色文本框（BGR 的 R=255）
void drawTextBox(cv::Mat &boxImg, cv::RotatedRect &rect, int thickness);

// ★ drawTextBox（四点框版）：按左上/右上/右下/左下四点连线画红色文本框
void drawTextBox(cv::Mat &boxImg, const std::vector<cv::Point> &box, int thickness);

// ★ drawTextBoxes：批量画检测框（调试结果图 boxImg 用）
void drawTextBoxes(cv::Mat &boxImg, std::vector<TextBox> &textBoxes, int thickness);

// ★ matRotateClockWise180：顺时针旋转 180 度（两次 flip 实现，用于方向分类判定为倒置的小图）
cv::Mat matRotateClockWise180(cv::Mat src);

// ★ matRotateClockWise90：顺时针旋转 90 度（转置+水平翻转实现）
cv::Mat matRotateClockWise90(cv::Mat src);

// ★ getRotateCropImage：按四点框透视校正裁出"摆正"的小图（det→rec 的关键一步，
//   内部做外接矩形裁剪、透视变换与竖排文本 90 度转正）
cv::Mat getRotateCropImage(const cv::Mat &src, std::vector<cv::Point> box);

// ★ adjustTargetImg：把小图缩放到固定高度、再贴到固定尺寸的白底图上
//   （不足处填白，超出处居中裁剪）——方向分类网的定尺寸输入用
cv::Mat adjustTargetImg(cv::Mat &src, int dstWidth, int dstHeight);

// ★ getMinBoxes：把旋转矩形还原为按"左上/右上/右下/左下"排序的四点框，
//   并带出矩形长边长度（DB 后处理用它过滤过小的框）
std::vector<cv::Point2f> getMinBoxes(const cv::RotatedRect &boxRect, float &maxSideLen);

// ★ boxScoreFast：计算四点框内 DB 概率图的平均得分——按外接矩形裁出小块，
//   用多边形掩码只统计框内像素，得分低于阈值的候选框被丢弃
float boxScoreFast(const std::vector<cv::Point2f> &boxes, const cv::Mat &pred);

// ★ unClip：DB 后处理扩边——按 Vatti 偏移算法（Clipper 库）把文本框向外扩张
//   unClipRatio 倍"面积/周长"的距离，补偿二值化收缩的边缘，避免裁字
cv::RotatedRect unClip(std::vector<cv::Point2f> box, float unClipRatio);

// ★ substractMeanNormalize：三网统一的张量前处理——HWC 的 BGR 像素按通道
//   做归一化并重排为 CHW 的 NCHW 输入向量
std::vector<float> substractMeanNormalize(cv::Mat &src, const float *meanVals, const float *normVals);

// ★ getAngleIndexes：抽取 Angle 数组的 index 序列（mostAngle 多数投票统计用）
std::vector<int> getAngleIndexes(std::vector<Angle> &angles);

// ★ getInputNames：提取 ONNX 会话的全部输入节点名（推理时按名绑定输入张量）
std::vector<Ort::AllocatedStringPtr> getInputNames(Ort::Session *session);

// ★ getOutputNames：提取 ONNX 会话的全部输出节点名（推理时按名取输出张量）
std::vector<Ort::AllocatedStringPtr> getOutputNames(Ort::Session *session);

// ★ saveImg：调试用图片落盘（imwrite 封装）
void saveImg(cv::Mat &img, const char *imgPath);

// ★ getSrcImgFilePath：拼接源图完整路径（path + imgName）
std::string getSrcImgFilePath(const char *path, const char *imgName);

// ★ getResultTxtFilePath：拼接识别结果 txt 路径（imgName + "-result.txt"）
std::string getResultTxtFilePath(const char *path, const char *imgName);

// ★ getResultImgFilePath：拼接结果图路径（imgName + "-result.jpg"）
std::string getResultImgFilePath(const char *path, const char *imgName);

// ★ getDebugImgFilePath：拼接中间过程调试图路径（imgName + tag + 序号 + ".jpg"）
std::string getDebugImgFilePath(const char *path, const char *imgName, size_t i, const char *tag);

#endif //__OCR_UTILS_H__
