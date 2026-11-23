// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrBridge —— libentry.so ↔ libocr.so 桥接层实现（OcrBridge.cpp）            ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【文件职责】OCR/SR 桥接实现：dlopen libocr.so + dlsym C ABI（OCR/表格/SR）， ║
// ║   线程安全串行调用与引擎生命周期管理                                          ║
// ║                                                                              ║
// ║ 【设计意图】libentry.so 编译期不依赖 libocr.so：                              ║
// ║   ① 重依赖（onnxruntime 等）不进主 .so 加载链，entry 模块注册永不被阻塞；     ║
// ║   ② 加载失败 dlerror() 精确定位，且各业务接口静默降级返回（空串/0/false），   ║
// ║     不影响相机/渲染主功能；                                                   ║
// ║   ③ 符号缺失时整体 dlclose 回滚，防止"半初始化"状态。                        ║
// ║                                                                              ║
// ║ 【调用链】YuvPipeline/napi_init → 本命名空间 → g_api 函数指针（dlsym 获得） ║
// ║   → libocr.so 实现（推理在其自有 worker 线程）。                              ║
// ║   OcrApi 成员与 ocr_c_api.h 声明、libocr.so 导出符号严格一一对应，           ║
// ║   新增接口需三处同步：ocr_c_api.h / 本文件 OcrApi 与 load() / OcrBridge.h。 ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "OcrBridge.h"
#include "ocr_c_api.h"

#include <hilog/log.h>
#include <dlfcn.h>
#include <mutex>
#include <vector>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3200
#define LOG_TAG "OcrBridge"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace ocr_bridge {

    // 与 ocr_c_api.h 一一对应的函数指针表
    // ★ OcrApi：dlsym 解析出的 C ABI 函数指针集合——声明顺序与 ocr_c_api.h 分组
    //   保持一致（OCR 基础/参数/表格/超分/人脸），新增符号必须同步三处
    //   （ocr_c_api.h、此结构体、load() 的 dlsym 与完整性校验），否则 load() 整体失败
    struct OcrApi {
        int (*init)(const char *);
        int (*init_paths)(const char *, const char *, const char *, const char *);
        void (*release)(void);
        int (*is_ready)(void);
        int (*is_enabled)(void);
        void (*set_enabled)(int);
        void (*maybe_trigger)(const uint8_t *, int, int);
        void (*draw_last_boxes)(uint8_t *, int, int);
        int (*get_result)(char *, int);
        int (*get_boxes)(float *, int);
        int (*get_box_text)(int, char *, int);
        void (*get_progress)(int *, int *);
        void (*set_trigger_interval)(int);
        void (*set_det_params)(float, float, float);
        void (*set_num_threads)(int);
        int (*table_init)(const char *);
        int (*table_is_ready)(void);
        int (*recognize_table_ex)(const uint8_t *, int, int, const char *,
                                  ocr_table_event_cb, void *, int *, char *, int);
        int (*sr_init)(const char *, int);
        int (*sr_is_ready)(void);
        void (*sr_release)(void);
        int (*sr_upscale_rgba)(const uint8_t *, int, int, uint8_t *, int, int);
        int (*sr_scale)(void);
        int (*face_init)(const char *, const char *, const char *, int);
        int (*face_is_ready)(void);
        void (*face_release)(void);
        int (*face_detect_rgba)(const uint8_t *, int, int, float *, int);
    };

    // ★ 全局状态：g_mutex 只保护加载/卸载流程（保证 dlopen 全进程仅一次）；
    //   g_load_attempted 防止加载失败后每次业务调用都重试 dlopen（快速失败）；
    //   g_api 在 load 成功后只读，业务调用无需加锁（线程安全由 libocr.so 内部保证）
    static std::mutex g_mutex;
    static void *g_handle = nullptr;
    static OcrApi g_api = {};
    static bool g_load_attempted = false;

    // ★ log_dlerror：统一输出 dlopen/dlsym 的动态链接器错误（when 标注出错阶段），
    //   便于精确定位"符号缺失/库损坏/依赖缺失"类问题
    static void log_dlerror(const char *when) {
        const char *err = dlerror();
        if (err) LOGE("dlopen/dlsym %{public}s: %{public}s", when, err);
    }

    // ★ load：加载 libocr.so 并解析全部 C ABI 符号（已加载直接返回 true）。
    //   候选路径两处：链接器默认搜索名（libs 目录已在搜索路径）与 el1/bundle 绝对路径兜底；
    //   RTLD_NOW 立即解析全部未定义符号（宁可加载时失败，也不留到调用时崩溃），
    //   RTLD_LOCAL 不污染全局符号表；
    //   任何一个符号 dlsym 失败即整体 dlclose 回滚（防"半初始化"），换下一候选路径；
    //   全部失败置 g_load_attempted=true，后续调用快速失败不再重试
    bool load() {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_handle) return true;
        if (g_load_attempted) return false;  // 不反复重试失败加载
        g_load_attempted = true;

        const char *candidates[] = {
                "libocr.so",
                "/data/storage/el1/bundle/libs/arm64/libocr.so",
        };

        for (const char *name : candidates) {
            dlerror();  // 清空旧错误
            void *h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (!h) {
                log_dlerror(name);
                continue;
            }
            // ★ 逐个 dlsym 解析符号：与 ocr_c_api.h/OcrApi 一一对应，
            //   缺一即视为 ABI 不匹配，整体回滚换下一候选路径
            OcrApi api = {};
            api.init = (int (*)(const char *)) dlsym(h, "ocr_init");
            api.init_paths = (int (*)(const char *, const char *, const char *, const char *))
                    dlsym(h, "ocr_init_paths");
            api.release = (void (*)(void)) dlsym(h, "ocr_release");
            api.is_ready = (int (*)(void)) dlsym(h, "ocr_is_ready");
            api.is_enabled = (int (*)(void)) dlsym(h, "ocr_is_enabled");
            api.set_enabled = (void (*)(int)) dlsym(h, "ocr_set_enabled");
            api.maybe_trigger = (void (*)(const uint8_t *, int, int)) dlsym(h, "ocr_maybe_trigger");
            api.draw_last_boxes = (void (*)(uint8_t *, int, int)) dlsym(h, "ocr_draw_last_boxes");
            api.get_result = (int (*)(char *, int)) dlsym(h, "ocr_get_result");
            api.get_boxes = (int (*)(float *, int)) dlsym(h, "ocr_get_boxes");
            api.get_box_text = (int (*)(int, char *, int)) dlsym(h, "ocr_get_box_text");
            api.get_progress = (void (*)(int *, int *)) dlsym(h, "ocr_get_progress");
            api.set_trigger_interval = (void (*)(int)) dlsym(h, "ocr_set_trigger_interval");
            api.set_det_params = (void (*)(float, float, float)) dlsym(h, "ocr_set_det_params");
            api.set_num_threads = (void (*)(int)) dlsym(h, "ocr_set_num_threads");
            api.table_init = (int (*)(const char *)) dlsym(h, "ocr_table_init");
            api.table_is_ready = (int (*)(void)) dlsym(h, "ocr_table_is_ready");
            api.recognize_table_ex = (int (*)(const uint8_t *, int, int, const char *,
                                              ocr_table_event_cb, void *, int *, char *, int))
                    dlsym(h, "ocr_recognize_table_ex");
            api.sr_init = (int (*)(const char *, int)) dlsym(h, "sr_init");
            api.sr_is_ready = (int (*)(void)) dlsym(h, "sr_is_ready");
            api.sr_release = (void (*)(void)) dlsym(h, "sr_release");
            api.sr_upscale_rgba = (int (*)(const uint8_t *, int, int, uint8_t *, int, int))
                    dlsym(h, "sr_upscale_rgba");
            api.sr_scale = (int (*)(void)) dlsym(h, "sr_scale");
            api.face_init = (int (*)(const char *, const char *, const char *, int))
                    dlsym(h, "face_init");
            api.face_is_ready = (int (*)(void)) dlsym(h, "face_is_ready");
            api.face_release = (void (*)(void)) dlsym(h, "face_release");
            api.face_detect_rgba = (int (*)(const uint8_t *, int, int, float *, int))
                    dlsym(h, "face_detect_rgba");

            // ★ 符号完整性校验：任一为空即 dlclose 回滚（防"半初始化"状态）
            if (!api.init || !api.init_paths || !api.release || !api.is_ready || !api.is_enabled ||
                !api.set_enabled || !api.maybe_trigger || !api.draw_last_boxes ||
                !api.get_result || !api.get_boxes || !api.get_box_text ||
                !api.get_progress || !api.set_trigger_interval ||
                !api.set_det_params || !api.set_num_threads ||
                !api.table_init || !api.table_is_ready || !api.recognize_table_ex ||
                !api.sr_init || !api.sr_is_ready || !api.sr_release ||
                !api.sr_upscale_rgba || !api.sr_scale ||
                !api.face_init || !api.face_is_ready || !api.face_release ||
                !api.face_detect_rgba) {
                LOGE("dlsym incomplete for %{public}s", name);
                log_dlerror(name);
                dlclose(h);
                continue;
            }

            g_handle = h;
            g_api = api;
            LOGI("libocr.so loaded via %{public}s", name);
            return true;
        }

        LOGE("libocr.so load failed: all candidates exhausted");
        return false;
    }

    // ★ unload：dlclose 卸载并清空函数指针表、重置加载标记（下次 load 可重新尝试）；
    //   仅热替换 libocr.so 的调试场景使用
    void unload() {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_handle) {
            dlclose(g_handle);
            g_handle = nullptr;
        }
        g_api = {};
        g_load_attempted = false;
    }

    // ★ isLoaded：libocr.so 是否已成功加载（不含模型初始化状态）
    bool isLoaded() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_handle != nullptr;
    }

    // ★ init：按模型目录初始化 OCR 引擎（内部 onnxruntime 建会话，耗时，
    //   napi 层已放入异步线程）；未加载返回 false
    bool init(const std::string &modelDir) {
        if (!load()) return false;
        return g_api.init(modelDir.c_str()) != 0;
    }

    // ★ initPaths：按显式 det/cls/rec/keys 路径初始化（多模型集切换，任意命名）
    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath) {
        if (!load()) return false;
        return g_api.init_paths(detPath.c_str(), clsPath.c_str(),
                                recPath.c_str(), keysPath.c_str()) != 0;
    }

    // ★ setTriggerInterval：周期识别间隔（ms，能耗模式分档，立即生效）
    void setTriggerInterval(int ms) {
        if (!isLoaded()) return;
        g_api.set_trigger_interval(ms);
    }

    // ★ setDetParams：检出阈值/框置信度/扩展比三件套（滑条立即生效）
    void setDetParams(float dbScore, float dbBox, float unclip) {
        if (!isLoaded()) return;
        g_api.set_det_params(dbScore, dbBox, unclip);
    }

    // ★ setNumThreads：OCR 推理线程数（下次 init 生效）
    void setNumThreads(int n) {
        if (!isLoaded()) return;
        g_api.set_num_threads(n);
    }

    // ★ release：释放 OCR 引擎（内部 join 推理 worker 可能阻塞数秒，
    //   napi 层 releaseOcr 已放入异步线程执行）
    void release() {
        if (!isLoaded()) return;
        g_api.release();
    }

    // ★ isReady：OCR 引擎是否初始化完成
    bool isReady() {
        if (!isLoaded()) return false;
        return g_api.is_ready() != 0;
    }

    // ★ isEnabled：识别开关状态
    bool isEnabled() {
        if (!isLoaded()) return false;
        return g_api.is_enabled() != 0;
    }

    // ★ setEnabled：识别总开关（关闭后 maybeTrigger 空操作，省电）
    void setEnabled(bool enabled) {
        if (!isLoaded()) return;
        g_api.set_enabled(enabled ? 1 : 0);
    }

    // ★ getResult：最近一次识别的整段文本（调用方出缓冲回拷，空串=无结果/未加载）
    std::string getResult() {
        if (!isLoaded()) return "";
        char buf[4096];
        int n = g_api.get_result(buf, sizeof(buf));
        if (n <= 0) return "";
        return std::string(buf);
    }

    // ★ maybeTrigger：管线每帧调用的识别触发入口——内部 800ms 节流 + 队列容量 1，
    //   推理在 libocr 自有 worker 执行，绝不阻塞渲染线程
    void maybeTrigger(const uint8_t *rgba, int w, int h) {
        if (!isLoaded()) return;
        g_api.maybe_trigger(rgba, w, h);
    }

    // ★ drawLastBoxes：把最近一次识别的文字框（青色折线）画到当前帧
    void drawLastBoxes(uint8_t *rgba, int w, int h) {
        if (!isLoaded()) return;
        g_api.draw_last_boxes(rgba, w, h);
    }

    // ★ getBoxes：最近一次识别的四边形文字框坐标（每框 8 float，返回框数）
    int getBoxes(float *ptsOut, int maxBoxes) {
        if (!isLoaded()) return 0;
        return g_api.get_boxes(ptsOut, maxBoxes);
    }

    // ★ getBoxText：第 idx 个框的识别文本（越界/未加载返回空串）
    std::string getBoxText(int idx) {
        if (!isLoaded()) return "";
        char buf[2048];
        int n = g_api.get_box_text(idx, buf, sizeof(buf));
        if (n <= 0) return "";
        return std::string(buf);
    }

    // ★ getProgress：识别进度（done=已出文字框数，total=总框数；未加载时 0/0）
    void getProgress(int &done, int &total) {
        done = 0;
        total = 0;
        if (!isLoaded()) return;
        g_api.get_progress(&done, &total);
    }

    /* ★ 表格结构识别（PP-Structure SLANet） */

    // ★ initTable：加载 SLANet 表格模型（独立于 OCR 三件套生命周期，互不影响）
    bool initTable(const std::string &tableModelPath) {
        if (!load()) return false;
        return g_api.table_init(tableModelPath.c_str()) != 0;
    }

    // ★ isTableReady：表格模型是否就绪
    bool isTableReady() {
        if (!isLoaded()) return false;
        return g_api.table_is_ready() != 0;
    }

    // ★ recognizeTableEx：一帧同步完成"SLANet 结构 + 表格区域 OCR + 单元格匹配"。
    //   JSON 含 html 全文可能远超普通文本缓冲，故用 256KB 大缓冲回拷；
    //   structureFound 回写是否识别到表格结构（false 时 napi 层换帧重试，最多 3 次）；
    //   eventCb/userdata 透传给 libocr 做阶段事件回调（预览图路径实时回传 JS）
    std::string recognizeTableEx(const uint8_t *rgba, int w, int h, const std::string &outDir,
                                 ocr_table_event_cb eventCb, void *userdata,
                                 bool &structureFound) {
        structureFound = false;
        if (!isLoaded()) return "{\"error\":\"OCR 库未加载\"}";
        // 表格 JSON（含 html 全文）可能远超 4KB，用大缓冲
        std::vector<char> buf(256 * 1024);
        int found = 0;
        int n = g_api.recognize_table_ex(rgba, w, h, outDir.c_str(),
                                         eventCb, userdata,
                                         &found, buf.data(), (int) buf.size());
        if (n <= 0) return "{\"error\":\"表格识别失败\"}";
        structureFound = (found != 0);
        return std::string(buf.data());
    }

    /* ★ AI 超分（QuickSRNetSmall / Real-ESRGAN x4） */

    // ★ srInit：加载超分 ONNX（动态尺寸版），numThreads<=0 时 libocr 内部默认 4
    bool srInit(const std::string &onnxPath, int numThreads) {
        if (!load()) return false;
        return g_api.sr_init(onnxPath.c_str(), numThreads) != 0;
    }

    // ★ srIsReady：超分模型是否就绪
    bool srIsReady() {
        if (!isLoaded()) return false;
        return g_api.sr_is_ready() != 0;
    }

    // ★ srRelease：释放超分模型（回收 25MB 级推理缓冲）
    void srRelease() {
        if (!isLoaded()) return;
        g_api.sr_release();
    }

    // ★ srUpscale：单帧同步超分（RGBA→xN→RGBA）；输出容量由调用方按 srScale 保证，
    //   失败返回 false 由上层走非 AI 路径兜底
    bool srUpscale(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH) {
        if (!isLoaded()) return false;
        return g_api.sr_upscale_rgba(in, inW, inH, out, outW, outH) != 0;
    }

    // ★ srScale：当前模型放大倍数（2/3/4），未就绪返回 0
    int srScale() {
        if (!isLoaded()) return 0;
        return g_api.sr_scale();
    }

    /* ★ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage） */

    // ★ faceInit：加载检测/关键点/性别年龄三个模型（同步建会话，napi 层已异步化）
    bool faceInit(const std::string &detPath, const std::string &lmPath,
                  const std::string &gaPath, int numThreads) {
        if (!load()) return false;
        return g_api.face_init(detPath.c_str(), lmPath.c_str(), gaPath.c_str(), numThreads) != 0;
    }

    // ★ faceIsReady：人脸模型组是否就绪
    bool faceIsReady() {
        if (!isLoaded()) return false;
        return g_api.face_is_ready() != 0;
    }

    // ★ faceRelease：释放人脸模型组
    void faceRelease() {
        if (!isLoaded()) return;
        g_api.face_release();
    }

    // ★ faceDetect：一帧同步检测（face 工作线程调用）——facesOut 每脸 228 float，
    //   布局见 ocr_c_api.h（box/分数/性别年龄颜值/106 点/颜值分项/trackId），返回脸数
    int faceDetect(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces) {
        if (!isLoaded()) return 0;
        return g_api.face_detect_rgba(rgba, w, h, facesOut, maxFaces);
    }

}  // namespace ocr_bridge
