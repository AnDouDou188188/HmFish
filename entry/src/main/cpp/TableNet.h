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

    // 一个表格单元格：逻辑网格位置 + 像素框 + 匹配到的 OCR 文本
    struct TableCell {
        int row = 0;        // 起始行
        int col = 0;        // 起始列
        int rowspan = 1;
        int colspan = 1;
        bool header = false;    // 位于 <thead> 区域
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 像素框（原图坐标，左上/右下）
        std::string text;       // 匹配到的 OCR 文本（多行拼接）
    };

    // 一条 OCR 文本行（框 + 文本）
    struct OcrLine {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // 左上/右下（原图坐标）
        std::string text;
    };

    class TableNet {
    public:
        ~TableNet();

        bool isReady() const { return session_ != nullptr; }

        // 加载 SLANet onnx；词表优先读模型 metadata（key="character"），
        // 缺失时用内置 48 行词表兜底；校验输出维度一致
        bool init(const std::string &modelPath, int numThreads);

        // bgr：原图（BGR）。tokens：结构 token 流；cells：单元格（含网格位置，不含文本）
        bool detect(const cv::Mat &bgr,
                    std::vector<std::string> &tokens,
                    std::vector<TableCell> &cells,
                    std::string &errMsg);

    private:
        void *env_ = nullptr;       // Ort::Env（void* 避免 header 泄漏 onnxruntime 类型）
        void *session_ = nullptr;   // Ort::Session
        std::vector<std::string> chars_;  // sos + 词表 + eos
        std::string inputName_;
        std::string bboxOutName_;
        std::string probOutName_;
        int numThreads_ = 4;
    };

    // 单元格-OCR 匹配 + 组装结果 JSON（照抄 RapidTable TableMatch + get_pred_html）
    // 返回 JSON：{"rows":R,"cols":C,"cells":[{"r","c","rs","cs","h","t"}],"html":"...","ms":N}
    std::string buildTableJson(const std::vector<std::string> &tokens,
                               std::vector<TableCell> &cells,
                               const std::vector<OcrLine> &ocrLines,
                               long long elapsedMs);

    // 错误 JSON：{"error":"..."}
    std::string errorJson(const std::string &msg);

}  // namespace table_net

#endif
