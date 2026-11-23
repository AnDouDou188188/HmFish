// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ TableNet —— 表格结构识别（TableNet.h）                                        ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】PP-Structure 表格结构识别（SLANet onnx，RapidTable 同款模型/解码）： ║
// ║   图像 → 结构 token 流 + 单元格逻辑网格（行/列/跨行/跨列）→ 与 OCR 文本      ║
// ║   行 IoU+L1 匹配 → 组装 JSON/HTML（buildTableJson）。                        ║
// ║                                                                              ║
// ║ 【模型规格】输入 488x488（等比缩放 + ImageNet normalize + 右/下补零）；      ║
// ║   输出结构概率序列（50 类：sos + 48 词表 + eos）+ 每位置 8 坐标 bbox 回归。  ║
// ║                                                                              ║
// ║ 【线程模型】libocr.so 侧表格 worker 线程同步调用（由                          ║
// ║   ocr_recognize_table_ex 触发）；单次调用内 预处理→推理→解码→匹配 全串行，   ║
// ║   无跨线程共享可变状态，天然线程安全。                                        ║
// ║                                                                              ║
// ║ 【与其它模块关系】ocr_c_api.h（ocr_recognize_table_ex 的底层实现）；          ║
// ║   OCR 检测/识别管线（OcrLine 文本行的来源）；                                 ║
// ║   YuvPipeline/ArkTS 经 OcrBridge dlsym 间接触发。                            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ TableNet 头文件：表格识别 C ABI 声明
#ifndef TABLE_NET_H
#define TABLE_NET_H

#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

/* PP-Structure 表格结构识别（SLANet onnx，RapidTable 同款模型/解码）
 * - 输入 488x488（等比缩放 + normalize + 补零）
 * - 输出 structure 序列（50 类：sos + 48 词表 + eos）+ 每位置 8 坐标 bbox 回归
 * - 贪婪解码至 eos，<td* token 取回归框（归一化 → 原图像素）
 * - 单元格逻辑网格（行/列/跨行/跨列）照抄 RapidTable decode_one_logic_points
 * - 单元格文本匹配照抄 RapidTable TableMatch（IoU + L1 距离） */
namespace table_net {

    // ★ TableCell：一个表格单元格：模型输出的"物理事实"（像素框）+
    //   解码得到的"逻辑事实"（网格位置/跨行跨列/表头标记）+
    //   匹配得到的"内容事实"（OCR 文本），三者合起来即还原 HTML 表格
    // 所需的全部信息
    struct TableCell {
        int row = 0;        // 起始行
        int col = 0;        // 起始列
        int rowspan = 1;
        int colspan = 1;
        bool header = false;    // 位于 <thead> 区域
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 像素框（原图坐标，左上/右下）
        std::string text;       // 匹配到的 OCR 文本（多行拼接）
    };

    // ★ OcrLine：一条 OCR 文本行（框 + 文本），表格匹配阶段的输入——
    //   由 OCR 检测/识别管线产出，与 TableCell 的像素框做 IoU+L1 匹配
    struct OcrLine {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 左上/右下（原图坐标）
        std::string text;
    };

    // ★ TableNet：SLANet 会话封装。为什么用 void* 藏 Ort 指针：避免头文件
    //   泄漏 onnxruntime/OpenCV 重型头，缩短下游编译单元依赖链
    class TableNet {
    public:
        ~TableNet();

        // 就绪判断：会话建好即认为可推理
        bool isReady() const { return session_ != nullptr; }

        // 加载 SLANet onnx；词表优先读模型 metadata（key="character"），
        // 缺失时用内置 48 行词表兜底；校验输出维度一致
        // ★ init：模型加载 + 词表来源三级保障（metadata → 内置兜底）+
        //   输出定位（按最后一维 8=bbox / 类数=概率 自动认名，不依赖导出时的
        //   输出命名），任何一步失败即整体回退
        bool init(const std::string &modelPath, int numThreads);

        // bgr：原图（BGR）。tokens：结构 token 流；cells：单元格（含网格位置，不含文本）
        // ★ detect：结构识别主流程（预处理 488x488 → ONNX 推理 → 贪婪解码至
        //   eos → <td* 取回归框 → 还原行/列/跨行/跨列逻辑网格）；
        //   文本匹配不在本函数——由 buildTableJson 单独承担（关注点分离：
        //   纯视觉结构 vs 视觉+文本融合）
        bool detect(const cv::Mat &bgr,
                    std::vector<std::string> &tokens,
                    std::vector<TableCell> &cells,
                    std::string &errMsg);

    private:
        // ───────── Ort 句柄（void* 防头文件泄漏 onnxruntime 类型）─────────
        // Ort::Env（生命周期须覆盖 session，析构逆序释放）
        void *env_ = nullptr;       // Ort::Env（void* 避免 header 泄漏 onnxruntime 类型）
        // Ort::Session（SLANet 结构模型）
        void *session_ = nullptr;   // Ort::Session
        // 完整解码词表：sos + 48 词表 + eos（索引即模型类别 id）
        std::vector<std::string> chars_;  // sos + 词表 + eos
        // 推理 I/O tensor 名（init 时自动探测缓存）
        std::string inputName_;
        std::string bboxOutName_;
        std::string probOutName_;
        int numThreads_ = 4;
    };

    // 单元格-OCR 匹配 + 组装结果 JSON（照抄 RapidTable TableMatch + get_pred_html）
    // 返回 JSON：{"rows":R,"cols":C,"cells":[{"r","c","rs","cs","h","t"}],"html":"...",
    //            "boxed":"...","ms":N,"t_struct":N,"t_ocr":N}
    // ★ buildTableJson：视觉→结构的最后一步。流程：过滤表格上方游离文本 →
    //   逐 OCR 行找最佳单元格（IoU 优先、L1 距离次之）→ 重建 HTML
    //   （token 流 + 匹配文本按 </td> 序插入）→ 拼 JSON（rows/cols/cells/
    //   html/boxed/耗时），供 ArkTS 侧直接渲染表格
    std::string buildTableJson(const std::vector<std::string> &tokens,
                               std::vector<TableCell> &cells,
                               const std::vector<OcrLine> &ocrLines,
                               long long elapsedMs,
                               long long structMs, long long ocrMs,
                               const std::string &boxedPath);

    // ★ errorJson：统一错误出口（{"error":"..."}），消息经 JSON 转义，
    //   保证 ArkTS 侧 JSON.parse 永不因消息内容含引号/换行而炸
    // 错误 JSON：{"error":"..."}
    std::string errorJson(const std::string &msg);

}  // namespace table_net

#endif
