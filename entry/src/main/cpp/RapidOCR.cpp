// ★ RapidOCR 引擎 C ABI 导出：OCR 全流程（det/cls/rec）+ 结果查询 + 叠加绘制
#include "RapidOCR.h"
#include "OcrLite.h"
#include "OcrStruct.h"
#include "TableNet.h"
#include "ocr_c_api.h"

#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "RapidOCR"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace rapidocr_wrapper {

    static std::mutex g_ocr_mutex;
    static std::unique_ptr<OcrLite> g_ocr;
    static std::atomic<bool> g_ready(false);
    static std::atomic<bool> g_enabled(false);
    static std::atomic<long long> g_last_trigger_ms(0);
    // ★ OCR 可调参数（OcrSheet / PowerSheet，立即生效）：
    //   定时识别间隔 / det 阈值三件套 / 推理线程数（init 时应用）
    static std::atomic<int> g_trigger_interval_ms(4000);
    static std::atomic<float> g_db_score(0.5f);
    static std::atomic<float> g_db_box(0.3f);
    static std::atomic<float> g_unclip(1.6f);
    static std::atomic<int> g_num_threads(4);

    static std::mutex g_result_mutex;
    static std::string g_last_result;
    static std::vector<OcrBox> g_last_boxes;

    // ★ PP-Structure 表格结构识别（SLANet）：独立于 OCR 开关，init 后常驻
    static std::mutex g_table_mutex;
    static std::unique_ptr<table_net::TableNet> g_table;

    struct OcrJob {
        cv::Mat rgba;
    };

    static std::deque<OcrJob> g_queue;
    static std::mutex g_queue_mutex;
    static std::condition_variable g_queue_cv;
    static std::thread g_worker;
    static std::atomic<bool> g_running(false);

    static const size_t QUEUE_CAP = 1;

    static long long now_ms() {
        using namespace std::chrono;
        return (long long) duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
    }

    static void publish(const std::string &text, const std::vector<OcrBox> &boxes) {
        if (!g_enabled.load()) return;
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_last_result = text;
        g_last_boxes = boxes;
    }

    // 阶段一：det 完成，先发布灰色虚线框（文字尚未识别）
    static void publishPending(const std::vector<std::vector<cv::Point>> &detBoxes) {
        if (!g_enabled.load() || detBoxes.empty()) return;
        constexpr size_t kMaxBoxes = 30;
        std::vector<OcrBox> boxes;
        size_t n = detBoxes.size();
        if (n > kMaxBoxes) n = kMaxBoxes;
        boxes.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            OcrBox b;
            b.pts = detBoxes[i];
            b.score = 0.f;
            b.pending = true;
            boxes.push_back(std::move(b));
        }
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_last_result = "";
        g_last_boxes = std::move(boxes);
    }

    // 灰色虚线框绘制：沿四边形边缘按 on/off 交替画短线段
    static void drawDashedQuad(cv::Mat &rgba, const std::vector<cv::Point> &pts,
                               const cv::Scalar &color, int thickness) {
        constexpr int kOnLen = 12;
        constexpr int kOffLen = 8;
        for (size_t i = 0; i < pts.size(); ++i) {
            const cv::Point &a = pts[i];
            const cv::Point &b = pts[(i + 1) % pts.size()];
            double dx = b.x - a.x, dy = b.y - a.y;
            double len = std::sqrt(dx * dx + dy * dy);
            if (len < 1.0) continue;
            double ux = dx / len, uy = dy / len;
            double t = 0.0;
            bool on = true;
            while (t < len) {
                double step = std::min((double) (on ? kOnLen : kOffLen), len - t);
                if (on) {
                    cv::Point p1(a.x + (int) (ux * t), a.y + (int) (uy * t));
                    cv::Point p2(a.x + (int) (ux * (t + step)), a.y + (int) (uy * (t + step)));
                    cv::line(rgba, p1, p2, color, thickness, cv::LINE_AA);
                }
                t += step;
                on = !on;
            }
        }
    }

    static void worker_loop() {
        constexpr int kPadding = 50;
        constexpr int kMaxSideLen = 1024;
        constexpr size_t kMaxBoxes = 30;

        LOGI("ocr worker started");
        while (g_running.load()) {
            OcrJob job;
            {
                std::unique_lock<std::mutex> lk(g_queue_mutex);
                g_queue_cv.wait(lk, [] { return !g_queue.empty() || !g_running.load(); });
                if (!g_running.load() && g_queue.empty()) break;
                job = std::move(g_queue.front());
                g_queue.pop_front();
            }
            if (job.rgba.empty() || !g_enabled.load()) continue;

            cv::Mat bgr;
            try {
                if (job.rgba.channels() == 4)
                    cv::cvtColor(job.rgba, bgr, cv::COLOR_RGBA2BGR);
                else
                    bgr = job.rgba.clone();
            } catch (const cv::Exception &e) {
                LOGE("ocr cvtColor: %{public}s", e.what());
                continue;
            }

            OcrResult result;
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(g_ocr_mutex);
                if (!g_ocr || !g_enabled.load()) continue;
                try {
                    result = g_ocr->detect(bgr, kPadding, kMaxSideLen,
                                           g_db_score.load(), g_db_box.load(),
                                           g_unclip.load(), false, false);
                    ok = true;
                } catch (const std::exception &e) {
                    LOGE("ocr detect exception: %{public}s", e.what());
                } catch (...) {
                    LOGE("ocr detect unknown exception");
                }
            }
            if (!ok || !g_enabled.load()) continue;

            std::vector<OcrBox> boxes;
            std::string text;
            size_t n = result.textBlocks.size();
            if (n > kMaxBoxes) n = kMaxBoxes;
            boxes.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                const TextBlock &tb = result.textBlocks[i];
                OcrBox b;
                b.pts = tb.boxPoint;
                b.text = tb.text;
                b.score = tb.boxScore;
                if (b.pts.size() < 4) continue;
                boxes.push_back(std::move(b));
                if (!tb.text.empty()) {
                    if (!text.empty()) text += "\n";
                    text += tb.text;
                }
            }
            publish(text, boxes);
        }
        LOGI("ocr worker exited");
    }

    // 前置声明：init 转调 initPaths（默认 PaddleOCR 文件名）
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath);

    bool init(const std::string &modelDir) {
        if (g_ready.load()) return true;

        // ★ 默认 PaddleOCR 文件名（模型集目录形态，与 Android 端一致）
        return initPaths(modelDir + "/ch_PP-OCRv4_det_infer.onnx",
                         modelDir + "/ch_ppocr_mobile_v2.0_cls_infer.onnx",
                         modelDir + "/ch_PP-OCRv4_rec_infer.onnx",
                         modelDir + "/ppocr_keys_v1.txt");
    }


    // ★ 多模型集入口：det/cls/rec/keys 四个文件任意命名（config.json 或启发式扫描指定）
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath) {
        if (g_ready.load()) return true;

        LOGI("ocr init: det=%{public}s", detPath.c_str());
        LOGI("ocr init: cls=%{public}s", clsPath.c_str());
        LOGI("ocr init: rec=%{public}s", recPath.c_str());
        LOGI("ocr init: keys=%{public}s", keysPath.c_str());

        try {
            auto ocr = std::make_unique<OcrLite>();
            ocr->setNumThread(g_num_threads.load());
            ocr->initLogger(false, false, false);
            // 两阶段懒加载：det 完成立即发布灰色虚线框
            ocr->setDetStageCallback([](const std::vector<std::vector<cv::Point>> &detBoxes) {
                publishPending(detBoxes);
            });
            if (!ocr->initModels(detPath, clsPath, recPath, keysPath)) {
                LOGE("ocr initModels failed");
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(g_ocr_mutex);
                g_ocr = std::move(ocr);
            }
            g_running.store(true);
            g_worker = std::thread(worker_loop);
            g_ready.store(true);
            LOGI("ocr ready (threads=%{public}d)", g_num_threads.load());
            return true;
        } catch (const std::exception &e) {
            LOGE("ocr init exception: %{public}s", e.what());
            return false;
        } catch (...) {
            LOGE("ocr init unknown exception");
            return false;
        }
    }

    void release() {
        if (g_running.load()) {
            g_running.store(false);
            g_queue_cv.notify_all();
            if (g_worker.joinable()) g_worker.join();
        }
        {
            std::lock_guard<std::mutex> lk(g_queue_mutex);
            g_queue.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_ocr_mutex);
            g_ocr.reset();
        }
        {
            std::lock_guard<std::mutex> lk(g_result_mutex);
            g_last_result.clear();
            g_last_boxes.clear();
        }
        g_last_trigger_ms.store(0);
        g_enabled.store(false);
        g_ready.store(false);
        LOGI("ocr released");
    }

    bool isReady() { return g_ready.load(); }

    bool isOCREnabled() { return g_ready.load() && g_enabled.load(); }

    void setOCREnabled(bool enabled) {
        const bool prev = g_enabled.exchange(enabled);
        if (prev == enabled) return;
        if (!enabled) {
            {
                std::lock_guard<std::mutex> lk(g_queue_mutex);
                g_queue.clear();
            }
            g_last_trigger_ms.store(0);
            {
                std::lock_guard<std::mutex> lk(g_result_mutex);
                g_last_result.clear();
                g_last_boxes.clear();
            }
            LOGI("ocr disabled: queue/result cleared");
        } else {
            LOGI("ocr enabled");
        }
    }

    void maybeTrigger(const cv::Mat &rgba) {
        if (!g_ready.load() || !g_enabled.load() || rgba.empty()) return;

        const long long now = now_ms();
        const long long last = g_last_trigger_ms.load();
        // 定时识别：间隔可配（能耗模式 1~10s，默认 4s 对齐 Android setOCRTriggerInterval(4000)），
        // 低频节奏 + 结果保留显示，形成"延迟识别"效果
        if (now - last < g_trigger_interval_ms.load()) return;
        g_last_trigger_ms.store(now);

        OcrJob job;
        job.rgba = rgba.clone();

        {
            std::lock_guard<std::mutex> lk(g_queue_mutex);
            while (g_queue.size() >= QUEUE_CAP)
                g_queue.pop_front();
            g_queue.push_back(std::move(job));
        }
        g_queue_cv.notify_one();
    }

    std::string getResult() {
        if (!g_enabled.load()) return "";
        std::lock_guard<std::mutex> lk(g_result_mutex);
        return g_last_result;
    }

    /* ★ 参数 setter（滑条拖动立即生效，下次推理用新值） */
    void setTriggerInterval(int ms) {
        if (ms >= 500 && ms <= 20000) g_trigger_interval_ms.store(ms);
    }

    void setDetParams(float dbScore, float dbBox, float unclip) {
        if (dbScore >= 0.1f && dbScore <= 0.9f) g_db_score.store(dbScore);
        if (dbBox >= 0.1f && dbBox <= 0.9f) g_db_box.store(dbBox);
        if (unclip >= 1.0f && unclip <= 3.0f) g_unclip.store(unclip);
    }

    void setNumThreads(int n) {
        g_num_threads.store(n);
    }

    void getLastBoxes(std::vector<OcrBox> &out) {
        out.clear();
        if (!g_enabled.load()) return;
        std::lock_guard<std::mutex> lk(g_result_mutex);
        out = g_last_boxes;
    }

    // ★ 识别进度：done=已出文字的框数，total=总框数（供 HUD"识别中"徽标显示）
    void getProgress(int &done, int &total) {
        done = 0;
        total = 0;
        if (!g_enabled.load()) return;
        std::lock_guard<std::mutex> lk(g_result_mutex);
        total = (int) g_last_boxes.size();
        for (const auto &b : g_last_boxes) {
            if (!b.pending && !b.text.empty()) done++;
        }
    }

    /* ★ 表格结构识别（PP-Structure SLANet） */

    bool initTable(const std::string &tableModelPath) {
        std::lock_guard<std::mutex> lk(g_table_mutex);
        if (g_table && g_table->isReady()) return true;
        auto net = std::make_unique<table_net::TableNet>();
        if (!net->init(tableModelPath, g_num_threads.load())) {
            LOGE("table init failed: %{public}s", tableModelPath.c_str());
            return false;
        }
        g_table = std::move(net);
        LOGI("table ready: %{public}s", tableModelPath.c_str());
        return true;
    }

    bool isTableReady() {
        std::lock_guard<std::mutex> lk(g_table_mutex);
        return g_table && g_table->isReady();
    }

    // ★ 表格识别（增强版）：三个目标合一
    //   1) 提速：工作图降采样到长边 1024（det 上限本就 1024，SLANet 内部自行缩放 488，
    //      OCR 文本行来自该图 → 行宽缩短，逐行 CRNN 显著加速）
    //   2) 提速：OCR 只跑表格区域（cells 联合 bbox 外扩裁剪；框外文本行本就会被
    //      IoU 匹配过滤，全图 OCR 纯属浪费）
    //   3) 交互：采集帧 JPEG（stage 1）与结构框标注图 JPEG（stage 2）落 outDir 并经
    //      cb 回调，供弹窗实时显示被识别画面与 SLANet 结构框
    std::string recognizeTableEx(const cv::Mat &bgrFull, const std::string &outDir,
                                 ocr_table_event_cb cb, void *userdata,
                                 int &structureFound, std::string &boxedPathOut) {
        structureFound = 0;
        boxedPathOut.clear();
        if (bgrFull.empty()) return table_net::errorJson("画面为空");
        const long long t0 = now_ms();

        // ★ 提速①：统一降采样到长边 1024（AREA 插值，缩小时质量最优）
        cv::Mat bgr = bgrFull;
        constexpr int kWorkSide = 1024;
        const int maxSide = std::max(bgrFull.cols, bgrFull.rows);
        if (maxSide > kWorkSide) {
            const double s = (double) kWorkSide / (double) maxSide;
            cv::resize(bgrFull, bgr, cv::Size(), s, s, cv::INTER_AREA);
        }

        // ★ 交互 stage 1：采集帧 JPEG 先回传（弹窗 loading 态立即显示被识别画面）
        const std::string framePath = outDir + "/table_frame.jpg";
        const std::vector<int> jpegParams = {cv::IMWRITE_JPEG_QUALITY, 85};
        cv::imwrite(framePath, bgr, jpegParams);
        if (cb) cb(1, framePath.c_str(), userdata);

        // 1. 表格结构推理（SLANet）
        std::vector<std::string> tokens;
        std::vector<table_net::TableCell> cells;
        std::string errMsg;
        const long long tStruct0 = now_ms();
        {
            std::lock_guard<std::mutex> lk(g_table_mutex);
            if (!g_table || !g_table->isReady())
                return table_net::errorJson("表格模型未初始化");
            if (!g_table->detect(bgr, tokens, cells, errMsg))
                return table_net::errorJson(errMsg.empty() ? "表格识别失败" : errMsg);
        }
        const long long tStruct = now_ms() - tStruct0;
        structureFound = 1;

        // ★ 交互 stage 2：结构框标注图回传（绿色单元格框画在帧上，弹窗切"正在识别文字"）
        const std::string boxedPath = outDir + "/table_boxed.jpg";
        {
            cv::Mat boxed = bgr.clone();
            const int th = std::max(2, boxed.cols / 400);
            for (const auto &c : cells) {
                if (c.x1 - c.x0 < 0.5f && c.y1 - c.y0 < 0.5f) continue;  // 跳过空框
                cv::rectangle(boxed,
                              cv::Point((int) c.x0, (int) c.y0),
                              cv::Point((int) c.x1, (int) c.y1),
                              cv::Scalar(0, 220, 0), th, cv::LINE_AA);
            }
            cv::imwrite(boxedPath, boxed, jpegParams);
        }
        boxedPathOut = boxedPath;
        if (cb) cb(2, boxedPath.c_str(), userdata);

        // 2. ★ 提速②：表格区域裁剪 OCR——cells 联合 bbox 外扩 2%（容纳贴边文字）后裁剪，
        //    det/CRNN 输入与文本行数双降；行坐标加回裁剪偏移即工作图坐标
        float tx0 = 0.f, ty0 = 0.f, tx1 = 0.f, ty1 = 0.f;
        bool hasBox = false;
        for (const auto &c : cells) {
            if (c.x1 - c.x0 < 0.5f && c.y1 - c.y0 < 0.5f) continue;
            if (!hasBox) {
                tx0 = c.x0; ty0 = c.y0; tx1 = c.x1; ty1 = c.y1;
                hasBox = true;
            } else {
                tx0 = std::min(tx0, c.x0);
                ty0 = std::min(ty0, c.y0);
                tx1 = std::max(tx1, c.x1);
                ty1 = std::max(ty1, c.y1);
            }
        }
        std::vector<table_net::OcrLine> lines;
        const long long tOcr0 = now_ms();
        if (hasBox) {
            const float padX = std::max(4.f, (tx1 - tx0) * 0.02f);
            const float padY = std::max(4.f, (ty1 - ty0) * 0.02f);
            const int ox = std::max(0, (int) (tx0 - padX));
            const int oy = std::max(0, (int) (ty0 - padY));
            const int ow = std::min(bgr.cols - ox, (int) (tx1 - tx0 + 2 * padX) + 1);
            const int oh = std::min(bgr.rows - oy, (int) (ty1 - ty0 + 2 * padY) + 1);
            if (ow > 8 && oh > 8) {
                const cv::Mat crop = bgr(cv::Rect(ox, oy, ow, oh)).clone();
                std::lock_guard<std::mutex> lk(g_ocr_mutex);
                if (g_ocr) {
                    try {
                        // detect 返回的 boxPoint 已扣 padding（crop 内坐标），加回 (ox, oy) 即工作图坐标
                        OcrResult result = g_ocr->detect(crop, 50, 1024,
                                                         g_db_score.load(), g_db_box.load(),
                                                         g_unclip.load(), false, false);
                        lines.reserve(result.textBlocks.size());
                        for (const TextBlock &tb : result.textBlocks) {
                            if (tb.boxPoint.size() < 4 || tb.text.empty()) continue;
                            table_net::OcrLine line;
                            float x0 = (float) tb.boxPoint[0].x, y0 = (float) tb.boxPoint[0].y;
                            float x1 = x0, y1 = y0;
                            for (const cv::Point &p : tb.boxPoint) {
                                x0 = std::min(x0, (float) p.x);
                                y0 = std::min(y0, (float) p.y);
                                x1 = std::max(x1, (float) p.x);
                                y1 = std::max(y1, (float) p.y);
                            }
                            line.x0 = x0 + (float) ox;
                            line.y0 = y0 + (float) oy;
                            line.x1 = x1 + (float) ox;
                            line.y1 = y1 + (float) oy;
                            line.text = tb.text;
                            lines.push_back(std::move(line));
                        }
                    } catch (const std::exception &e) {
                        LOGE("table ocr detect exception: %{public}s", e.what());
                    } catch (...) {
                        LOGE("table ocr detect unknown exception");
                    }
                }
            }
        }
        const long long tOcr = now_ms() - tOcr0;

        // 3. 匹配 + 组装 JSON（含 boxed 预览图路径与阶段计时）
        const long long elapsed = now_ms() - t0;
        return table_net::buildTableJson(tokens, cells, lines, elapsed, tStruct, tOcr, boxedPath);
    }


    void drawLastBoxes(cv::Mat &rgba) {        if (rgba.empty() || rgba.type() != CV_8UC4) return;
        if (!g_enabled.load()) return;

        std::vector<OcrBox> boxes;
        {
            std::lock_guard<std::mutex> lk(g_result_mutex);
            boxes = g_last_boxes;
        }
        if (boxes.empty()) return;

        // ★ 对齐 Android：识别完成 5 色轮换实线 2px；待识别灰虚线 1px（薄一点，显轻）
        static const cv::Scalar kOcrColors[5] = {
                cv::Scalar(0, 255, 0, 255), cv::Scalar(0, 200, 255, 255),
                cv::Scalar(255, 120, 0, 255), cv::Scalar(255, 0, 255, 255),
                cv::Scalar(0, 255, 255, 255)
        };
        int idx = 0;
        // ★ 线宽随分辨率等比（1080p→3px），高分辨率下框线不细成发丝
        const int solidTh = std::max(2, rgba.cols / 360);
        const int dashTh = std::max(1, solidTh / 2);
        for (const auto &b : boxes) {
            if (b.pts.size() < 4) continue;
            if (b.pending) {
                // det 已完成、文字识别中：灰色虚线
                drawDashedQuad(rgba, b.pts, cv::Scalar(180, 180, 180, 220), dashTh);
            } else {
                // 识别完成：彩色实线多边形
                std::vector<std::vector<cv::Point>> pts{b.pts};
                cv::polylines(rgba, pts, true, kOcrColors[idx % 5], solidTh, cv::LINE_AA);
            }
            idx++;
        }
    }

    }  // namespace rapidocr_wrapper

// ═══════════ C ABI 导出（供 libentry.so dlsym 调用）═══════════
extern "C" {

    int ocr_init(const char *modelDir) {
        if (!modelDir) return 0;
        return rapidocr_wrapper::init(std::string(modelDir)) ? 1 : 0;
    }

    void ocr_release(void) { rapidocr_wrapper::release(); }

    int ocr_is_ready(void) { return rapidocr_wrapper::isReady() ? 1 : 0; }

    int ocr_is_enabled(void) { return rapidocr_wrapper::isOCREnabled() ? 1 : 0; }

    void ocr_set_enabled(int enabled) { rapidocr_wrapper::setOCREnabled(enabled != 0); }

    void ocr_maybe_trigger(const uint8_t *rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return;
        cv::Mat m(h, w, CV_8UC4, const_cast<uint8_t *>(rgba));
        rapidocr_wrapper::maybeTrigger(m);
    }

    void ocr_draw_last_boxes(uint8_t *rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return;
        cv::Mat m(h, w, CV_8UC4, rgba);
        rapidocr_wrapper::drawLastBoxes(m);
    }

    int ocr_get_result(char *buf, int bufLen) {
        if (!buf || bufLen <= 0) return 0;
        std::string s = rapidocr_wrapper::getResult();
        if (s.empty()) { buf[0] = '\0'; return 1; }
        size_t n = s.size();
        if ((int) n >= bufLen) n = (size_t) bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
        return (int) n + 1;
    }

    // ★ 识别进度 C API：done=已出文字框数，total=总框数
    void ocr_get_progress(int *done, int *total) {
        if (!done || !total) return;
        int d = 0, t = 0;
        rapidocr_wrapper::getProgress(d, t);
        *done = d;
        *total = t;
    }

    int ocr_get_boxes(float *ptsOut, int maxBoxes) {        if (!ptsOut || maxBoxes <= 0) return 0;
        std::vector<rapidocr_wrapper::OcrBox> boxes;
        rapidocr_wrapper::getLastBoxes(boxes);
        int n = (int) boxes.size();
        if (n > maxBoxes) n = maxBoxes;
        for (int i = 0; i < n; ++i) {
            const auto &b = boxes[i];
            if (b.pts.size() < 4) { for (int k = 0; k < 8; ++k) ptsOut[i * 8 + k] = 0.f; continue; }
            for (int k = 0; k < 4; ++k) {
                ptsOut[i * 8 + k * 2] = (float) b.pts[k].x;
                ptsOut[i * 8 + k * 2 + 1] = (float) b.pts[k].y;
            }
        }
        return n;
    }

    int ocr_get_box_text(int idx, char *buf, int bufLen) {
        if (!buf || bufLen <= 0 || idx < 0) return 0;
        std::vector<rapidocr_wrapper::OcrBox> boxes;
        rapidocr_wrapper::getLastBoxes(boxes);
        if (idx >= (int) boxes.size()) { buf[0] = '\0'; return 1; }
        const std::string &s = boxes[idx].text;
        if (s.empty()) { buf[0] = '\0'; return 1; }
        size_t n = s.size();
        if ((int) n >= bufLen) n = (size_t) bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
        return (int) n + 1;
    }

    /* ★ 多模型集/参数 C API（OcrSheet 模型选择 + 参数滑条，立即生效） */

    // 按显式文件路径初始化（多模型集：paddle / xiaomi 等任意命名）
    int ocr_init_paths(const char *detPath, const char *clsPath,
                       const char *recPath, const char *keysPath) {
        if (!detPath || !clsPath || !recPath || !keysPath) return 0;
        return rapidocr_wrapper::initPaths(std::string(detPath), std::string(clsPath),
                                           std::string(recPath), std::string(keysPath)) ? 1 : 0;
    }

    // 定时识别间隔（能耗模式，1~10s）
    void ocr_set_trigger_interval(int ms) {
        if (ms >= 500 && ms <= 20000) rapidocr_wrapper::setTriggerInterval(ms);
    }

    // det 阈值三件套：检出阈值 / 框置信度 / 文本框扩展比
    void ocr_set_det_params(float dbScore, float dbBox, float unclip) {
        rapidocr_wrapper::setDetParams(dbScore, dbBox, unclip);
    }

    // 推理线程数（下次 init 生效）
    void ocr_set_num_threads(int n) {
        if (n >= 1 && n <= 8) rapidocr_wrapper::setNumThreads(n);
    }

    /* ★ 表格结构识别（PP-Structure SLANet）C API */

    // 加载 SLANet 表格模型（独立于 OCR 模型生命周期）
    int ocr_table_init(const char *tableModelPath) {
        if (!tableModelPath) return 0;
        return rapidocr_wrapper::initTable(std::string(tableModelPath)) ? 1 : 0;
    }

    int ocr_table_is_ready(void) {
        return rapidocr_wrapper::isTableReady() ? 1 : 0;
    }

    // 单帧同步识别（增强版）：rgba 输入 → JSON 输出；预览图落 outDir 并经 cb 阶段回调，
    // structureFound 回写是否识别到表格结构（0=无框，供调用方换帧重试）
    int ocr_recognize_table_ex(const uint8_t *rgba, int w, int h, const char *outDir,
                               ocr_table_event_cb cb, void *userdata,
                               int *structureFound, char *jsonOut, int bufLen) {
        if (!rgba || w <= 0 || h <= 0 || !jsonOut || bufLen <= 0 || !outDir) return 0;
        cv::Mat m(h, w, CV_8UC4, const_cast<uint8_t *>(rgba));
        cv::Mat bgr;
        cv::cvtColor(m, bgr, cv::COLOR_RGBA2BGR);
        int found = 0;
        std::string boxed;
        const std::string json = rapidocr_wrapper::recognizeTableEx(
                bgr, std::string(outDir), cb, userdata, found, boxed);
        if (structureFound) *structureFound = found;
        size_t n = json.size();
        if ((int) n >= bufLen) n = (size_t) bufLen - 1;
        std::memcpy(jsonOut, json.data(), n);
        jsonOut[n] = '\0';
        return (int) n + 1;
    }

}  // extern "C"
