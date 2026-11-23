// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrStruct.h —— ocrlite 公共数据结构定义                                       ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】定义 OCR 流水线各阶段之间传递结果的全部 POD 结构体，是                ║
// ║   det（DbNet 检测）→ cls（AngleNet 方向分类）→ rec（CrnnNet 识别）            ║
// ║   三阶段以及编排层 OcrLiteImpl 之间统一的数据载体。                           ║
// ║ 【流水线位置】最底层头文件：被 DbNet/AngleNet/CrnnNet/OcrUtils/               ║
// ║   OcrLiteImpl 及上层门面 OcrLite.h 共同包含；HmFish 侧 RapidOCR.cpp           ║
// ║   最终把 TextBlock/OcrResult 转换为自己的 OcrBox 结构跨 NAPI 抛给 ArkTS。     ║
// ║ 【与 Android 原版差异】结构定义完全一致，未增删字段；仅补充中文注释。         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_STRUCT_H__
#define __OCR_STRUCT_H__

#include "opencv2/core.hpp"
#include <vector>

// ★ ScaleParam：预处理缩放参数——描述"原始图 → 网络输入图"的尺寸映射关系。
//   由 OcrUtils 的 getScaleParam() 计算后传入 DbNet.getTextBoxes()，
//   DB 后处理阶段用它把网络输出坐标（缩放后）除以 ratio 还原回原图坐标。
struct ScaleParam {
    // 原始图宽（px，含 padding 白边后的尺寸）
    int srcWidth;
    // 原始图高（px）
    int srcHeight;
    // 网络输入图宽（px，已对齐到 32 的倍数——DbNet 下采样 5 次的要求）
    int dstWidth;
    // 网络输入图高（px，同为 32 的倍数）
    int dstHeight;
    // 水平缩放比 = dstWidth / srcWidth，DB 后处理坐标还原用其倒数
    float ratioWidth;
    // 垂直缩放比 = dstHeight / srcHeight
    float ratioHeight;
};

// ★ TextBox：DbNet 检测输出的单个文本框。
//   boxPoint 为四边形（透视倾斜的文本也能框住），顺序为左上/右上/右下/左下。
struct TextBox {
    // 文本框四顶点（原图坐标系，已扣 padding 并除回缩放比）
    std::vector<cv::Point> boxPoint;
    // DB 概率图在该框内的平均得分（boxScoreFast 计算），越高越可能是文本
    float score;
};

// ★ Angle：AngleNet（方向分类网）对单行小图的一次判定结果。
struct Angle {
    // 分类下标：0=正向（无需旋转）；1=旋转 180 度；-1=doAngle=false 时的占位（表示未做分类）
    int index;
    // 分类置信度（softmax 最大值）
    float score;
    // 本行方向分类耗时（ms），用于性能统计
    double time;
};

// ★ TextLine：CrnnNet 对单行小图的一次识别结果（CTC 解码后）。
struct TextLine {
    // 解码出的文字串（按 keys.txt 字表映射）
    std::string text;
    // 每个输出字符的置信度（与 text 逐字对应，已剔除 blank 重复帧）
    std::vector<float> charScores;
    // 本行 CRNN 推理耗时（ms）
    double time;
};

// ★ TextBlock：一个文本块的完整识别结果——把 det/cls/rec 三阶段对同一行的
//   输出聚合在一起，是 OcrResult.textBlocks 的元素类型。
struct TextBlock {
    // 文本框四顶点（原图坐标，已扣 padding）
    std::vector<cv::Point> boxPoint;
    // DbNet 检测框得分
    float boxScore;
    // 方向分类结果（0/1/-1，含义同 Angle.index）
    int angleIndex;
    // 方向分类置信度
    float angleScore;
    // 方向分类耗时（ms）
    double angleTime;
    // CrnnNet 识别出的文字
    std::string text;
    // 逐字符置信度
    std::vector<float> charScores;
    // CRNN 识别耗时（ms）
    double crnnTime;
    // 本块总耗时 = angleTime + crnnTime（不含 det 分摊）
    double blockTime;
};

// ★ OcrResult：一次完整 detect() 调用的总输出。
//   OcrLiteImpl::detect 核心流程在结尾聚合生成，经 OcrLite 门面返回给调用方
//   （HmFish 中即 RapidOCR.cpp 的工作线程，再转 OcrBox 抛给 ArkTS UI 绘制）。
struct OcrResult {
    // DbNet 检测阶段总耗时（ms，含前处理+推理+DB 后处理）
    double dbNetTime;
    // 全部文本块的识别结果（按框出现顺序，已 reverse 过）
    std::vector<TextBlock> textBlocks;
    // 画了红色检测框的结果图（已裁回原始尺寸，扣掉 padding 白边）
    cv::Mat boxImg;
    // 本次 detect 全流程总耗时（ms）
    double detectTime;
    // 便捷字段：所有文本块文字按行拼接（每块后补换行）
    std::string strRes;
};

#endif //__OCR_STRUCT_H__
