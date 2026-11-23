// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ RapidOCR 头文件（RapidOCR.h）—— 水下 OCR 引擎的 C++ 接口层                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】声明 OCR 引擎（DBNet 文本检测 + 可选方向分类 cls + CRNN 识别）的      ║
// ║   初始化、场景触发、结果查询、叠加绘制，以及 PP-Structure 表格识别接口。        ║
// ║   实现在 RapidOCR.cpp；底部 extern "C" 块（ocr_c_api.h）供 libentry dlsym。    ║
// ║                                                                                ║
// ║ 【管线全景】渲染线程每帧 maybeTrigger（200ms 节流 + 静帧门控 + 默认           ║
// ║   1500ms 最小间隔三重门控）→ QUEUE_CAP=1 替换式队列 → worker 线程：降采样      ║
// ║   长边 1024 → 自适应增强 → DBNet det（先发布灰色虚线框）→ cls（可选）→        ║
// ║   CRNN rec → publish 终稿；渲染线程 drawLastBoxes 把框画回显示帧               ║
// ║   （框坐标已乘 g_box_scale 从工作图还原到显示帧坐标系）。                       ║
// ║                                                                                ║
// ║ 【线程约定】maybeTrigger/drawLastBoxes/查询接口在渲染线程调用；               ║
// ║   init/release/setter 可在任意线程调用；内部全部原子量/锁保护                  ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
// ★ RapidOCR 头文件：OCR 引擎 C ABI 声明
#ifndef RAPID_OCR_WRAPPER_H
#define RAPID_OCR_WRAPPER_H

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "ocr_c_api.h"

namespace rapidocr_wrapper {

    // ★ OcrBox：一个文本区域的识别结果。
    //   pts 是 DBNet 检出的四边形顶点（非轴对齐矩形——水下票据/标签常有
    //   透视倾斜，四边形贴边更准）；坐标已还原到显示帧坐标系，可直接绘制
    struct OcrBox {
        // 四边形顶点（通常 4 个；<4 视为退化框，消费方需跳过）
        std::vector<cv::Point> pts;
        // CRNN 识别出的文字（pending 阶段为空）
        std::string text;
        // det 框置信度（DBNet 概率图得分，pending 阶段为 0）
        float score;
        // true = det 阶段先发布的灰色虚线框（文字识别中）
        bool pending = false;
    };

    // ★ init：按默认 PaddleOCR 文件名从模型目录加载 det/cls/rec/keys 四件套
    // ★ release：停 worker 线程并清空模型/队列/结果（可重新 init 做模型热切换）
    // ★ isReady：模型是否加载成功（与开关无关）
    // ★ isOCREnabled：ready 且开关打开——业务侧判断"OCR 真在工作"用这个
    // ★ setOCREnabled：运行开关。关闭清队列与旧结果；打开重置静帧门控、
    //   强制立即识别一次（不用等画面变化）
    bool init(const std::string& modelDir);
    void release();
    bool isReady();
    bool isOCREnabled();
    void setOCREnabled(bool enabled);

    // ★ maybeTrigger：渲染线程每帧调用。三重门控（① 200ms 场景检查节流
    //   ② 静帧门控 ③ 默认 1500ms 最小触发间隔）全过才把帧克隆入
    //   QUEUE_CAP=1 替换式队列。绝大多数调用在前两道门被拦下，可放心每帧调
    void maybeTrigger(const cv::Mat& rgba);

    // ★ getResult：最近一次识别全文（文本块以 \n 拼接；禁用/未识别返回空）
    // ★ getLastBoxes：拷贝最近一次的框列表（含 pending 虚线框）
    // ★ drawLastBoxes：把最近结果画到显示帧——pending 框灰色虚线、
    //   完成框 5 色轮换实线；渲染线程每帧调用
    std::string getResult();
    void getLastBoxes(std::vector<OcrBox>& out);
    void drawLastBoxes(cv::Mat& rgba);

    // ★ initTable：加载 SLANet 表格结构模型（独立于 OCR 模型生命周期，可单独就绪）
    bool initTable(const std::string& tableModelPath);
    // ★ isTableReady：表格模型是否就绪
    bool isTableReady();
    /* ★ 表格结构识别（PP-Structure SLANet）：单帧同步识别（增强版）——
     * 工作图降采样 1024 + 表格区域裁剪 OCR 提速；预览图落 outDir 并经 cb 阶段回调；
     * structureFound 回写是否识别到表格结构（0=无框，供换帧重试）；
     * 返回 JSON（含 "boxed"/"t_struct"/"t_ocr"，或 {"error"}）；
     * 注意为同步执行（调用线程内跑完 SLANet+OCR 才返回），勿在渲染线程调用 */
    std::string recognizeTableEx(const cv::Mat& bgrFull, const std::string& outDir,
                                 ocr_table_event_cb cb, void* userdata,
                                 int& structureFound, std::string& boxedPathOut);

}  // namespace rapidocr_wrapper

#endif
