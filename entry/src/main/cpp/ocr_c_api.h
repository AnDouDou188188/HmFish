// ★ libocr.so C ABI 声明：OCR/表格识别/AI 超分统一 C 接口（供 dlsym 动态加载）
#ifndef OCR_C_API_H
#define OCR_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libocr.so 导出的 C ABI —— libentry.so 通过 dlopen/dlsym 调用 */

int ocr_init(const char *modelDir);
void ocr_release(void);
int ocr_is_ready(void);
int ocr_is_enabled(void);
void ocr_set_enabled(int enabled);

/* 触发一次异步识别（内部 800ms 节流 + 队列容量 1） */
void ocr_maybe_trigger(const uint8_t *rgba, int w, int h);

/* 在 RGBA 图上绘制最近一次识别的检测框（青色折线） */
void ocr_draw_last_boxes(uint8_t *rgba, int w, int h);

/* 拷贝最近一次识别的整段文本，返回实际拷贝字节数（含 '\0'），失败返回 0 */
int ocr_get_result(char *buf, int bufLen);

/* 拷贝最近一次识别的检测框坐标。ptsOut 每框 8 个 float (x0,y0..x3,y3)，
 * maxBoxes 为容量，返回实际框数 */
int ocr_get_boxes(float *ptsOut, int maxBoxes);

/* 拷贝第 idx 个框的识别文本，返回实际拷贝字节数（含 '\0'） */
int ocr_get_box_text(int idx, char *buf, int bufLen);

/* 识别进度：done=已出文字框数，total=总框数 */
void ocr_get_progress(int *done, int *total);

/* 多模型集：按显式 det/cls/rec/keys 文件路径初始化（任意命名） */
int ocr_init_paths(const char *detPath, const char *clsPath,
                   const char *recPath, const char *keysPath);

/* 定时识别间隔（ms，能耗模式可调） */
void ocr_set_trigger_interval(int ms);

/* det 阈值三件套：检出阈值 / 框置信度 / 文本框扩展比 */
void ocr_set_det_params(float dbScore, float dbBox, float unclip);

/* 推理线程数（下次 init 生效） */
void ocr_set_num_threads(int n);

/* ★ 表格结构识别（PP-Structure SLANet） */

/* 加载 SLANet 表格模型（独立于 OCR 模型生命周期） */
int ocr_table_init(const char *tableModelPath);

int ocr_table_is_ready(void);

/* 单帧同步识别：rgba 输入，JSON 输出（{"rows","cols","cells","html","ms"} 或 {"error"}），
 * 返回实际拷贝字节数（含 '\0'），失败返回 0 */
int ocr_recognize_table(const uint8_t *rgba, int w, int h, char *jsonOut, int bufLen);

/* ★ AI 超分（QuickSRNetSmall / Real-ESRGAN x4，onnxruntime CPU） */

/* 加载超分 ONNX（动态尺寸版），numThreads<=0 用 4；成功返回 1 */
int sr_init(const char *onnxPath, int numThreads);

int sr_is_ready(void);

void sr_release(void);

/* RGBA → xN 超分 → RGBA：in 为 inW x inH（偶数），out 为 inW*N x inH*N（容量调用方保证）；
 * 成功返回 1，失败 0（失败后建议走非 AI 路径兜底） */
int sr_upscale_rgba(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH);

/* 当前模型放大倍数（2/3/4）；未就绪返回 0 */
int sr_scale(void);

#ifdef __cplusplus
}
#endif

#endif
