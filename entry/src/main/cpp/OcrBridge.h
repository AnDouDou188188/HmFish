// ★ OCR/SR 桥接头文件：libocr.so 动态加载封装（maybeTrigger 触发 OCR、
//   drawLastBoxes/getBoxes 叠加绘制、srUpscale/srScale AI 超分接口）
#ifndef OCR_BRIDGE_H
#define OCR_BRIDGE_H

#include "ocr_c_api.h"

#include <string>

/* libentry.so 侧的 OCR 桥接层：
 * 运行时 dlopen("libocr.so") + dlsym 拿到 C ABI 函数指针，
 * 使 libentry.so 不再 DT_NEEDED libonnxruntime.so / libocr.so，
 * 加载失败可通过 dlerror() 精确定位，且不阻塞 entry 模块注册。 */
namespace ocr_bridge {

    /* dlopen + dlsym 全部符号；已加载则直接返回 true */
    bool load();
    void unload();

    bool isLoaded();

    bool init(const std::string &modelDir);
    /* 多模型集：按显式文件路径初始化（paddle / xiaomi 等任意命名） */
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath);
    void release();

    /* 参数（滑条立即生效） */
    void setTriggerInterval(int ms);
    void setDetParams(float dbScore, float dbBox, float unclip);
    void setNumThreads(int n);
    bool isReady();
    bool isEnabled();
    void setEnabled(bool enabled);

    std::string getResult();

    void maybeTrigger(const uint8_t *rgba, int w, int h);
    void drawLastBoxes(uint8_t *rgba, int w, int h);
    int getBoxes(float *ptsOut, int maxBoxes);
    std::string getBoxText(int idx);
    // ★ OCR 识别进度（done/total），供 HUD"识别中"徽标显示
    void getProgress(int &done, int &total);

    /* ★ 表格结构识别（PP-Structure SLANet） */
    bool initTable(const std::string &tableModelPath);
    bool isTableReady();
    // rgba 输入 → JSON（含 boxed 预览图路径）；outDir 写预览 JPEG（table_frame/table_boxed）；
    // eventCb 阶段回调（stage 1=采集帧 2=结构框图，可空）；structureFound 回写是否识别到表格结构
    std::string recognizeTableEx(const uint8_t *rgba, int w, int h, const std::string &outDir,
                                 ocr_table_event_cb eventCb, void *userdata,
                                 bool &structureFound);

    /* ★ AI 超分（ESPCN/FSRCNN/QuickSRNetMedium/Real-ESRGAN，x2-x4，onnxruntime CPU 推理） */
    bool srInit(const std::string &onnxPath, int numThreads);
    bool srIsReady();
    void srRelease();
    // RGBA → xN 超分 → RGBA（out 尺寸 = inW*N x inH*N，N 由 srScale 探测，容量调用方保证）
    bool srUpscale(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);
    // 当前模型放大倍数（2/3/4），未就绪 0
    int srScale();

    /* ★ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage 性别年龄 + 几何颜值） */
    bool faceInit(const std::string &detPath, const std::string &lmPath,
                  const std::string &gaPath, int numThreads);
    bool faceIsReady();
    void faceRelease();
    // 同步检测一帧：facesOut 每脸 223 float（布局见 ocr_c_api.h），返回脸数
    int faceDetect(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces);

}  // namespace ocr_bridge

#endif
