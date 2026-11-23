// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ TableNet 实现 —— SLANet 表格结构识别全流程（TableNet.cpp）                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【流程】init（模型/词表加载）→ detect（预处理→ONNX 推理→贪婪解码→           ║
// ║   逻辑网格还原）→ buildTableJson（OCR 文本匹配→HTML/JSON 组装）。           ║
// ║   算法逐行对齐 RapidTable（TablePreprocess / TableLabelDecode /              ║
// ║   decode_one_logic_points / TableMatch / get_pred_html），保证与             ║
// ║   PP-Structure 官方推理结果一致。                                            ║
// ║                                                                              ║
// ║ 【容错设计】词表缺失→内置 48 行兜底；输出命名不固定→按最后一维               ║
// ║   （8=bbox、类数=结构概率）自动定位；无表格结构→中文 errMsg；                ║
// ║   全部错误经 errorJson 走 {"error":"..."} 统一出口。                         ║
// ║                                                                              ║
// ║ 【线程模型】由 libocr.so 表格 worker 同步调用（ocr_recognize_table_ex），    ║
// ║   无共享可变状态；OpenCV/onnxruntime 内部线程池由 numThreads_ 约束。         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 表格识别 C ABI 导出：TableNet 表格结构识别流程
#include "TableNet.h"

#include <hilog/log.h>
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "TableNet"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace table_net {

    // ───────── 常量与词表 ─────────

    // 模型输入边长：SLANet 训练分辨率 488x488（非 512/640，换错会精度崩塌）
    static const int kMaxLen = 488;

    // 归一化参数（照抄 RapidTable TablePreprocess；BGR 通道序 + ImageNet 常量）
    static const float kMean[3] = {0.485f, 0.456f, 0.406f};
    static const float kStd[3] = {0.229f, 0.224f, 0.225f};

    // 兜底词表：与 ch_ppstructure_mobile_v2_SLANet.onnx 内嵌 metadata 一致（48 行，
    // 已是 RapidTable merge 后的形态：<td></td> 在末尾、无 <td>）
    // ★ 为什么需要兜底：部分模型导出工具会丢弃 metadata，词表丢了模型就是
    //   "哑巴"——输出类别 id 无从映射成 HTML token
    static const char *kBuiltinDict[] = {
            "<thead>", "</thead>", "<tbody>", "</tbody>", "<tr>", "</tr>",
            "<td", ">", "</td>",
            " colspan=\"2\"", " colspan=\"3\"", " colspan=\"4\"", " colspan=\"5\"",
            " colspan=\"6\"", " colspan=\"7\"", " colspan=\"8\"", " colspan=\"9\"",
            " colspan=\"10\"", " colspan=\"11\"", " colspan=\"12\"", " colspan=\"13\"",
            " colspan=\"14\"", " colspan=\"15\"", " colspan=\"16\"", " colspan=\"17\"",
            " colspan=\"18\"", " colspan=\"19\"", " colspan=\"20\"",
            " rowspan=\"2\"", " rowspan=\"3\"", " rowspan=\"4\"", " rowspan=\"5\"",
            " rowspan=\"6\"", " rowspan=\"7\"", " rowspan=\"8\"", " rowspan=\"9\"",
            " rowspan=\"10\"", " rowspan=\"11\"", " rowspan=\"12\"", " rowspan=\"13\"",
            " rowspan=\"14\"", " rowspan=\"15\"", " rowspan=\"16\"", " rowspan=\"17\"",
            " rowspan=\"18\"", " rowspan=\"19\"", " rowspan=\"20\"",
            "<td></td>"
    };

    // ───────── 生命周期 ─────────

    // ★ ~TableNet：逆序释放 session→env（Ort 要求 Env 活得比 Session 久），
    //   void* 转 delete 与 init 里 new 严格配对
    TableNet::~TableNet() {
        delete static_cast<Ort::Session *>(session_);
        delete static_cast<Ort::Env *>(env_);
        session_ = nullptr;
        env_ = nullptr;
    }

    // ───────── 词表工具 ─────────

    // ★ isTdOpener：判定 token 是否开启一个单元格（三种形态：
    //   <td（待拼属性）、<td>（旧版直接闭合）、<td></td>（merge 后的空单元格））
    static bool isTdOpener(const std::string &s) {
        return s == "<td" || s == "<td>" || s == "<td></td>";
    }

    // 词表构建：metadata（或内置兜底）→ merge（补 <td></td>、删 <td>）→ sos/eos 包夹
    // ★ buildCharList：对齐 RapidTable 的 merge 逻辑——
    //   模型输出的 <td></td> 是"空单元格"标记，必须在词表里；
    //   裸 <td> 与 <td></td> 语义重复且会破坏类别数一致性，删掉；
    //   首尾包夹 sos/eos 使类别数 = 词表数 + 2，与解码端 eosIdx/sosIdx 对应
    static void buildCharList(const std::vector<std::string> &rawDict,
                              std::vector<std::string> &out) {
        std::vector<std::string> dict = rawDict;
        bool hasSpan = false;
        for (size_t i = 0; i < dict.size(); i++) {
            if (dict[i] == "<td></td>") hasSpan = true;
        }
        if (!hasSpan) dict.push_back("<td></td>");
        for (size_t i = 0; i < dict.size(); i++) {
            if (dict[i] == "<td>") {
                dict.erase(dict.begin() + (long) i);
                break;
            }
        }
        out.clear();
        out.push_back("sos");
        for (size_t i = 0; i < dict.size(); i++) out.push_back(dict[i]);
        out.push_back("eos");
    }

    // ───────── 模型加载 ─────────

    // ★ TableNet::init：四步加载——建会话（线程数限幅 1~8）→ 读词表
    //   （metadata key="character" 优先，缺失用内置兜底）→ merge 成完整类别表
    //   → 按"最后一维=8 → bbox、=类数 → 概率"定位两个输出名。
    //   不硬编码输出名的原因：不同导出工具命名不一（如 sigmoid_0/tmp_0），
    //   按形状特征认名是唯一稳的做法；任何一步失败直接 false 不留半初始化状态
    bool TableNet::init(const std::string &modelPath, int numThreads) {
        if (isReady()) return true;
        numThreads_ = (numThreads >= 1 && numThreads <= 8) ? numThreads : 4;

        try {
            auto *env = new Ort::Env(ORT_LOGGING_LEVEL_ERROR, "TableNet");
            Ort::SessionOptions opt;
            opt.SetInterOpNumThreads(numThreads_);
            opt.SetIntraOpNumThreads(numThreads_);
            opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            auto *session = new Ort::Session(*env, modelPath.c_str(), opt);

            Ort::AllocatorWithDefaultOptions alloc;

            // 词表：优先读模型 metadata（key="character"），缺失用内置兜底
            std::vector<std::string> rawDict;
            {
                Ort::ModelMetadata meta = session->GetModelMetadata();
                Ort::AllocatedStringPtr v = meta.LookupCustomMetadataMapAllocated("character", alloc);
                if (v && v.get() && v.get()[0] != '\0') {
                    std::istringstream ss(std::string(v.get()));
                    std::string line;
                    while (std::getline(ss, line)) {
                        // 去 \r
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        rawDict.push_back(line);
                    }
                }
            }
            if (rawDict.empty()) {
                LOGI("no embedded character metadata, use builtin dict");
                const size_t n = sizeof(kBuiltinDict) / sizeof(kBuiltinDict[0]);
                rawDict.reserve(n);
                for (size_t i = 0; i < n; i++) rawDict.push_back(kBuiltinDict[i]);
            }
            buildCharList(rawDict, chars_);
            const int numClasses = (int) chars_.size();

            // 输入/输出定位：单输入；输出按最后一维区分（8=bbox，numClasses=结构概率）
            inputName_ = std::string(session->GetInputNameAllocated(0, alloc).get());
            bboxOutName_.clear();
            probOutName_.clear();
            const size_t outCount = session->GetOutputCount();
            for (size_t i = 0; i < outCount; i++) {
                const std::string name(session->GetOutputNameAllocated(i, alloc).get());
                auto info = session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo();
                auto shape = info.GetShape();
                if (shape.empty()) continue;
                const int64_t lastDim = shape.back();
                if (lastDim == 8 && bboxOutName_.empty()) {
                    bboxOutName_ = name;
                } else if (lastDim == numClasses && probOutName_.empty()) {
                    probOutName_ = name;
                }
            }
            if (bboxOutName_.empty() || probOutName_.empty()) {
                LOGE("init failed: output shapes mismatch (classes=%{public}d)", numClasses);
                delete session;
                delete env;
                return false;
            }

            env_ = env;
            session_ = session;
            LOGI("table model ready: classes=%{public}zu threads=%{public}d",
                 chars_.size(), numThreads_);
            return true;
        } catch (const std::exception &e) {
            LOGE("init exception: %{public}s", e.what());
            return false;
        } catch (...) {
            LOGE("init unknown exception");
            return false;
        }
    }

    // ───────── 结构推理与解码 ─────────

    // ★ TableNet::detect：结构识别主流程五步（全部在调用线程内同步完成）：
    //   ① 预处理：等比缩放至 488 内 + ImageNet normalize + 右/下补零
    //   ② 推理：一次 Run 同时取 bbox[seq,8] 与 结构概率[seq,类数] 双输出
    //   ③ 贪婪解码：逐位置 argmax，遇 eos 停；<td* 位置取回归框
    //      （归一化 8 坐标 → 原图像素外接矩形）
    //   ④ 逻辑网格：按 token 流（<tr>/<td/colspan/rowspan...>）走位，
    //      occupied 表处理跨行跨列占位，还原每个单元格的 row/col/rs/cs
    //   ⑤ 填回 cells（像素框 + 网格位置 + thead 标记）
    bool TableNet::detect(const cv::Mat &bgr,
                          std::vector<std::string> &tokens,
                          std::vector<TableCell> &cells,
                          std::string &errMsg) {
        tokens.clear();
        cells.clear();
        errMsg.clear();
        if (!isReady()) {
            errMsg = "表格模型未初始化";
            return false;
        }
        // 过小画面（<8px）无识别意义，直接拒绝
        if (bgr.empty() || bgr.cols < 8 || bgr.rows < 8) {
            errMsg = "画面为空";
            return false;
        }

        const int srcH = bgr.rows;
        const int srcW = bgr.cols;

        // ── 预处理（照抄 RapidTable TablePreprocess）──
        // 等比缩放：长边贴合 488，短边留白稍后补零——保持单元格横纵比，
        // 拉伸变形会让 bbox 回归与实际表格错位
        const float ratio = (float) kMaxLen / (float) std::max(srcH, srcW);
        const int rh = (int) (srcH * ratio);
        const int rw = (int) (srcW * ratio);
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(rw, rh));

        // CHW planar 布局：channel 平面连续，右/下自动留 0（补零=中性灰，
        // 与训练 padding 一致）；BGR 顺序 × ImageNet 常量逐通道归一化
        std::vector<float> input((size_t) 3 * kMaxLen * kMaxLen, 0.f);
        {
            cv::Mat fmat;
            resized.convertTo(fmat, CV_32F, 1.0 / 255.0);
            std::vector<cv::Mat> ch(3);
            cv::split(fmat, ch);
            for (int c = 0; c < 3; c++) {
                cv::Mat normed = (ch[c] - kMean[c]) / kStd[c];
                for (int y = 0; y < rh; y++) {
                    const float *src = normed.ptr<float>(y);
                    float *dst = input.data() + (size_t) c * kMaxLen * kMaxLen
                                 + (size_t) y * kMaxLen;
                    std::memcpy(dst, src, (size_t) rw * sizeof(float));
                }
            }
        }

        // ── 推理 ──
        // 双输出一次 Run 取回：bbox[seq,8]（每位置 8 归一化坐标）+
        // prob[seq,numClasses]（每位置结构类别概率）
        std::vector<float> bboxOut;   // [seq, 8]
        std::vector<float> probOut;   // [seq, numClasses]
        int seqLen = 0;
        try {
            auto *session = static_cast<Ort::Session *>(session_);
            auto *env = static_cast<Ort::Env *>(env_);
            const int64_t inShape[4] = {1, 3, kMaxLen, kMaxLen};
            auto memInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                    memInfo, input.data(), input.size(), inShape, 4);
            const char *inNames[] = {inputName_.c_str()};
            const char *outNames[] = {bboxOutName_.c_str(), probOutName_.c_str()};
            auto outs = session->Run(Ort::RunOptions{nullptr}, inNames, &inTensor, 1,
                                     outNames, 2);
            if (outs.size() != 2) {
                errMsg = "表格模型输出异常";
                return false;
            }
            // 再按形状认一次输出（防导出器改变输出顺序），
            // 同时从 bbox 形状拿序列长度 seqLen
            const float *bboxData = nullptr;
            const float *probData = nullptr;
            for (size_t i = 0; i < outs.size(); i++) {
                auto info = outs[i].GetTensorTypeAndShapeInfo();
                auto shape = info.GetShape();
                if (shape.size() != 3) continue;
                if (shape[2] == 8 && !bboxData) {
                    bboxData = outs[i].GetTensorData<float>();
                    seqLen = (int) shape[1];
                } else if (shape[2] == (int64_t) chars_.size() && !probData) {
                    probData = outs[i].GetTensorData<float>();
                }
            }
            if (!bboxData || !probData || seqLen <= 1) {
                errMsg = "表格模型输出异常";
                return false;
            }
            // 拷出 tensor 数据：Ort::Value 生命周期仅在本 try 块内，
            // 解码阶段还需使用，必须落到自有缓冲
            bboxOut.assign(bboxData, bboxData + (size_t) seqLen * 8);
            probOut.assign(probData, probData + (size_t) seqLen * chars_.size());
        } catch (const std::exception &e) {
            errMsg = "表格推理异常";
            LOGE("run exception: %{public}s", e.what());
            return false;
        }

        // ── 贪婪解码（照抄 RapidTable TableLabelDecode）──
        // 逐位置 argmax；idx>0 后遇 eos 停（idx==0 的 eos 是空识别噪声，
        // 跳过继续）；sos/eos 不入 token 流；<td* 位置额外取 bbox 回归框：
        // 8 归一化坐标（4 角点 xy 交替）×原图宽高 → 外接矩形
        const int numClasses = (int) chars_.size();
        const int eosIdx = numClasses - 1;
        const int sosIdx = 0;
        std::vector<float> cellRects;   // 每单元格 4 值（左上/右下，原图像素）
        for (int idx = 0; idx < seqLen; idx++) {
            const float *p = probOut.data() + (size_t) idx * numClasses;
            int best = 0;
            float bestV = p[0];
            for (int c = 1; c < numClasses; c++) {
                if (p[c] > bestV) {
                    bestV = p[c];
                    best = c;
                }
            }
            if (idx > 0 && best == eosIdx) break;
            if (best == sosIdx || best == eosIdx) continue;
            const std::string &text = chars_[best];
            if (isTdOpener(text)) {
                // 归一化坐标 → 原图像素（x*w / y*h），8 坐标 → 外接矩形
                const float *bb = bboxOut.data() + (size_t) idx * 8;
                float xs[4] = {bb[0] * srcW, bb[2] * srcW, bb[4] * srcW, bb[6] * srcW};
                float ys[4] = {bb[1] * srcH, bb[3] * srcH, bb[5] * srcH, bb[7] * srcH};
                cellRects.push_back(*std::min_element(xs, xs + 4));
                cellRects.push_back(*std::min_element(ys, ys + 4));
                cellRects.push_back(*std::max_element(xs, xs + 4));
                cellRects.push_back(*std::max_element(ys, ys + 4));
            }
            tokens.push_back(text);
        }

        const size_t cellCount = cellRects.size() / 4;
        if (cellCount == 0) {
            errMsg = "未识别到表格结构";
            return false;
        }

        // ── 逻辑网格（照抄 decode_one_logic_points：行/列/跨行/跨列 + thead 标记）──
        // 先把解码出的像素框按 <td* 出现序填进 cells（与 token 流严格对应），
        // 再单趟扫描 token 流走位：occupied 表记录已被占用的 (row,col)，
        // 跨列/跨行的格子将其覆盖区域全部标记占用，后续 <td 找到第一个
        // 空位落座——这就是 HTML 表格语义在解码端的完整还原
        cells.resize(cellCount);
        for (size_t i = 0; i < cellCount; i++) {
            cells[i].x0 = cellRects[i * 4];
            cells[i].y0 = cellRects[i * 4 + 1];
            cells[i].x1 = cellRects[i * 4 + 2];
            cells[i].y1 = cellRects[i * 4 + 3];
        }
        std::map<std::pair<int, int>, bool> occupied;
        int curRow = 0, curCol = 0;
        int maxRows = 0, maxCols = 0;
        bool inThead = false;
        size_t cellIdx = 0;
        size_t i = 0;
        while (i < tokens.size() && cellIdx < cellCount) {
            const std::string &tok = tokens[i];
            if (tok == "<thead>") {
                inThead = true;
            } else if (tok == "</thead>") {
                inThead = false;
            } else if (tok == "<tr>") {
                // 新行：列游标归零（行游标由 </tr> 推进）
                curCol = 0;
            } else if (tok == "</tr>") {
                curRow++;
            } else if (tok.rfind("<td", 0) == 0) {
                // <td 后可能跟随 colspan/rowspan 属性 token（直到 ">" 结束），
                // 解析数值（限幅 1~30 防模型幻觉出巨型跨格）
                int colspan = 1, rowspan = 1;
                if (tok != "<td></td>") {
                    size_t j = i + 1;
                    while (j < tokens.size() && tokens[j].rfind(">", 0) != 0) {
                        const size_t pos = tokens[j].find_first_of("0123456789");
                        if (pos != std::string::npos) {
                            const int v = atoi(tokens[j].c_str() + pos);
                            if (tokens[j].find("colspan=") != std::string::npos) {
                                if (v >= 1 && v <= 30) colspan = v;
                            } else if (tokens[j].find("rowspan=") != std::string::npos) {
                                if (v >= 1 && v <= 30) rowspan = v;
                            }
                        }
                        j++;
                    }
                    i = j;
                }
                // 跳过被上方 rowspan 占用的列，找到本行第一个空位
                while (occupied.count(std::make_pair(curRow, curCol))) curCol++;
                TableCell &c = cells[cellIdx++];
                c.row = curRow;
                c.col = curCol;
                c.rowspan = rowspan;
                c.colspan = colspan;
                c.header = inThead;
                // 标记整个跨行跨列覆盖区为占用
                for (int r = curRow; r < curRow + rowspan; r++)
                    for (int cc = curCol; cc < curCol + colspan; cc++)
                        occupied[std::make_pair(r, cc)] = true;
                curCol += colspan;
                maxRows = std::max(maxRows, curRow + rowspan);
                maxCols = std::max(maxCols, curCol);
            }
            i++;
        }
        (void) maxRows;
        (void) maxCols;
        return true;
    }

    // ───────── 匹配 + 结果组装（照抄 RapidTable TableMatch）─────────

    // ★ boxIoU：两框交并比（匹配主指标）：重叠越大越可能是同一单元格的文本
    static float boxIoU(const float a[4], const float b[4]) {
        const float left = std::max(a[0], b[0]);
        const float right = std::min(a[2], b[2]);
        const float top = std::max(a[1], b[1]);
        const float bottom = std::min(a[3], b[3]);
        if (left >= right || top >= bottom) return 0.f;
        const float inter = (right - left) * (bottom - top);
        const float s1 = (a[2] - a[0]) * (a[3] - a[1]);
        const float s2 = (b[2] - b[0]) * (b[3] - b[1]);
        return inter / (s1 + s2 - inter);
    }

    // L1 距离（照抄 distance：四角差绝对值之和 + 较小对角差）
    // ★ boxDistance：IoU 相同时的次级指标——衡量"框角点错位程度"，
    //   值越小文本行与单元格贴得越紧（IoU 平票时选贴得更准的那个）
    static float boxDistance(const float a[4], const float b[4]) {
        const float d0 = std::fabs(b[0] - a[0]) + std::fabs(b[1] - a[1]);
        const float d1 = std::fabs(b[2] - a[2]) + std::fabs(b[3] - a[3]);
        return d0 + d1 + std::min(d0, d1);
    }

    // ★ jsonEscape：JSON 字符串转义（引号/反斜杠/控制字符），
    //   保证 OCR 文本里出现任意字符都不会破坏 ArkTS 侧 JSON.parse
    static void jsonEscape(const std::string &in, std::string &out) {
        out.clear();
        out.reserve(in.size() + 8);
        for (unsigned char ch : in) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                        out += buf;
                    } else {
                        out += (char) ch;
                    }
            }
        }
    }

    // ★ appendCellText：单元格文本追加（多行 OCR 文本落进同一格时拼接）：
    //   去首尾空白后以单空格连接，空片段直接丢弃——与 get_pred_html 的
    //   content 拼接行为一致，避免单元格里出现多余换行/空格
    static void appendCellText(std::string &dst, const std::string &piece) {
        // 多行匹配同一单元格：非空内容以空格拼接（照抄 get_pred_html 的 content 拼接）
        std::string s = piece;
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\n')) b++;
        if (b > 0) s = s.substr(b);
        if (s.empty()) return;
        if (!dst.empty()) dst += " ";
        dst += s;
    }

    // ★ buildTableJson：匹配 + 组装总入口，五步：
    //   ① 求表格顶部基准线，过滤完全位于表格之上的游离文本（页面标题等）
    //   ② 逐 OCR 行在所有非空单元格中选最佳：IoU 优先、L1 距离次之，
    //      必须有实际重叠（IoU>1e-8）才认领
    //   ③ 由 row/col/rowspan/colspan 推得行列规模
    //   ④ 重建 HTML：token 流原样输出（剔除 thead/tbody 容器标签），
    //      文本按 </td> 出现序插入对应单元格
    //   ⑤ 拼 JSON：rows/cols/cells(r,c,rs,cs,h,t)/html/boxed/三个耗时字段
    std::string buildTableJson(const std::vector<std::string> &tokens,
                               std::vector<TableCell> &cells,
                               const std::vector<OcrLine> &ocrLines,
                               long long elapsedMs,
                               long long structMs, long long ocrMs,
                               const std::string &boxedPath) {
        if (cells.empty()) return errorJson("未识别到表格结构");

        // 1. 过滤表格上方游离文本（照抄 filter_ocr_result：完全位于最高单元格之上的行丢弃）
        float topY = -1.f;
        for (const auto &c : cells) {
            if (c.x1 - c.x0 < 0.5f && c.y1 - c.y0 < 0.5f) continue;  // 跳过空框
            topY = (topY < 0.f) ? c.y0 : std::min(topY, c.y0);
        }

        // 2. 逐 OCR 行匹配最佳单元格（IoU 优先、L1 距离次之，照抄 match_result）
        for (size_t li = 0; li < ocrLines.size(); li++) {
            const OcrLine &line = ocrLines[li];
            const float box[4] = {line.x0, line.y0, line.x1, line.y1};
            if (topY >= 0.f && line.y1 < topY) continue;
            int bestIdx = -1;
            float bestIoU = 0.f, bestDist = 0.f;
            for (size_t ci = 0; ci < cells.size(); ci++) {
                const TableCell &c = cells[ci];
                if (c.x1 - c.x0 < 0.5f && c.y1 - c.y0 < 0.5f) continue;  // 空框不参与
                const float cb[4] = {c.x0, c.y0, c.x1, c.y1};
                const float iou = boxIoU(box, cb);
                const float dist = boxDistance(box, cb);
                if (bestIdx < 0 || (1.f - iou) < (1.f - bestIoU) ||
                    ((1.f - iou) == (1.f - bestIoU) && dist < bestDist)) {
                    bestIdx = (int) ci;
                    bestIoU = iou;
                    bestDist = dist;
                }
            }
            // 必须有实际重叠（IoU > 1e-8）
            if (bestIdx < 0 || bestIoU <= 1e-8f) continue;
            appendCellText(cells[bestIdx].text, line.text);
        }

        // 3. 行列规模
        // 由各单元格的 (row+rowspan)/(col+colspan) 最大值推得，
        // 而非直接用 token 数——跨行跨列格会占多格，必须按覆盖区间算
        int rows = 0, cols = 0;
        for (const auto &c : cells) {
            rows = std::max(rows, c.row + c.rowspan);
            cols = std::max(cols, c.col + c.colspan);
        }
        if (rows <= 0 || cols <= 0) return errorJson("未识别到表格结构");

        // 4. HTML（照抄 get_pred_html：文本按 </td> 序插入，thead/tbody 过滤）
        // tdIdx 与 detect 阶段的 <td* 出现序严格对应——token 流决定结构顺序，
        // cells[tdIdx].text 提供该格内容，两路信息在此汇合
        std::string html;
        size_t tdIdx = 0;
        for (size_t t = 0; t < tokens.size(); t++) {
            const std::string &tag = tokens[t];
            if (tag.find("</td>") == std::string::npos) {
                if (tag == "<thead>" || tag == "</thead>" ||
                    tag == "<tbody>" || tag == "</tbody>") continue;
                html += tag;
                continue;
            }
            if (tag == "<td></td>") html += "<td>";
            if (tdIdx < cells.size()) html += cells[tdIdx].text;
            if (tag == "<td></td>") html += "</td>";
            else html += tag;
            tdIdx++;
        }
        html = "<html><body><table>" + html + "</table></body></html>";

        // 5. JSON
        // 字段全短名（r/c/rs/cs/h/t）减小体积；文本/HTML/路径一律 jsonEscape
        std::string esc;
        std::string json = "{";
        json += "\"rows\":" + std::to_string(rows);
        json += ",\"cols\":" + std::to_string(cols);
        json += ",\"cells\":[";
        for (size_t ci = 0; ci < cells.size(); ci++) {
            const TableCell &c = cells[ci];
            if (ci > 0) json += ",";
            json += "{\"r\":" + std::to_string(c.row);
            json += ",\"c\":" + std::to_string(c.col);
            json += ",\"rs\":" + std::to_string(c.rowspan);
            json += ",\"cs\":" + std::to_string(c.colspan);
            json += ",\"h\":" + std::string(c.header ? "1" : "0");
            json += ",\"t\":\"";
            jsonEscape(c.text, esc);
            json += esc;
            json += "\"}";
        }
        json += "],\"html\":\"";
        jsonEscape(html, esc);
        json += esc;
        json += "\",\"boxed\":\"";
        jsonEscape(boxedPath, esc);
        json += esc;
        json += "\",\"ms\":" + std::to_string(elapsedMs);
        json += ",\"t_struct\":" + std::to_string(structMs);
        json += ",\"t_ocr\":" + std::to_string(ocrMs);
        json += "}";
        return json;
    }

    // ★ errorJson：统一错误出口；消息转义后包成 {"error":"..."}，
    //   与 buildTableJson 的成功 JSON 形态互斥，ArkTS 侧据 "error" 键分流
    std::string errorJson(const std::string &msg) {
        std::string esc;
        jsonEscape(msg, esc);
        return "{\"error\":\"" + esc + "\"}";
    }

}  // namespace table_net
