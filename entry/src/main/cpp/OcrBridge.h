// ★ OCR/SR 桥接头文件：libocr.so 动态加载封装（maybeTrigger 触发 OCR、
//   drawLastBoxes/getBoxes 叠加绘制、srUpscale/srScale AI 超分接口）
#ifndef OCR_BRIDGE_H
#define OCR_BRIDGE_H

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
    // rgba 输入 → JSON（{"rows","cols","cells","html","ms"} 或 {"error"}）
    std::string recognizeTable(const uint8_t *rgba, int w, int h);

    /* ★ AI 超分（ESPCN/FSRCNN/QuickSRNetMedium/Real-ESRGAN，x2-x4，onnxruntime CPU 推理） */
    bool srInit(const std::string &onnxPath, int numThreads);
    bool srIsReady();
    void srRelease();
    // RGBA → xN 超分 → RGBA（out 尺寸 = inW*N x inH*N，N 由 srScale 探测，容量调用方保证）
    bool srUpscale(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);
    // 当前模型放大倍数（2/3/4），未就绪 0
    int srScale();

}  // namespace ocr_bridge

#endif
