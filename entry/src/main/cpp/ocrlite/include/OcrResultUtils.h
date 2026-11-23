// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrResultUtils.h —— OcrResult → JNI 对象转换工具（仅 __JNI__ 构建生效）       ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】把 C++ 的 OcrResult 逐字段搬进 Java 的 OcrResult/TextBlock/           ║
// ║   TextBox 对象，供 Android 层直接消费。                                       ║
// ║ 【流水线位置】OCRLITE_JAVA 桥接层专用，主流程不经过。                         ║
// ║ 【HmFish 现状】CMake 未定义 __JNI__，本文件整体处于"被预处理剔除"状态，       ║
// ║   实际不参与编译——HmFish 用 NAPI 自行转换（RapidOCR.cpp 的 OcrBox），         ║
// ║   保留仅为与上游 Android 版保持文件对齐。                                     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifdef __JNI__
#ifndef __OCR_RESULT_UTILS_H__
#define __OCR_RESULT_UTILS_H__
#include <jni.h>
#include "OcrStruct.h"

// ★ OcrResultUtils：JNI 转换器（一次性使用：构造时完成全部转换）
class OcrResultUtils {
public:
    // ★ 构造：持有 JNIEnv 与 OcrResult 引用，立即触发全部字段转换
    OcrResultUtils(JNIEnv *env, OcrResult &ocrResult);

    // ★ 析构：清理局部引用
    ~OcrResultUtils();

    // ★ getJObject：取转换完成的 Java OcrResult 对象
    jobject getJObject();

private:
    // JNI 环境指针（调用线程所有）
    JNIEnv *jniEnv;
    // 转换产物：Java OcrResult 对象
    jobject jOcrResult;

    // ★ newJListClass：java/util/ArrayList 类引用
    jclass newJListClass();

    // ★ getListConstructor：ArrayList 构造器 methodID
    jmethodID getListConstructor(jclass clazz);

    // ★ getTextBlock：单个 TextBlock → Java TextBlock
    jobject getTextBlock(TextBlock &textBlock);

    // ★ getTextBlocks：全部 TextBlock → Java ArrayList
    jobject getTextBlocks(std::vector<TextBlock> &textBlocks);

    // ★ newJPoint：cv::Point → Android.graphics.Point
    jobject newJPoint(cv::Point &point);

    // ★ newJBoxPoint：四点框 → Java ArrayList<Point>
    jobject newJBoxPoint(std::vector<cv::Point> &boxPoint);

    // ★ newJScoreArray：置信度向量 → Java float[]
    jfloatArray newJScoreArray(std::vector<float> &scores);

};
#endif //__OCR_RESULT_UTILS_H__
#endif
