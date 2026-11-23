// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ RapidOCR 实现（RapidOCR.cpp）—— 水下 OCR 引擎全流程 + C ABI 导出              ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】DBNet 文本检测 + CRNN 文字识别（可选方向分类 cls）的完整封装：         ║
// ║   场景触发（maybeTrigger 三重门控）→ 替换式队列（QUEUE_CAP=1）→ worker 线程   ║
// ║   降采样/增强/det/cls/rec → 结果发布（publishPending 虚线框 → publish 终稿） ║
// ║   → 渲染线程 drawLastBoxes 叠框；另含 PP-Structure SLANet 表格识别。          ║
// ║                                                                                ║
// ║ 【触发链（maybeTrigger，渲染线程每帧调用）】                                   ║
// ║   ① 200ms 场景检查节流（g_last_check_ms）：场景比对不必每帧做                 ║
// ║   ② 静帧门控（sceneChanged）：64×64 灰度缩略图均值差 < 2.5 判"画面没动"，     ║
// ║      不推理（上次结果继续显示）→ 静止场景 0 推理                               ║
// ║   ③ 最小触发间隔（g_trigger_interval_ms，默认 1500ms，用户可调 0.5~20s）      ║
// ║   三关全过 → clone 帧入队（满则淘汰旧帧）→ notify worker                      ║
// ║                                                                                ║
// ║ 【坐标映射】worker 把显示帧降采样到长边 1024 得"工作图"（回放因子记入          ║
// ║   g_box_scale = 1/s）；det/rec 的框都是工作图坐标，发布前统一乘回              ║
// ║   g_box_scale 还原到显示帧坐标 → drawLastBoxes 直接可画。                     ║
// ║                                                                                ║
// ║ 【线程模型】渲染线程（maybeTrigger/drawLastBoxes/查询）+ OCR worker 线程       ║
// ║   （worker_loop）+ 表格调用线程（recognizeTableEx 同步执行）。共享状态全部    ║
// ║   原子量或锁保护：g_ocr_mutex 引擎互斥（worker 与表格 crop OCR 竞争）、       ║
// ║   g_result_mutex 结果读写、g_queue_* 队列、g_thumb_mutex 场景缩略图。          ║
// ║                                                                                ║
// ║ 【与其他模块关系】libentry.so 经 ocr_c_api.h 的 extern "C" 符号（本文件底部） ║
// ║   dlsym 调用；napi 侧/渲染管线在帧尾部调 ocr_maybe_trigger、                  ║
// ║   ocr_draw_last_boxes。识别引擎本体是 OcrLite（libocr），本文件只做调度/       ║
// ║   门控/坐标换算/绘制。                                                        ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
// ★ RapidOCR 引擎 C ABI 导出：OCR 全流程（det/cls/rec）+ 结果查询 + 叠加绘制
#include "RapidOCR.h"
#include "OcrLite.h"
#include "OcrStruct.h"
#include "TableNet.h"
#include "ocr_c_api.h"

#include <hilog/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
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

    // ───────── 全局状态区（g_ 前缀：跨线程共享，全为原子量或锁保护） ─────────

    // OCR 引擎互斥锁：用 timed_mutex 是因为 worker 用 try_lock_for(150ms)——
    // 表格识别占用引擎做 crop OCR 时，普通识别等 150ms 拿不到锁就放弃本轮，
    // 不会和表格识别互相卡死
    static std::timed_mutex g_ocr_mutex;
    // OCR 引擎单例：OcrLite 内含 det/cls/rec 三个 ONNX 模型 + 字典
    static std::unique_ptr<OcrLite> g_ocr;
    // 模型是否已加载成功（init 成功置 true，release 清 false）
    static std::atomic<bool> g_ready(false);
    // OCR 功能开关（用户 UI 控制；关闭后门控与发布全部短路）
    static std::atomic<bool> g_enabled(false);
    // 上次真正触发推理的时间戳（ms）：与 g_trigger_interval_ms 配合实现最小间隔
    static std::atomic<long long> g_last_trigger_ms(0);
    // ★ OCR 可调参数（OcrSheet / PowerSheet，立即生效）：
    //   两轮识别最小间隔（画面变化后的重识别节奏）/ det 阈值三件套 / 推理线程数（init 时应用）
    //   ★ 滞后修复：间隔默认 1500ms（原 4000ms 太迟钝），配合 200ms 场景检查节流——
    //     画面一动最多 1.5s 就重新识别，虚线框跟手；静止场景被静帧门控拦住，不会白跑
    static std::atomic<int> g_trigger_interval_ms(1500);
    static std::atomic<float> g_db_score(0.5f);
    static std::atomic<float> g_db_box(0.3f);
    static std::atomic<float> g_unclip(1.6f);
    static std::atomic<int> g_num_threads(4);
    // ★ 场景检查节流时间戳：场景比对 200ms 一次即可（64×64 缩略图很轻，无需每帧）
    static std::atomic<long long> g_last_check_ms(0);

    static std::mutex g_result_mutex;
    static std::string g_last_result;
    static std::vector<OcrBox> g_last_boxes;
    // ★ 框坐标回放因子：detect 输入是降采样后的工作图，返回框在工作图坐标系；
    //   drawLastBoxes 画在全分辨率显示帧上 → publishPending/final 框都要乘回 1/s。
    //   消费点有两处：① publishPending（det 回调线程内）② worker 终稿前显式乘；
    //   worker 在调 detect 前先 store 本值，det 回调在同线程内读，无竞态
    static std::atomic<double> g_box_scale(1.0);

    // ★ PP-Structure 表格结构识别（SLANet）：独立于 OCR 开关，init 后常驻；
    //   g_table_mutex 保护懒加载与 detect 期间的指针稳定性
    static std::mutex g_table_mutex;
    static std::unique_ptr<table_net::TableNet> g_table;

    // ───────── 异步任务队列：QUEUE_CAP=1 的"替换式"队列 ─────────
    // ★ 设计：容量固定 1，入队前把旧任务挤掉——推理慢时永远处理最新帧，
    //   旧帧识别完早已过时，积压毫无意义（丢帧不丢实时性）

    // 任务体：仅一帧克隆好的 RGBA（clone 在入队前由 maybeTrigger 完成）
    struct OcrJob {
        cv::Mat rgba;
    };

    // 任务队列（渲染线程入队 / worker 出队）
    static std::deque<OcrJob> g_queue;
    // 队列互斥锁
    static std::mutex g_queue_mutex;
    // 入队后 notify_one 唤醒 worker
    static std::condition_variable g_queue_cv;
    // OCR worker 线程（worker_loop）
    static std::thread g_worker;
    // worker 生命周期标志（false 退出循环）
    static std::atomic<bool> g_running(false);

    // 队列容量：1 = 替换式（maybeTrigger 里 while(size>=CAP) pop_front 淘汰旧帧）
    static const size_t QUEUE_CAP = 1;

    // ★ now_ms：当前系统时间（ms，epoch）。所有节流/间隔计时统一走它，
    //   避免不同时钟源混用导致间隔判断漂移
    static long long now_ms() {
        using namespace std::chrono;
        return (long long) duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count();
    }

    // ★ publish：发布"终稿"结果（识别完成的文字 + 框），整体替换上次结果
    //   （含虚线框）。禁用瞬间 worker 可能还在收尾，此时直接丢弃不发布
    static void publish(const std::string &text, const std::vector<OcrBox> &boxes) {
        if (!g_enabled.load()) return;
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_last_result = text;
        g_last_boxes = boxes;
    }

    // ★ publishPending：两阶段发布的第一阶段。det（DBNet）远快于 rec（CRNN 逐行），
    //   det 一完成就经 setDetStageCallback 抢先发布"空文字 + pending=true"的框，
    //   用户立即看到灰虚线知道"正在识别"，而不是干等全部文字；worker 结束后
    //   publish 终稿整体覆盖这批虚线框。上限 30 框防误检爆炸
    // 阶段一：det 完成，先发布灰色虚线框（文字尚未识别）
    // ★ det 回调的框在降采样工作图坐标系：先乘 g_box_scale 还原到显示帧坐标
    //   （回调在 worker 线程内同步执行，g_box_scale 已由 worker 提前 store，无竞态）
    static void publishPending(const std::vector<std::vector<cv::Point>> &detBoxes) {
        if (!g_enabled.load() || detBoxes.empty()) return;
        constexpr size_t kMaxBoxes = 30;
        const double inv = g_box_scale.load();
        std::vector<OcrBox> boxes;
        size_t n = detBoxes.size();
        if (n > kMaxBoxes) n = kMaxBoxes;
        boxes.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            OcrBox b;
            b.pts = detBoxes[i];
            if (inv != 1.0) {
                for (auto &pt : b.pts) {
                    pt.x = cvRound((double) pt.x * inv);
                    pt.y = cvRound((double) pt.y * inv);
                }
            }
            b.score = 0.f;
            b.pending = true;
            boxes.push_back(std::move(b));
        }
        std::lock_guard<std::mutex> lk(g_result_mutex);
        g_last_result = "";
        g_last_boxes = std::move(boxes);
    }

    // ★ drawDashedQuad：OpenCV 没有虚线原语，手工实现——沿四边形每条边按
    //   弧长参数 t 前进，画 12px 实线段、跳 8px 空白交替；LINE_AA 抗锯齿
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

    // ★ OpenCV5 场景门控：64×64 灰度缩略图比对（INTER_AREA + absdiff），
    //   静态场景不重复推理（上次结果保留显示）→ 大幅省电提速；
    //   缩略图只在触发推理时更新，缓慢连续移动也能累计越过阈值
    // ───────── 场景门控区：静帧检测（sceneChanged） ─────────

    // 上次推理时的 64×64 灰度缩略图（"基准帧"）；只在真正触发推理时更新，
    // 所以缓慢连续移动的差值能逐次累计、最终越过阈值
    static std::mutex g_thumb_mutex;
    static cv::Mat g_last_thumb;
    // "强制视为场景已变化"旁路：初始化/开关打开/参数修改后置 true，
    // 让下一次 maybeTrigger 无条件通过静帧门控（强制重识别一次）
    static std::atomic<bool> g_scene_reset(true);

    // ★ sceneChanged：判断"画面相对上次推理有没有动"。流程：64×64 INTER_AREA
    //   缩略（保留亮度结构、丢高频细节，HUD 水印等固定内容几乎不影响）→ 灰度
    //   → 与基准缩略图 absdiff → 均值 < 2.5/255 判静止（不推理，上次结果继续
    //   显示）→ 静止场景 0 推理，省电省 CPU
    static bool sceneChanged(const cv::Mat &rgba) {
        cv::Mat thumb;
        cv::resize(rgba, thumb, cv::Size(64, 64), 0, 0, cv::INTER_AREA);
        cv::Mat gray;
        cv::cvtColor(thumb, gray, cv::COLOR_RGBA2GRAY);
        std::lock_guard<std::mutex> lk(g_thumb_mutex);
        if (!g_scene_reset.load() && !g_last_thumb.empty()) {
            cv::Mat diff;
            cv::absdiff(gray, g_last_thumb, diff);
            // 均值差 < 2.5/255 视为画面未变（HUD 水印等固定内容不影响灰度差）；
            //   阈值偏高会让手持慢漂移迟迟不重识别 → 框滞后，2.5 兼顾噪声与响应
            if (cv::mean(diff)[0] < 2.5) return false;
        }
        // 走到这里说明"判定为变化"（或首次/被 reset）：更新基准缩略图供下次比对；
        // 同时清掉 reset 旁路，恢复正常门控
        g_last_thumb = gray;
        g_scene_reset.store(false);
        return true;
    }

    // ★ enhanceForOcr：为什么要在 det 之前做——DBNet 概率图对对比度极敏感，
    //   水下浑浊/暗光帧直接检测就是一片糊；两步全是 256 项 LUT 查表
    //   （O(像素数) 一次遍历，NEON 下开销可忽略），先增强再检测远比
    //   "检不出→反复重试"划算
    // ★ OpenCV5 自适应预处理（普通/票据两种模式共用，提升低质量帧识别率）：
    //   ① 暗光 gamma 提亮（LUT，NEON 加速）
    //   ② 低对比度直方图 1% 裁剪线性拉伸（calcHist + LUT）
    static void enhanceForOcr(cv::Mat &bgr) {
        cv::Mat gray;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::Scalar mean, stddev;
        cv::meanStdDev(gray, mean, stddev);
        // ① 暗光：gamma = 1 + (65-均值)/60，均值 5 时约 2.0 封顶
        if (mean[0] < 65.0) {
            double gamma = 1.0 + (65.0 - mean[0]) / 60.0;
            if (gamma > 2.0) gamma = 2.0;
            cv::Mat lut(1, 256, CV_8U);
            uint8_t *p = lut.ptr<uint8_t>(0);
            for (int i = 0; i < 256; i++)
                p[i] = cv::saturate_cast<uint8_t>(std::pow(i / 255.0, 1.0 / gamma) * 255.0);
            cv::LUT(bgr, lut, bgr);
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::meanStdDev(gray, mean, stddev);
        }
        // ② 低对比度：标准差 < 42 时按 1%/99% 分位裁剪拉伸
        if (stddev[0] > 0.0 && stddev[0] < 42.0) {
            const int histSize = 256;
            const int channels[] = {0};
            const float range[] = {0.f, 256.f};
            const float *ranges[] = {range};
            cv::Mat hist;
            cv::calcHist(&gray, 1, channels, cv::Mat(), hist, 1, &histSize, ranges);
            const int total = bgr.rows * bgr.cols;
            const int clipLow = total / 100;    // 1%
            const int clipHigh = total / 100;   // 1%
            int lo = 0, hi = 255, acc = 0;
            for (int i = 0; i < histSize; i++) {
                acc += (int) hist.at<float>(i);
                if (acc >= clipLow) { lo = i; break; }
            }
            acc = 0;
            for (int i = histSize - 1; i >= 0; i--) {
                acc += (int) hist.at<float>(i);
                if (acc >= clipHigh) { hi = i; break; }
            }
            if (hi - lo >= 30) {  // 拉伸空间足够小才做，避免放大噪声
                const double scale = 255.0 / (double) (hi - lo);
                cv::Mat lut(1, 256, CV_8U);
                uint8_t *p = lut.ptr<uint8_t>(0);
                for (int i = 0; i < 256; i++) {
                    int v = (int) ((i - lo) * scale);
                    p[i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t) v);
                }
                cv::LUT(bgr, lut, bgr);
            }
        }
    }

    // ───────── 推理流程区：worker 线程主循环（det → cls → rec） ─────────
    // ★ worker_loop：从队列取帧 → 降采样 1024 → 增强 → 引擎 detect（内部依次跑
    //   DBNet det / cls 方向分类 / CRNN 逐行识别）→ 终稿坐标乘回放因子 → publish。
    //   det 阶段回调 publishPending 在本线程内同步触发（虚线框先上屏）；
    //   任何一步失败都 continue 丢弃该帧（上次结果保留显示），不重试不过夜
    static void worker_loop() {
        // kPadding：det 输入四边外扩像素——贴边文字感受野不足会检不全，补边提高召回
        // kMaxSideLen：工作图长边上限（DBNet 输入上限本就 1024，再大是浪费）
        // kMaxBoxes：单帧最多发布的框数，防误检风暴拖垮绘制/上层
        constexpr int kPadding = 50;
        constexpr int kMaxSideLen = 1024;
        constexpr size_t kMaxBoxes = 30;

        LOGI("ocr worker started");
        while (g_running.load()) {
            OcrJob job;
            {
                std::unique_lock<std::mutex> lk(g_queue_mutex);
                // 条件变量等待：队列空则挂起（零 CPU），入队 notify 或退出广播时唤醒
                g_queue_cv.wait(lk, [] { return !g_queue.empty() || !g_running.load(); });
                if (!g_running.load() && g_queue.empty()) break;
                job = std::move(g_queue.front());
                g_queue.pop_front();
            }
            if (job.rgba.empty() || !g_enabled.load()) continue;

            // ★ 提速：先降采样到长边 1024（det 输入上限本就 1024，
            //   INTER_AREA 缩小质量最优），cvtColor/预处理/推理全在小图上跑；
            //   记录回放因子 1/s，框坐标乘回去后仍与全分辨率帧对齐。
            //   boxScale 的两个消费点：① det 回调 publishPending 读 g_box_scale；
            //   ② 终稿 textBlocks 在下方显式乘 boxScale
            cv::Mat work = job.rgba;
            double boxScale = 1.0;
            const int maxSide = std::max(work.cols, work.rows);
            if (maxSide > kMaxSideLen) {
                const double s = (double) kMaxSideLen / (double) maxSide;
                cv::resize(work, work, cv::Size(), s, s, cv::INTER_AREA);
                boxScale = 1.0 / s;
            }

            cv::Mat bgr;
            try {
                cv::cvtColor(work, bgr, cv::COLOR_RGBA2BGR);
            } catch (const cv::Exception &e) {
                LOGE("ocr cvtColor: %{public}s", e.what());
                continue;
            }
            // ★ 自适应预处理（暗光提亮/对比度拉伸），低质量帧识别率提升
            try {
                enhanceForOcr(bgr);
            } catch (const cv::Exception &e) {
                LOGE("ocr enhance: %{public}s", e.what());
            }

            OcrResult result;
            bool ok = false;
            {
                // ★ 防冲突：表格识别占用引擎（持有 g_ocr_mutex 做 crop OCR）时
                //   try_lock 150ms 失败即跳过本轮（上次结果保留显示），不排长队抢占 CPU
                std::unique_lock<std::timed_mutex> lk(g_ocr_mutex, std::defer_lock);
                if (!lk.try_lock_for(std::chrono::milliseconds(150))) continue;
                if (!g_ocr || !g_enabled.load()) continue;
                // ★ det 阶段回调（publishPending）在本线程内触发，先放好回放因子
                g_box_scale.store(boxScale);
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

            // ★ 框坐标还原：detect 返回的是降采样工作图坐标 → 乘回 1/s 与显示帧对齐
            if (boxScale != 1.0) {
                for (auto &tb : result.textBlocks) {
                    for (auto &pt : tb.boxPoint) {
                        pt.x = cvRound((double) pt.x * boxScale);
                        pt.y = cvRound((double) pt.y * boxScale);
                    }
                }
            }

            // 组装终稿：pts<4 的退化框丢弃；全文把各文本块文字以 \n 拼接
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

    // ★ init：默认入口——按 PaddleOCR 官方文件名拼出四个路径转调 initPaths；
    //   g_ready 幂等保护，重复调用直接返回 true
    bool init(const std::string &modelDir) {
        if (g_ready.load()) return true;

        // ★ 默认 PaddleOCR 文件名（模型集目录形态，与 Android 端一致）
        return initPaths(modelDir + "/ch_PP-OCRv4_det_infer.onnx",
                         modelDir + "/ch_ppocr_mobile_v2.0_cls_infer.onnx",
                         modelDir + "/ch_PP-OCRv4_rec_infer.onnx",
                         modelDir + "/ppocr_keys_v1.txt");
    }


    // ★ 多模型集入口：det/cls/rec/keys 四个文件任意命名（config.json 或启发式扫描指定）。
    //   顺序讲究：先限 OpenCV 线程池 → 建 OcrLite、注册 det 两阶段回调、加载模型
    //   → 引擎指针移入 g_ocr → 最后才置 g_running/g_ready 并起 worker，
    //   避免半初始化状态被 worker / 门控看到
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath) {
        if (g_ready.load()) return true;

        LOGI("ocr init: det=%{public}s", detPath.c_str());
        LOGI("ocr init: cls=%{public}s", clsPath.c_str());
        LOGI("ocr init: rec=%{public}s", recPath.c_str());
        LOGI("ocr init: keys=%{public}s", keysPath.c_str());
        // ★ 防冲突：限制本库 OpenCV parallel_for_ 线程池（默认=全部核心），
        //   避免与 ORT 推理线程/YOLO/人脸/SR 线程总和超额抢占 CPU
        cv::setNumThreads(4);

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
                std::lock_guard<std::timed_mutex> lk(g_ocr_mutex);
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

    // ★ release：优雅停机，顺序不可乱——先停 worker（g_running=false + notify +
    //   join，确保不再有推理在跑）→ 清队列 → 清引擎 → 清结果 → 复位全部状态量；
    //   之后可重新 init（模型热切换场景）
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
            std::lock_guard<std::timed_mutex> lk(g_ocr_mutex);
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

    // ★ isReady：仅模型加载状态（与开关无关）
    bool isReady() { return g_ready.load(); }

    // ★ isOCREnabled：ready 且开关打开——"OCR 真正可用"的业务判断
    bool isOCREnabled() { return g_ready.load() && g_enabled.load(); }

    // ★ setOCREnabled：运行开关。关闭：清队列与旧结果（框立即消失）；
    //   打开：g_scene_reset 置 true 绕过静帧门控 + 清触发时间戳 → 强制立即识别
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
            // 开启后强制立即识别一次（绕过静帧门控）
            g_scene_reset.store(true);
            g_last_trigger_ms.store(0);
            LOGI("ocr enabled");
        }
    }

    // ───────── 触发调度区：maybeTrigger（渲染线程每帧调用） ─────────
    // ★ maybeTrigger：完整触发链（三道门顺序不可换，逐级变贵）：
    //   0. 短路检查：未就绪/开关关/空帧直接返回（每帧调用，必须最便宜）
    //   ① 场景检查节流（200ms，g_last_check_ms）：sceneChanged 要 resize+cvtColor，
    //      虽轻也不必每帧跑——200ms 内至多比对一次场景
    //   ② 静帧门控（sceneChanged）：画面没动不推理，上次结果继续显示；
    //      基准缩略图只在触发时更新，缓慢移动的差值能累计越阈
    //   ③ 最小触发间隔（g_trigger_interval_ms，默认 1500ms 用户可调）：
    //      画面动了也别连珠炮——det+rec 一轮数百 ms，1.5s 已足够跟手
    //   三关全过 → clone 帧（与渲染缓冲解耦，防推理读到被覆盖的像素）→
    //   QUEUE_CAP=1 满则淘汰旧任务 → notify_one 唤醒 worker
    void maybeTrigger(const cv::Mat &rgba) {
        if (!g_ready.load() || !g_enabled.load() || rgba.empty()) return;

        const long long now = now_ms();
        // ① 场景检查节流：200ms 一次（原来是隔满定时间隔才检查一次场景，
        //    画面移动后要干等数秒才重新识别——这就是"框滞后于画面"的主因）
        if (now - g_last_check_ms.load() < 200) return;
        g_last_check_ms.store(now);

        // ② 静帧门控：画面无变化不重复推理（上次结果保留显示）→ 静止场景 0 推理省电
        if (!sceneChanged(rgba)) return;

        // ③ 两轮识别最小间隔（用户可配）：默认 1500ms，画面移动后框快速跟上
        if (now - g_last_trigger_ms.load() < g_trigger_interval_ms.load()) return;
        g_last_trigger_ms.store(now);

        // clone 是必须的：入队帧要在 worker 里慢慢处理，
        // 不 clone 的话渲染缓冲复用会覆盖正在推理的像素
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

    // ★ getResult：读最近识别全文。禁用时返回空串（上层据此清 UI 文案）
    std::string getResult() {
        if (!g_enabled.load()) return "";
        std::lock_guard<std::mutex> lk(g_result_mutex);
        return g_last_result;
    }

    /* ★ 参数 setter（滑条拖动立即生效，下次推理用新值） */
    // ★ setTriggerInterval：两轮识别最小间隔，夹 [500,20000]ms 防呆
    void setTriggerInterval(int ms) {
        if (ms >= 500 && ms <= 20000) g_trigger_interval_ms.store(ms);
    }

    // ★ setDetParams：DBNet 三参数——检出阈值/框置信度/unclip 文本框扩展比，
    //   各自夹紧安全区间；改动后置 g_scene_reset 强制重识别一次以验证效果
    void setDetParams(float dbScore, float dbBox, float unclip) {
        if (dbScore >= 0.1f && dbScore <= 0.9f) g_db_score.store(dbScore);
        if (dbBox >= 0.1f && dbBox <= 0.9f) g_db_box.store(dbBox);
        if (unclip >= 1.0f && unclip <= 3.0f) g_unclip.store(unclip);
        // 参数变化后强制重新识别一次（绕过静帧门控）
        g_scene_reset.store(true);
    }

    // ★ setNumThreads：OCR 推理线程数（下次 init 加载模型时应用）
    void setNumThreads(int n) {
        g_num_threads.store(n);
    }

    // ★ getLastBoxes：拷贝最近一次框列表（含 pending 虚线框），供上层自绘
    void getLastBoxes(std::vector<OcrBox> &out) {
        out.clear();
        if (!g_enabled.load()) return;
        std::lock_guard<std::mutex> lk(g_result_mutex);
        out = g_last_boxes;
    }

    // ★ 识别进度：done=已出文字的框数，total=总框数（供 HUD"识别中"徽标显示）。
    //   det 先行发布后 total 即非零而 done=0 → 上层据此显示"识别中 x/y"
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

    // ───────── 表格结构识别区（PP-Structure SLANet，独立于普通 OCR 管线） ─────────

    /* ★ 表格结构识别（PP-Structure SLANet） */

    // ★ initTable：加载 SLANet 表格结构模型；独立于 OCR 模型生命周期（可单独就绪），
    //   重复调用幂等（已就绪直接返回 true）
    bool initTable(const std::string &tableModelPath) {
        // ★ 防冲突：表格模型初始化也限制 OpenCV 线程池（独立 dlopen 库，
        //   initPaths 未跑过时兜底）
        cv::setNumThreads(4);
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

    // ★ isTableReady：表格模型是否就绪（与 OCR 开关/就绪状态互不影响）
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
    //      cb 回调，供弹窗实时显示被识别画面与 SLANet 结构框。
    //   坐标系：全程在"长边 1024 工作图"系内闭环——SLANet cells、OCR 行框、
    //   标注图同系，无需回放到原图；crop OCR 的行坐标加回 (ox,oy) 偏移即工作图坐标
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
                std::lock_guard<std::timed_mutex> lk(g_ocr_mutex);
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


    // ───────── 绘制区：把最近结果画回显示帧 ─────────
    // ★ drawLastBoxes：渲染线程每帧调用。框坐标已是显示帧系（publishPending
    //   与 worker 都乘过 g_box_scale），直接绘制。分支：pending 框灰虚线
    //   （识别中的轻提示），完成框 5 色轮换实线（与 Android 端观感对齐）
    void drawLastBoxes(cv::Mat &rgba) {        if (rgba.empty() || rgba.type() != CV_8UC4) return;
        if (!g_enabled.load()) return;

        std::vector<OcrBox> boxes;
        {
            std::lock_guard<std::mutex> lk(g_result_mutex);
            boxes = g_last_boxes;
        }
        if (boxes.empty()) return;

        // ★ 调色板：识别完成的框按索引 %5 轮换取色，相邻框颜色错开便于分辨
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

    // C ABI：默认文件名初始化（返回 1=成功 / 0=失败）
    int ocr_init(const char *modelDir) {
        if (!modelDir) return 0;
        return rapidocr_wrapper::init(std::string(modelDir)) ? 1 : 0;
    }

    // C ABI：停机释放全部模型/线程/结果
    void ocr_release(void) { rapidocr_wrapper::release(); }

    // C ABI：模型是否加载成功
    int ocr_is_ready(void) { return rapidocr_wrapper::isReady() ? 1 : 0; }

    // C ABI：OCR 是否真正在工作（ready && enabled）
    int ocr_is_enabled(void) { return rapidocr_wrapper::isOCREnabled() ? 1 : 0; }

    // C ABI：运行开关（非 0 = 开）
    void ocr_set_enabled(int enabled) { rapidocr_wrapper::setOCREnabled(enabled != 0); }

    // C ABI：渲染线程每帧触发入口——把裸 RGBA 指针零拷贝包成 Mat 后走三重门控；
    //   此处不 clone，真正的 clone 发生在门控全过之后（maybeTrigger 内部）
    void ocr_maybe_trigger(const uint8_t *rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return;
        cv::Mat m(h, w, CV_8UC4, const_cast<uint8_t *>(rgba));
        rapidocr_wrapper::maybeTrigger(m);
    }

    // C ABI：把最近结果框画到显示帧（零拷贝包 Mat；渲染线程每帧调用）
    void ocr_draw_last_boxes(uint8_t *rgba, int w, int h) {
        if (!rgba || w <= 0 || h <= 0) return;
        cv::Mat m(h, w, CV_8UC4, rgba);
        rapidocr_wrapper::drawLastBoxes(m);
    }

    // C ABI：取识别全文。返回值约定 = 写入长度 + 结尾 '\0'（空串也返回 1）；
    //   超长按 bufLen-1 截断，保证始终 NUL 结尾
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

    // C ABI：取框坐标。输出布局 ptsOut[i*8 + k*2(+1)] = 第 i 框第 k 顶点的 x(y)，
    //   即每框 8 个 float（4 顶点 × xy）；退化框（<4 点）写 0 占位保持对齐；
    //   返回实际写入的框数
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

    // C ABI：按框下标取单个框的文字。返回值约定同 ocr_get_result
    //   （写入长度+1；越界或空文字写空串返回 1）
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

    // C ABI：按显式文件路径初始化——多模型集（paddle/xiaomi 等任意命名）入口
    // 按显式文件路径初始化（多模型集：paddle / xiaomi 等任意命名）
    int ocr_init_paths(const char *detPath, const char *clsPath,
                       const char *recPath, const char *keysPath) {
        if (!detPath || !clsPath || !recPath || !keysPath) return 0;
        return rapidocr_wrapper::initPaths(std::string(detPath), std::string(clsPath),
                                           std::string(recPath), std::string(keysPath)) ? 1 : 0;
    }

    // 定时识别间隔（能耗模式，1~10s）
    // C ABI：两轮识别最小间隔（夹 [500,20000]ms，滑条立即生效，下次触发用新值）
    void ocr_set_trigger_interval(int ms) {
        if (ms >= 500 && ms <= 20000) rapidocr_wrapper::setTriggerInterval(ms);
    }

    // det 阈值三件套：检出阈值 / 框置信度 / 文本框扩展比
    // C ABI：DBNet 参数（各自夹紧安全区间，改动后强制重识别一次）
    void ocr_set_det_params(float dbScore, float dbBox, float unclip) {
        rapidocr_wrapper::setDetParams(dbScore, dbBox, unclip);
    }

    // 推理线程数（下次 init 生效）
    // C ABI：OCR 推理线程数（夹 [1,8]）
    void ocr_set_num_threads(int n) {
        if (n >= 1 && n <= 8) rapidocr_wrapper::setNumThreads(n);
    }

    /* ★ 表格结构识别（PP-Structure SLANet）C API */

    // 加载 SLANet 表格模型（独立于 OCR 模型生命周期）
    // C ABI：initTable 薄封装（返回 1=成功）
    int ocr_table_init(const char *tableModelPath) {
        if (!tableModelPath) return 0;
        return rapidocr_wrapper::initTable(std::string(tableModelPath)) ? 1 : 0;
    }

    // C ABI：表格模型是否就绪
    int ocr_table_is_ready(void) {
        return rapidocr_wrapper::isTableReady() ? 1 : 0;
    }

    // 单帧同步识别（增强版）：rgba 输入 → JSON 输出；预览图落 outDir 并经 cb 阶段回调，
    // structureFound 回写是否识别到表格结构（0=无框，供调用方换帧重试）；
    // 返回值 = JSON 写入长度 + 1（同 ocr_get_result 约定）
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
