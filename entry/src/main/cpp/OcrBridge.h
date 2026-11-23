// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrBridge —— libentry.so ↔ libocr.so 桥接层声明（OcrBridge.h）               ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【文件职责】OCR/SR 桥接头文件：libocr.so 动态加载封装（maybeTrigger 触发   ║
// ║   OCR、drawLastBoxes/getBoxes 叠加绘制、srUpscale/srScale AI 超分接口）      ║
// ║                                                                              ║
// ║ 【定位】YuvPipeline / napi_init 与 libocr.so 之间的薄封装：上层全部经       ║
// ║   本命名空间调用 OCR/表格/超分/人脸能力，统一处理"库未加载/未初始化时静默   ║
// ║   降级返回"的边界（空串/0/false），使业务代码不必到处做空指针检查。          ║
// ║                                                                              ║
// ║ 【线程模型】load/unload 由全局互斥串行（保证 dlopen 只发生一次）；           ║
// ║   引擎调用本身依赖 libocr.so 内部锁与自有 worker，本层不再加锁。             ║
// ║   实现见 OcrBridge.cpp（dlsym 函数指针表与 ocr_c_api.h 一一对应）。          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef OCR_BRIDGE_H
#define OCR_BRIDGE_H

#include "ocr_c_api.h"

#include <string>

/* libentry.so 侧的 OCR 桥接层：
 * 运行时 dlopen("libocr.so") + dlsym 拿到 C ABI 函数指针，
 * 使 libentry.so 不再 DT_NEEDED libonnxruntime.so / libocr.so，
 * 加载失败可通过 dlerror() 精确定位，且不阻塞 entry 模块注册。 */
namespace ocr_bridge {

    // ───────── 动态库加载（dlopen/dlsym） ─────────

    /* dlopen + dlsym 全部符号；已加载则直接返回 true */
    // ★ load：首次调用时加载 libocr.so 并解析全部 C ABI 符号，需在任何业务接口前
    //   调用（未加载时各业务接口静默返回失败值）；加载失败不重试（g_load_attempted）
    bool load();
    // ★ unload：卸载 libocr.so（dlclose）并清空函数指针表；当前流程基本不使用，
    //   保留用于运行时热替换 libocr.so 的调试场景
    void unload();

    // ★ isLoaded：libocr.so 是否已成功加载（不含模型初始化状态）
    bool isLoaded();

    // ───────── OCR 引擎生命周期 ─────────

    // ★ init：按模型目录初始化 OCR 引擎（det/cls/rec/keys），成功返回 true
    bool init(const std::string &modelDir);
    /* 多模型集：按显式文件路径初始化（paddle / xiaomi 等任意命名） */
    // ★ initPaths：切换模型集的显式路径初始化（napi 层 initOcrPaths 入口）
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath);
    // ★ release：释放 OCR 引擎（内部 join 推理 worker 耗时，napi 层已放入异步线程调用）
    void release();

    // ───────── OCR 运行参数与状态 ─────────

    /* 参数（滑条立即生效） */
    // ★ setTriggerInterval：周期识别间隔（ms，能耗模式分档）
    void setTriggerInterval(int ms);
    // ★ setDetParams：检出阈值/框置信度/扩展比三件套（滑条立即生效）
    void setDetParams(float dbScore, float dbBox, float unclip);
    // ★ setNumThreads：推理线程数（下次 init 生效）
    void setNumThreads(int n);
    // ★ isReady：OCR 引擎是否初始化完成
    bool isReady();
    // ★ isEnabled：识别开关状态
    bool isEnabled();
    // ★ setEnabled：识别总开关（关闭后 maybeTrigger 空操作）
    void setEnabled(bool enabled);

    // ★ getResult：最近一次识别的整段文本（未加载/无结果返回空串）
    std::string getResult();

    // ───────── 帧级触发与叠加绘制 ─────────

    // ★ maybeTrigger：管线每帧调用，由 libocr 内部节流/队列决定是否真正识别
    void maybeTrigger(const uint8_t *rgba, int w, int h);
    // ★ drawLastBoxes：把最近一次识别的文字框画到当前帧（叠加层）
    void drawLastBoxes(uint8_t *rgba, int w, int h);
    // ★ getBoxes：取最近一次识别的四边形文字框坐标（每框 8 float，返回框数）
    int getBoxes(float *ptsOut, int maxBoxes);
    // ★ getBoxText：取第 idx 个框的识别文本（越界/未加载返回空串）
    std::string getBoxText(int idx);
    // ★ OCR 识别进度（done/total），供 HUD"识别中"徽标显示
    void getProgress(int &done, int &total);

    /* ★ 表格结构识别（PP-Structure SLANet） */
    // ★ initTable：加载 SLANet 表格模型（独立于 OCR 三件套的生命周期）
    bool initTable(const std::string &tableModelPath);
    // ★ isTableReady：表格模型是否就绪
    bool isTableReady();
    // rgba 输入 → JSON（含 boxed 预览图路径）；outDir 写预览 JPEG（table_frame/table_boxed）；
    // eventCb 阶段回调（stage 1=采集帧 2=结构框图，可空）；structureFound 回写是否识别到表格结构
    // ★ recognizeTableEx：一帧完成结构识别 + 区域 OCR + 单元格匹配并输出 JSON；
    //   structureFound 供 napi 层实现"无框自动换帧重试"（最多 3 次）
    std::string recognizeTableEx(const uint8_t *rgba, int w, int h, const std::string &outDir,
                                 ocr_table_event_cb eventCb, void *userdata,
                                 bool &structureFound);

    /* ★ AI 超分（ESPCN/FSRCNN/QuickSRNetMedium/Real-ESRGAN，x2-x4，onnxruntime CPU 推理） */
    // ★ srInit：加载超分 ONNX 模型（napi 层 initSr 入口）
    bool srInit(const std::string &onnxPath, int numThreads);
    // ★ srIsReady：超分模型是否就绪
    bool srIsReady();
    // ★ srRelease：释放超分模型（回收 25MB 级推理缓冲）
    void srRelease();
    // RGBA → xN 超分 → RGBA（out 尺寸 = inW*N x inH*N，N 由 srScale 探测，容量调用方保证）
    // ★ srUpscale：单帧同步超分（SR 工作线程调用，输出倍数由模型决定）
    bool srUpscale(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);
    // 当前模型放大倍数（2/3/4），未就绪 0
    // ★ srScale：探测放大倍数（决定输出缓冲分配尺寸）
    int srScale();

    /* ★ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage 性别年龄 + 几何颜值） */
    // ★ faceInit：加载检测/关键点/性别年龄三个模型（napi 层 initFace 入口）
    bool faceInit(const std::string &detPath, const std::string &lmPath,
                  const std::string &gaPath, int numThreads);
    // ★ faceIsReady：人脸模型组是否就绪
    bool faceIsReady();
    // ★ faceRelease：释放人脸模型组
    void faceRelease();
    // 同步检测一帧：facesOut 每脸 223 float（布局见 ocr_c_api.h），返回脸数
    // ★ faceDetect：一帧同步检测（face 工作线程调用），223 float 布局见 ocr_c_api.h
    int faceDetect(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces);

}  // namespace ocr_bridge

#endif
