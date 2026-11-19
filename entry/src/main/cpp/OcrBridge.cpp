// ★ OCR/SR 桥接实现：dlopen libocr.so + dlsym C ABI（OCR/表格/SR），
//   线程安全串行调用与引擎生命周期管理
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

    static std::mutex g_mutex;
    static void *g_handle = nullptr;
    static OcrApi g_api = {};
    static bool g_load_attempted = false;

    static void log_dlerror(const char *when) {
        const char *err = dlerror();
        if (err) LOGE("dlopen/dlsym %{public}s: %{public}s", when, err);
    }

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

    void unload() {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_handle) {
            dlclose(g_handle);
            g_handle = nullptr;
        }
        g_api = {};
        g_load_attempted = false;
    }

    bool isLoaded() {
        std::lock_guard<std::mutex> lk(g_mutex);
        return g_handle != nullptr;
    }

    bool init(const std::string &modelDir) {
        if (!load()) return false;
        return g_api.init(modelDir.c_str()) != 0;
    }

    bool initPaths(const std::string &detPath, const std::string &clsPath,
                   const std::string &recPath, const std::string &keysPath) {
        if (!load()) return false;
        return g_api.init_paths(detPath.c_str(), clsPath.c_str(),
                                recPath.c_str(), keysPath.c_str()) != 0;
    }

    void setTriggerInterval(int ms) {
        if (!isLoaded()) return;
        g_api.set_trigger_interval(ms);
    }

    void setDetParams(float dbScore, float dbBox, float unclip) {
        if (!isLoaded()) return;
        g_api.set_det_params(dbScore, dbBox, unclip);
    }

    void setNumThreads(int n) {
        if (!isLoaded()) return;
        g_api.set_num_threads(n);
    }

    void release() {
        if (!isLoaded()) return;
        g_api.release();
    }

    bool isReady() {
        if (!isLoaded()) return false;
        return g_api.is_ready() != 0;
    }

    bool isEnabled() {
        if (!isLoaded()) return false;
        return g_api.is_enabled() != 0;
    }

    void setEnabled(bool enabled) {
        if (!isLoaded()) return;
        g_api.set_enabled(enabled ? 1 : 0);
    }

    std::string getResult() {
        if (!isLoaded()) return "";
        char buf[4096];
        int n = g_api.get_result(buf, sizeof(buf));
        if (n <= 0) return "";
        return std::string(buf);
    }

    void maybeTrigger(const uint8_t *rgba, int w, int h) {
        if (!isLoaded()) return;
        g_api.maybe_trigger(rgba, w, h);
    }

    void drawLastBoxes(uint8_t *rgba, int w, int h) {
        if (!isLoaded()) return;
        g_api.draw_last_boxes(rgba, w, h);
    }

    int getBoxes(float *ptsOut, int maxBoxes) {
        if (!isLoaded()) return 0;
        return g_api.get_boxes(ptsOut, maxBoxes);
    }

    std::string getBoxText(int idx) {
        if (!isLoaded()) return "";
        char buf[2048];
        int n = g_api.get_box_text(idx, buf, sizeof(buf));
        if (n <= 0) return "";
        return std::string(buf);
    }

    void getProgress(int &done, int &total) {
        done = 0;
        total = 0;
        if (!isLoaded()) return;
        g_api.get_progress(&done, &total);
    }

    /* ★ 表格结构识别（PP-Structure SLANet） */

    bool initTable(const std::string &tableModelPath) {
        if (!load()) return false;
        return g_api.table_init(tableModelPath.c_str()) != 0;
    }

    bool isTableReady() {
        if (!isLoaded()) return false;
        return g_api.table_is_ready() != 0;
    }

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

    bool srInit(const std::string &onnxPath, int numThreads) {
        if (!load()) return false;
        return g_api.sr_init(onnxPath.c_str(), numThreads) != 0;
    }

    bool srIsReady() {
        if (!isLoaded()) return false;
        return g_api.sr_is_ready() != 0;
    }

    void srRelease() {
        if (!isLoaded()) return;
        g_api.sr_release();
    }

    bool srUpscale(const uint8_t *in, int inW, int inH, uint8_t *out, int outW, int outH) {
        if (!isLoaded()) return false;
        return g_api.sr_upscale_rgba(in, inW, inH, out, outW, outH) != 0;
    }

    int srScale() {
        if (!isLoaded()) return 0;
        return g_api.sr_scale();
    }

    /* ★ 人脸识别（SCRFD det_500m + 2d106det 106 关键点 + genderage） */

    bool faceInit(const std::string &detPath, const std::string &lmPath,
                  const std::string &gaPath, int numThreads) {
        if (!load()) return false;
        return g_api.face_init(detPath.c_str(), lmPath.c_str(), gaPath.c_str(), numThreads) != 0;
    }

    bool faceIsReady() {
        if (!isLoaded()) return false;
        return g_api.face_is_ready() != 0;
    }

    void faceRelease() {
        if (!isLoaded()) return;
        g_api.face_release();
    }

    int faceDetect(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces) {
        if (!isLoaded()) return 0;
        return g_api.face_detect_rgba(rgba, w, h, facesOut, maxFaces);
    }

}  // namespace ocr_bridge
