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

    static const int kMaxLen = 488;

    // 归一化参数（照抄 RapidTable TablePreprocess；BGR 通道序 + ImageNet 常量）
    static const float kMean[3] = {0.485f, 0.456f, 0.406f};
    static const float kStd[3] = {0.229f, 0.224f, 0.225f};

    // 兜底词表：与 ch_ppstructure_mobile_v2_SLANet.onnx 内嵌 metadata 一致（48 行，
    // 已是 RapidTable merge 后的形态：<td></td> 在末尾、无 <td>）
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

    TableNet::~TableNet() {
        delete static_cast<Ort::Session *>(session_);
        delete static_cast<Ort::Env *>(env_);
        session_ = nullptr;
        env_ = nullptr;
    }

    static bool isTdOpener(const std::string &s) {
        return s == "<td" || s == "<td>" || s == "<td></td>";
    }

    // 词表构建：metadata（或内置兜底）→ merge（补 <td></td>、删 <td>）→ sos/eos 包夹
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
        if (bgr.empty() || bgr.cols < 8 || bgr.rows < 8) {
            errMsg = "画面为空";
            return false;
        }

        const int srcH = bgr.rows;
        const int srcW = bgr.cols;

        // ── 预处理（照抄 RapidTable TablePreprocess）──
        const float ratio = (float) kMaxLen / (float) std::max(srcH, srcW);
        const int rh = (int) (srcH * ratio);
        const int rw = (int) (srcW * ratio);
        cv::Mat resized;
        cv::resize(bgr, resized, cv::Size(rw, rh));

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
            bboxOut.assign(bboxData, bboxData + (size_t) seqLen * 8);
            probOut.assign(probData, probData + (size_t) seqLen * chars_.size());
        } catch (const std::exception &e) {
            errMsg = "表格推理异常";
            LOGE("run exception: %{public}s", e.what());
            return false;
        }

        // ── 贪婪解码（照抄 RapidTable TableLabelDecode）──
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
                curCol = 0;
            } else if (tok == "</tr>") {
                curRow++;
            } else if (tok.rfind("<td", 0) == 0) {
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
                while (occupied.count(std::make_pair(curRow, curCol))) curCol++;
                TableCell &c = cells[cellIdx++];
                c.row = curRow;
                c.col = curCol;
                c.rowspan = rowspan;
                c.colspan = colspan;
                c.header = inThead;
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

    // ── 匹配 + 结果组装（照抄 RapidTable TableMatch）──

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
    static float boxDistance(const float a[4], const float b[4]) {
        const float d0 = std::fabs(b[0] - a[0]) + std::fabs(b[1] - a[1]);
        const float d1 = std::fabs(b[2] - a[2]) + std::fabs(b[3] - a[3]);
        return d0 + d1 + std::min(d0, d1);
    }

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
        int rows = 0, cols = 0;
        for (const auto &c : cells) {
            rows = std::max(rows, c.row + c.rowspan);
            cols = std::max(cols, c.col + c.colspan);
        }
        if (rows <= 0 || cols <= 0) return errorJson("未识别到表格结构");

        // 4. HTML（照抄 get_pred_html：文本按 </td> 序插入，thead/tbody 过滤）
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

    std::string errorJson(const std::string &msg) {
        std::string esc;
        jsonEscape(msg, esc);
        return "{\"error\":\"" + esc + "\"}";
    }

}  // namespace table_net
