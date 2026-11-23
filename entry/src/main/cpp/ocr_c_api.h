// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ libocr.so C ABI 声明（ocr_c_api.h）                                          ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【文件职责】libocr.so 导出的统一 C 接口：OCR 文字识别（RapidOCR/PPOCR）／   ║
// ║   表格结构识别（PP-Structure SLANet）／AI 超分（QuickSRNet/Real-ESRGAN）／  ║
// ║   人脸识别（SCRFD + 2d106det + genderage）四组能力的纯 C 函数签名。         ║
// ║                                                                              ║
// ║ 【为什么是纯 C ABI】libentry.so 编译期不链接 libocr.so（无 DT_NEEDED），     ║
// ║   改为运行时 dlopen + dlsym 动态加载（见 OcrBridge.cpp）：                   ║
// ║   ① 避免 onnxruntime 等重依赖被拖进主 .so 的加载链路，加快启动；             ║
// ║   ② 加载失败可用 dlerror() 精确定位，entry 模块照常注册不崩溃；              ║
// ║   ③ C 符号不做 C++ 名字修饰，两侧可独立编译、独立演进。                      ║
// ║   本文件声明的每个函数都与 OcrBridge.cpp 里 OcrApi 函数指针表一一对应。      ║
// ║                                                                              ║
// ║ 【调用链】libentry.so（YuvPipeline / napi_init）                             ║
// ║   → ocr_bridge 命名空间封装（OcrBridge.h/.cpp，含加载失败静默降级）          ║
// ║   → dlsym 函数指针 → libocr.so 内部实现（模型推理在 libocr 自有 worker 线程）║
// ║   接口约定：int 返回值多为"成功/就绪标志"（1 成功、0 失败），                 ║
// ║   文本类结果一律由调用方出缓冲、接口回拷（避免跨 .so 释放堆内存）。          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef OCR_C_API_H
#define OCR_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libocr.so 导出的 C ABI —— libentry.so 通过 dlopen/dlsym 调用 */

// ───────── OCR 引擎生命周期 ─────────

// ★ ocr_init：按模型目录初始化 OCR 引擎（目录内含 det/cls/rec/keys 文件），成功返回 1
int ocr_init(const char *modelDir);
// ★ ocr_release：释放 OCR 引擎（内部会 join 推理 worker，可能阻塞数秒，调用方勿在主线程同步调用）
void ocr_release(void);
// ★ ocr_is_ready：引擎是否已初始化可用（模型加载完成）
int ocr_is_ready(void);
// ★ ocr_is_enabled：识别开关是否打开（由 ocr_set_enabled 控制）
int ocr_is_enabled(void);
// ★ ocr_set_enabled：识别总开关（关闭后 maybe_trigger 变为空操作，省电）
void ocr_set_enabled(int enabled);

// ───────── 定时识别触发与画面叠加 ─────────

/* 触发一次异步识别（内部 800ms 节流 + 队列容量 1） */
// ★ ocr_maybe_trigger：管线每帧调用——由 libocr 内部按节流/队列决定是否真正入队识别，
//   推理在其自有 worker 线程执行，绝不阻塞渲染线程
void ocr_maybe_trigger(const uint8_t *rgba, int w, int h);

/* 在 RGBA 图上绘制最近一次识别的检测框（青色折线） */
// ★ ocr_draw_last_boxes：把最近一次识别的文字框画到当前帧（叠加层的一部分）
void ocr_draw_last_boxes(uint8_t *rgba, int w, int h);

// ───────── 识别结果与进度 ─────────

/* 拷贝最近一次识别的整段文本，返回实际拷贝字节数（含 '\0'），失败返回 0 */
// ★ ocr_get_result：文本结果由调用方出缓冲回拷（跨 .so 不交出堆指针）
int ocr_get_result(char *buf, int bufLen);

/* 拷贝最近一次识别的检测框坐标。ptsOut 每框 8 个 float (x0,y0..x3,y3)，
 * maxBoxes 为容量，返回实际框数 */
// ★ ocr_get_boxes：四边形文字框坐标（顺时针四角），供叠加绘制/结果对位
int ocr_get_boxes(float *ptsOut, int maxBoxes);

/* 拷贝第 idx 个框的识别文本，返回实际拷贝字节数（含 '\0'） */
// ★ ocr_get_box_text：按框索引取单框文字（HUD 逐框展示用）
int ocr_get_box_text(int idx, char *buf, int bufLen);

/* 识别进度：done=已出文字框数，total=总框数 */
// ★ ocr_get_progress：识别进度（出参形式返回，供"识别中"徽标）
void ocr_get_progress(int *done, int *total);

// ───────── 多模型集与运行参数 ─────────

/* 多模型集：按显式 det/cls/rec/keys 文件路径初始化（任意命名） */
// ★ ocr_init_paths：切换模型集（paddle/xiaomi 等任意命名文件）的显式路径初始化
int ocr_init_paths(const char *detPath, const char *clsPath,
                   const char *recPath, const char *keysPath);

/* 定时识别间隔（ms，能耗模式可调） */
// ★ ocr_set_trigger_interval：周期识别的时间间隔，能耗模式分档调节
void ocr_set_trigger_interval(int ms);

/* det 阈值三件套：检出阈值 / 框置信度 / 文本框扩展比 */
// ★ ocr_set_det_params：检出灵敏度参数（滑条立即生效）
void ocr_set_det_params(float dbScore, float dbBox, float unclip);

/* 推理线程数（下次 init 生效） */
// ★ ocr_set_num_threads：OCR 推理线程数（重新 init 后生效）
void ocr_set_num_threads(int n);

/* ★ 表格结构识别（PP-Structure SLANet） */

/* 加载 SLANet 表格模型（独立于 OCR 模型生命周期） */
// ★ ocr_table_init：加载表格结构模型（与 OCR 三件套互不影响，可独立装卸）
int ocr_table_init(const char *tableModelPath);

// ★ ocr_table_is_ready：表格模型是否已加载就绪
int ocr_table_is_ready(void);

/* 表格识别阶段事件回调：stage 1=采集帧 JPEG 已生成，2=结构框标注图 JPEG 已生成
 * （jpegPath=文件路径）；userdata 原样透传，回调在识别线程内同步调用，需快速返回 */
// ★ ocr_table_event_cb：识别过程中的预览图事件回调类型定义——
//   因回调发生在 libocr 识别线程，跨线程投递到 JS 线程由调用方负责（见 napi tsfn 机制）
typedef void (*ocr_table_event_cb)(int stage, const char *jpegPath, void *userdata);

/* 单帧同步识别（增强版）：rgba 输入 → JSON 输出（{"rows","cols","cells","html",
 * "boxed","ms","t_struct","t_ocr"} 或 {"error"}）。
 * outDir：预览图输出目录（写入 table_frame.jpg / table_boxed.jpg）；
 * cb/userdata：阶段事件回调（可空）；structureFound 非空时回写是否识别到表格
 * 结构（0=无框，供调用方换帧重试）；返回实际拷贝字节数（含 '\0'），失败返回 0 */
// ★ ocr_recognize_table_ex：一帧完成"结构 + 区域 OCR + 单元格匹配"全流程；
//   structureFound 出参是"无框自动换帧重试"机制的关键（napi 层最多重试 3 次）
int ocr_recognize_table_ex(const uint8_t *rgba, int w, int h, const char *outDir,
                           ocr_table_event_cb cb, void *userdata,
                           int *structureFound, char *jsonOut, int bufLen);

/* ★ AI 超分（QuickSRNetSmall / Real-ESRGAN x4，onnxruntime CPU） */

/* 加载超分 ONNX（动态尺寸版），numThreads<=0 用 4；成功返回 1 */
// ★ sr_init：加载超分 ONNX 模型（支持动态输入尺寸，推理在调用线程同步执行）
int sr_init(const char *onnxPath, int numThreads);

// ★ sr_is_ready：超分模型是否已加载就绪
int sr_is_ready(void);

// ★ sr_release：释放超分模型（回收 25MB 级推理缓冲）
void sr_release(void);

/* RGBA → xN 超分 → RGBA：in 为 inW x inH（偶数），out 为 inW*N x inH*N（容量调用方保证）；
 * 成功返回 1，失败 0（失败后建议走非 AI 路径兜底） */
// ★ sr_upscale_rgba：单帧同步超分；调用方必须按 srScale() 探测的倍数预留输出容量
int sr_upscale_rgba(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);

/* 当前模型放大倍数（2/3/4）；未就绪返回 0 */
// ★ sr_scale：探测当前模型的放大倍数（决定输出缓冲尺寸）
int sr_scale(void);

/* ★ AI 黑白照片上色（DDColor-L，onnxruntime CPU fp32；黑白/棕褐色老照片 → 自然彩色） */

/* 加载上色 ONNX 模型（约 241MB，耗时数秒，napi 层已异步化）；成功返回 1 */
// ★ color_init：加载 DDColor-L 模型并探测 I/O 名与输入边长（推理在调用线程同步执行）
int color_init(const char *onnxPath, int numThreads);

// ★ color_is_ready：上色模型是否已加载就绪
int color_is_ready(void);

// ★ color_release：释放上色模型（回收 241MB 级模型内存）
void color_release(void);

/* RGBA → AI 上色 → RGBA（分辨率不变；in/out 等尺寸，允许同缓冲原地）。
 * 黑白/棕褐色照片输入时模型预测 Lab a/b 色度并与原图亮度合成自然彩色；
 * 成功返回 1，失败 0（调用方保留原图继续后续修复步骤） */
// ★ colorize_rgba：单张同步上色（restorePhoto 修复流水线在 napi 异步线程调用）
int colorize_rgba(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);

/* ★ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage 性别年龄 + 几何颜值分） */

/* 加载三个人脸模型（det/lm 必需，ga 可为 NULL）；numThreads<=0 用 3；成功返回 1 */
// ★ face_init：加载检测/关键点/性别年龄三个模型（ga 允许为空，仅 106 点模式）
int face_init(const char *detPath, const char *lmPath, const char *gaPath, int numThreads);

// ★ face_is_ready：人脸模型组是否已加载就绪
int face_is_ready(void);

// ★ face_release：释放人脸模型组
void face_release(void);

/* 同步检测一帧 RGBA。facesOut 每脸 228 个 float：
 *   [0..3] box x1,y1,x2,y2（图像坐标）
 *   [4]    检测分
 *   [5]    性别（0=女 1=男，-1=未知）
 *   [6]    年龄（-1=未知）
 *   [7]    颜值 0~100（-1=未知）
 *   [8..219] 106 关键点 x,y（图像坐标）
 *   [220..226] 颜值分项 0~100（-1=未知）：对称/眼形/眉形/鼻形/嘴形/三庭/眼型
 *   [227]  跨帧跟踪 id（libocr g_tracks 分配：同一张脸持续存在时不变，
 *          离场 3s 回收，再入场为新 id；上层配色/详情按它认脸）
 * 返回脸数（<=min(maxFaces,4)），未就绪返回 0 */
// ★ face_detect_rgba：一帧同步完成"检测 + 106 点 + 性别年龄 + 几何颜值 + 跟踪 id"；
//   固定布局的单块 float 输出避免跨 .so 传复杂结构，上层 FaceDrawInfo 再解包
int face_detect_rgba(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces);

#ifdef __cplusplus
}
#endif

#endif
