// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrLite.h —— ocrlite 对外门面（pImpl 惯用法）                                 ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】ocrlite 唯一的对外入口类：配置线程/GPU、initModels 加载三网、         ║
// ║   detect 跑完整流水线并返回 OcrResult；实现细节全部藏进 OcrLiteImpl，         ║
// ║   调用方（HmFish 的 RapidOCR.cpp）只依赖此头，符合 pImpl 编译防火墙。         ║
// ║ 【流水线位置】最上层门面：det(cls,rec) 的编排全部在 impl 的                   ║
// ║   detect(...) 里完成，本类仅转发调用。                                        ║
// ║ 【与 Android 原版差异】新增 setDetStageCallback 两阶段回调接口，              ║
// ║   其余接口签名一致。                                                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_LITE_H__
#define __OCR_LITE_H__

#include <string>
#include <functional>
#include "OcrStruct.h"

class OcrLiteImpl;

class OcrLite{
public:
    // ★ 构造：内部 new OcrLiteImpl
    OcrLite();

    // ★ 析构：释放 impl
    ~OcrLite();

    // ★ setNumThread：设置三网推理线程数（须在 initModels 前调）
    void setNumThread(int numOfThread);

    // ★ initLogger：控制台/中间图/结果图三类日志开关
    void initLogger(bool isConsole, bool isPartImg, bool isResultImg);

    // ★ enableResultTxt：识别结果落盘 txt（探鱼未启用）
    void enableResultTxt(const char *path, const char *imgName);

    // ★ setGpuIndex：三网统一挂 GPU（CPU 构建为空操作）
    void setGpuIndex(int gpuIndex);

    // ★ initModels：依次加载 det/cls/rec 三网与 keys 字表
    bool initModels(const std::string &detPath, const std::string &clsPath,
                    const std::string &recPath, const std::string &keysPath);

    // 两阶段懒加载：文本检测(det)完成后立即回调检测框（原始坐标，已扣除 padding），
    // 供 UI 先绘制灰色虚线框，识别(rec)完成后再补全文字
    void setDetStageCallback(std::function<void(const std::vector<std::vector<cv::Point>> &)> cb);

    // ★ Logger：OCRLITE 宏日志入口（转发 impl）
    void Logger(const char *format, ...);

    // ★ detect（路径版）：读图后委托 Mat 版 detect
    OcrResult detect(const char *path, const char *imgName,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle);

    // ★ detect（Mat 版）：HmFish 主入口——RapidOCR.cpp 以 BGR Mat 直调，
    //   传 padding=50 / maxSideLen=1024 / doAngle=false
    //   maxDetCandidates：det 候选框数量封顶（-1 不限）。非文字场景（人脸/
    //   皮肤纹理/水底乱石）DBNet 可能一次给出几十个"伪文本框"，每个都要跑
    //   一次 CRNN 逐行识别（数十 ms/行）——既拖慢出结果，又与 YOLO/人脸/
    //   SR 线程抢 CPU 造成互相延迟。>0 时按 det 得分降序保留前 N 个再恢复
    //   原有阅读顺序；实时识别路径传 20（正常票据行数上限），表格整区域
    //   OCR 不限（密集表格行数可能超过 20）
    OcrResult detect(const cv::Mat &mat,
                     int padding, int maxSideLen,
                     float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle,
                     int maxDetCandidates = -1);

private:
    // 实现指针（pImpl 惯用法，构造时 new、析构 delete）
    OcrLiteImpl* pImpl;
};

#endif //__OCR_LITE_H__
