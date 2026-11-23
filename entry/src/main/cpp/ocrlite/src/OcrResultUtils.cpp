// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrResultUtils.cpp —— OcrResult → Java 对象的 JNI 转换实现（Android 专用）    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】逐层把 C++ 结果映射到 com.benjaminwan.ocrlibrary 包：                 ║
// ║   OcrResult(dbNetTime, ArrayList<TextBlock>, detectTime, strRes)              ║
// ║   → TextBlock(ArrayList<Point>, boxScore, angleIndex, angleScore, ...)        ║
// ║   → Point(x, y) / float[] charScores。                                        ║
// ║ 【编译条件】整个实现被 __JNI__ 包裹；HmFish（HarmonyOS）的 CMake 未定义       ║
// ║   __JNI__，本文件编译为空，运行时不参与——保留仅为与上游 RapidOcrOnnx          ║
// ║   对账。HmFish 的对应链路是 RapidOCR.cpp 里"OcrResult → OcrBox → NAPI"。      ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifdef __JNI__
#include <OcrUtils.h>
#include "OcrResultUtils.h"

// ★ 构造函数：转换的总入口——查找 Java 侧 OcrResult 类并按签名
//   "(DLjava/util/ArrayList;DLjava/lang/String;)V"（dbNetTime, textBlocks,
//   detectTime, strRes）构造完整对象；任一类查找失败仅打印提示，
//   不抛异常（由调用方检查 NULL）
OcrResultUtils::OcrResultUtils(JNIEnv *env, OcrResult &ocrResult) {
    jniEnv = env;

    jclass jOcrResultClass = env->FindClass("com/benjaminwan/ocrlibrary/OcrResult");

    if (jOcrResultClass == NULL) {
        printf("OcrResult class is null\n");
    }

    jmethodID jOcrResultConstructor = env->GetMethodID(jOcrResultClass, "<init>",
                                                       "(DLjava/util/ArrayList;DLjava/lang/String;)V");

    jobject textBlocks = getTextBlocks(ocrResult.textBlocks);
    jdouble dbNetTime = (jdouble) ocrResult.dbNetTime;
    jdouble detectTime = (jdouble) ocrResult.detectTime;
    jstring jStrRest = jniEnv->NewStringUTF(ocrResult.strRes.c_str());

    jOcrResult = env->NewObject(jOcrResultClass, jOcrResultConstructor, dbNetTime,
                                textBlocks, detectTime, jStrRest);
}

// ★ 析构：仅置空 JNIEnv 指针（局部引用由 JNI 框架在返回原生方法时统一回收）
OcrResultUtils::~OcrResultUtils() {
    jniEnv = NULL;
}

// ★ getJObject：返回已构造好的 Java OcrResult 对象
jobject OcrResultUtils::getJObject() {
    return jOcrResult;
}

// ★ newJListClass：查找 java/util/ArrayList；失败打印并返回 NULL
jclass OcrResultUtils::newJListClass() {
    jclass clazz = jniEnv->FindClass("java/util/ArrayList");
    if (clazz == NULL) {
        printf("ArrayList class is null\n");
        return NULL;
    }
    return clazz;
}

// ★ getListConstructor：取 ArrayList 无参构造器（新建列表时用）
jmethodID OcrResultUtils::getListConstructor(jclass clazz) {
    jmethodID constructor = jniEnv->GetMethodID(clazz, "<init>", "()V");
    return constructor;
}

// ★ newJPoint：cv::Point → com.benjaminwan.ocrlibrary.Point(int x, int y)
jobject OcrResultUtils::newJPoint(cv::Point &point) {
    jclass clazz = jniEnv->FindClass("com/benjaminwan/ocrlibrary/Point");
    if (clazz == NULL) {
        printf("Point class is null\n");
        return NULL;
    }
    jmethodID constructor = jniEnv->GetMethodID(clazz, "<init>", "(II)V");
    jobject obj = jniEnv->NewObject(clazz, constructor, point.x, point.y);
    return obj;
}

// ★ newJBoxPoint：四点框 → Java ArrayList<Point>（逐点 newJPoint 后 add 入列）
jobject OcrResultUtils::newJBoxPoint(std::vector<cv::Point> &boxPoint) {
    jclass jListClass = newJListClass();
    jmethodID jListConstructor = getListConstructor(jListClass);
    jobject jList = jniEnv->NewObject(jListClass, jListConstructor);
    jmethodID jListAdd = jniEnv->GetMethodID(jListClass, "add", "(Ljava/lang/Object;)Z");

    for (auto point : boxPoint) {
        jobject jPoint = newJPoint(point);
        jniEnv->CallBooleanMethod(jList, jListAdd, jPoint);
    }
    return jList;
}

// ★ getTextBlock：TextBlock → Java TextBlock，签名为
//   "(Ljava/util/ArrayList;FIFDLjava/lang/String;[FDD)V"
//   （boxPoint, boxScore, angleIndex, angleScore, angleTime, text,
//    charScores[], crnnTime, blockTime），与 Java 侧构造器逐参对应
jobject OcrResultUtils::getTextBlock(TextBlock &textBlock) {
    jobject jBoxPint = newJBoxPoint(textBlock.boxPoint);
    jfloat jBoxScore = (jfloat) textBlock.boxScore;
    jfloat jAngleScore = (jfloat) textBlock.angleScore;
    jdouble jAngleTime = (jdouble) textBlock.angleTime;
    jstring jText = jniEnv->NewStringUTF(textBlock.text.c_str());
    jobject jCharScores = newJScoreArray(textBlock.charScores);
    jdouble jCrnnTime = (jdouble) textBlock.crnnTime;
    jdouble jBlockTime = (jdouble) textBlock.blockTime;
    jclass clazz = jniEnv->FindClass("com/benjaminwan/ocrlibrary/TextBlock");
    if (clazz == NULL) {
        printf("TextBlock class is null\n");
        return NULL;
    }
    jmethodID constructor = jniEnv->GetMethodID(clazz, "<init>",
                                                "(Ljava/util/ArrayList;FIFDLjava/lang/String;[FDD)V");
    jobject obj = jniEnv->NewObject(clazz, constructor, jBoxPint, jBoxScore, textBlock.angleIndex,
                                    jAngleScore, jAngleTime, jText, jCharScores, jCrnnTime,
                                    jBlockTime);
    return obj;
}

// ★ getTextBlocks：全部 TextBlock → Java ArrayList<TextBlock>（逐块转换后入列）
jobject OcrResultUtils::getTextBlocks(std::vector<TextBlock> &textBlocks) {
    jclass jListClass = newJListClass();
    jmethodID jListConstructor = getListConstructor(jListClass);
    jobject jList = jniEnv->NewObject(jListClass, jListConstructor);
    jmethodID jListAdd = jniEnv->GetMethodID(jListClass, "add", "(Ljava/lang/Object;)Z");

    for (int i = 0; i < textBlocks.size(); ++i) {
        auto textBlock = textBlocks[i];
        jobject jTextBlock = getTextBlock(textBlock);
        jniEnv->CallBooleanMethod(jList, jListAdd, jTextBlock);
    }
    return jList;
}

// ★ newJScoreArray：置信度 vector → Java float[]（NewFloatArray 后整块拷入）
jfloatArray OcrResultUtils::newJScoreArray(std::vector<float> &scores) {
    jfloatArray jScores = jniEnv->NewFloatArray(scores.size());
    jniEnv->SetFloatArrayRegion(jScores, 0, scores.size(), (jfloat *) scores.data());
    return jScores;
}

#endif