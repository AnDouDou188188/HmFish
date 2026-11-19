// ★ 人脸识别引擎（libocr.so）：onnxruntime CPU 推理
//   模型组合（insightface buffalo_s 系，离线 Python 实证解码）：
//     det_500m.onnx   SCRFD 人脸检测（letterbox 640x640：长边等比贴合、短边 pad 114，
//                     9 输出：3 scores + 3 bbox + 3 kps，stride 8/16/32，每格 2 anchor 位于整数 stride 网格点）
//     2d106det.onnx   106 关键点（192x192，bbox 中心 1.5 倍裁剪，图内自带归一化，
//                     输出 fc1[1,212]，解码 (v+1)*96 后逆映射回原图）
//     genderage.onnx  性别年龄（96x96，输出[1,3]：argmax 前 2 位为性别、第 3 位×100 为年龄）
//   颜值分：106 点几何量（对称性/眼宽比/鼻宽比/嘴宽比/三庭比例/眼睛开合比）
//           以双眼中心距归一化，高斯容差加权求和，0~100
//   ABI: face_init/face_is_ready/face_release/face_detect_rgba（libentry 经 OcrBridge dlsym 调用）
#include <onnxruntime/core/session/onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <hilog/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define LOG_DOMAIN 0x3200
#define LOG_TAG "FaceNet"
#define LOGI(...) OH_LOG_INFO(LOG_APP, __VA_ARGS__)
#define LOGE(...) OH_LOG_ERROR(LOG_APP, __VA_ARGS__)

namespace {

    Ort::Env &faceEnv() {
        static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "facenet");
        return env;
    }

    std::mutex g_face_mutex;
    std::unique_ptr<Ort::Session> g_det_sess;
    std::unique_ptr<Ort::Session> g_lm_sess;
    std::unique_ptr<Ort::Session> g_ga_sess;
    std::atomic<bool> g_face_ready{false};

    std::string g_det_in;                 // 检测输入 tensor 名
    std::string g_det_outs[9];            // 检测输出 tensor 名（0-2 scores / 3-5 bbox / 6-8 kps）
    std::string g_lm_in;
    std::string g_lm_out;
    std::string g_ga_in;
    std::string g_ga_out;
    int g_lm_size = 192;                  // 2d106det 输入边长
    int g_ga_size = 96;                   // genderage 输入边长

    constexpr int kDetInput = 640;        // SCRFD 固定输入尺寸
    constexpr int kDetStrides[3] = {8, 16, 32};
    constexpr float kDetThresh = 0.40f;   // 检测阈值（insightface 默认 0.5，实机取宽）
    constexpr float kNmsThresh = 0.40f;
    constexpr int64_t kGaRefreshMs = 1200;   // 性别年龄复检间隔（track 内节流）
    constexpr size_t kMaxTracks = 8;
    constexpr size_t kMaxFacesOut = 4;    // 最多输出脸数（关键点全部推理）
    constexpr int kFaceFloats = 227;      // 每脸输出 float 数：[0..7]基本信息 + [8..219]106点 + [220..226]颜值分项
    constexpr float kGaVoteMargin = 0.25f;  // 性别投票置信度门槛（|logit 差|）

    // 复用缓冲（全部在 g_face_mutex 内访问，仅人脸工作线程实际使用）
    cv::Mat g_rgb;                        // 全分辨率 RGB
    cv::Mat g_det_small;                  // letterbox 等比缩放中间图
    cv::Mat g_det_mat;                    // 640x640 RGB（letterbox pad 后）
    cv::Mat g_warp;                       // 关键点/性别年龄裁剪输出
    std::vector<float> g_det_blob;        // (640*640*3)
    std::vector<float> g_lm_blob;
    std::vector<float> g_ga_blob;
    int g_frame_w = 0, g_frame_h = 0;     // 当前帧尺寸（[ga] 日志切边诊断用）

    struct FaceBox {
        float x1, y1, x2, y2, score;
        float kps[10];
    };

    // 性别年龄 track：按 IoU 匹配跨帧关联，genderage 推理节流 + 性别 5 票多数表决（置信度门槛）+ 年龄环形中值
    struct FaceTrack {
        float x1, y1, x2, y2;
        std::chrono::steady_clock::time_point lastSeen;
        std::chrono::steady_clock::time_point lastGa;
        int gender = -1;
        float age = -1.0f;
        int votes[5] = {-1, -1, -1, -1, -1};
        int voteIdx = 0;
        int voteCnt = 0;
        float ages[7] = {0};   // 年龄环形缓冲（中值滤波）
        int ageIdx = 0;
        int ageCnt = 0;
    };
    std::vector<FaceTrack> g_tracks;

    float medianOf(const float *v, int n) {
        float tmp[7];
        for (int i = 0; i < n; ++i) tmp[i] = v[i];
        std::sort(tmp, tmp + n);
        return (n & 1) ? tmp[n / 2] : 0.5f * (tmp[n / 2 - 1] + tmp[n / 2]);
    }

    float boxIou(const FaceBox &a, const FaceBox &b) {
        const float xx1 = std::max(a.x1, b.x1), yy1 = std::max(a.y1, b.y1);
        const float xx2 = std::min(a.x2, b.x2), yy2 = std::min(a.y2, b.y2);
        const float inter = std::max(0.0f, xx2 - xx1) * std::max(0.0f, yy2 - yy1);
        const float ua = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
        return ua > 0.0f ? inter / ua : 0.0f;
    }

    // ★ 颜值分（0~100）：106 点几何量加权。全部以双眼中心距 d 归一化，
    //   高斯容差 gauss(r, ideal, tol) = 100*exp(-((r-ideal)/tol)^2)。
    //   comp[7] 输出分项（各 0~100）：[0]对称 [1]眼形 [2]眉形 [3]鼻形 [4]嘴形 [5]三庭 [6]眼型
    //   dbg 可为 null；非 null 时输出 8 个诊断比值：
    //   [0]d [1]眼宽比 [2]右眉眼距 [3]左眉眼距 [4]鼻宽比 [5]嘴宽比 [6]三庭 [7]眼开合
    float faceBeautyScore(const float *p, float *comp, float *dbg) {
        auto X = [&](int i) { return p[2 * i]; };
        auto Y = [&](int i) { return p[2 * i + 1]; };
        const float rex = X(34), rey = Y(34);   // 右眼中心（瞳孔重复点）
        const float lex = X(88), ley = Y(88);   // 左眼中心
        const float d = std::hypot(lex - rex, ley - rey);
        // NaN 安全守卫：点位含 NaN 时 d/faceW/alen 为 NaN，普通 < 比较为假会
        // 放行，导致 NaN 扩散到分项/总分 → 改为 !(x>=min) 形式（NaN 必拦截）
        if (!(d >= 8.0f)) return -1.0f;
        const float faceW = std::hypot(X(17) - X(1), Y(17) - Y(1));
        if (!(faceW >= 16.0f)) return -1.0f;
        // 中轴线：双眼中点 → 下巴(0)
        const float mx = (rex + lex) * 0.5f, my = (rey + ley) * 0.5f;
        float ax = X(0) - mx, ay = Y(0) - my;
        const float alen = std::hypot(ax, ay);
        if (!(alen >= 1.0f)) return -1.0f;
        ax /= alen; ay /= alen;
        auto lineDist = [&](float px, float py) {
            const float dx = px - mx, dy = py - my;
            return std::fabs(dx * ay - dy * ax);
        };
        // ① 对称性：左右镜像点对到中轴线距离差的均值（占脸宽比例）
        static const int kSymPairs[][2] = {
            {35, 93}, {39, 89}, {52, 61}, {77, 83}, {46, 97},
            {2, 24}, {4, 22}, {6, 20}, {8, 18},
            {10, 31}, {12, 29}, {14, 27}, {16, 25}
        };
        float asym = 0.0f;
        for (const auto &pr : kSymPairs) {
            asym += std::fabs(lineDist(X(pr[0]), Y(pr[0])) - lineDist(X(pr[1]), Y(pr[1])));
        }
        asym /= (float)(sizeof(kSymPairs) / sizeof(kSymPairs[0])) * faceW;
        const float symScore = 100.0f - std::min(45.0f, asym * 450.0f);
        auto gauss = [](float r, float ideal, float tol) {
            const float t = (r - ideal) / tol;
            return 100.0f * std::exp(-t * t);
        };
        // ② 眼形：眼宽比（外眼角-内眼角 / 双眼距）
        const float eyeW = (std::hypot(X(39) - X(35), Y(39) - Y(35)) +
                            std::hypot(X(93) - X(89), Y(93) - Y(89))) * 0.5f;
        const float eyeScore = gauss(eyeW / d, 0.46f, 0.09f);
        // ③ 眉形：眉眼距比（眉下沿中点高于眼中心 / d，理想 0.28 Lena 校准）
        //    + 左右眉眼距对称；眉点被遮挡时 runLandmarks 标记 NaN →
        //    取该侧有效点均值，整侧无效回退对侧，双侧无效回退理想值
        auto browGapOf = [&](const int *bi, float eyeCy) -> float {
            float sum = 0.0f;
            int cnt = 0;
            for (int k = 0; k < 5; ++k) {
                const float y = Y(bi[k]);
                if (!std::isnan(y)) {
                    sum += y;
                    ++cnt;
                }
            }
            return cnt > 0 ? (eyeCy - sum / (float)cnt) / d
                           : std::numeric_limits<float>::quiet_NaN();
        };
        static const int kBrowRLo[] = {43, 44, 45, 46, 47};
        static const int kBrowLLo[] = {97, 98, 99, 100, 101};
        float browGapR = browGapOf(kBrowRLo, rey);
        float browGapL = browGapOf(kBrowLLo, ley);
        if (std::isnan(browGapR) && !std::isnan(browGapL)) browGapR = browGapL;
        if (std::isnan(browGapL) && !std::isnan(browGapR)) browGapL = browGapR;
        if (std::isnan(browGapR)) browGapR = 0.28f;
        if (std::isnan(browGapL)) browGapL = 0.28f;
        const float browScore =
            0.5f * gauss((browGapR + browGapL) * 0.5f, 0.28f, 0.12f) +
            0.5f * gauss(std::fabs(browGapR - browGapL), 0.0f, 0.08f);
        // ④ 鼻形：鼻宽比（鼻翼 77/83）
        const float noseW = std::hypot(X(83) - X(77), Y(83) - Y(77));
        const float noseScore = gauss(noseW / d, 0.58f, 0.11f);
        // ⑤ 嘴形：嘴宽比（嘴角 52/61）
        const float mouthW = std::hypot(X(61) - X(52), Y(61) - Y(52));
        const float mouthScore = gauss(mouthW / d, 0.75f, 0.13f);
        // ⑥ 三庭：眼中线→鼻基线 : 鼻基线→下巴
        const float eyeY = (rey + ley) * 0.5f;
        const float noseBaseY = (Y(78) + Y(79) + Y(80)) / 3.0f;
        const float thirds = (noseBaseY - eyeY) / std::max(1.0f, Y(0) - noseBaseY);
        // ★ 理想值校准：本式锚点是"眼中线"而非传统三庭的"眉线"——眼中线低于
        //   眉线约 0.27 个三庭单位，故正常脸该比值 ≈ 0.73，不是 1.0
        //   （旧理想 1.0 导致三庭恒 3 分左右）。实测 0.70 → 校准后 ~93 分，
        //   下庭过长/过短仍显著失分
        const float thirdsScore = gauss(thirds, 0.74f, 0.15f);
        // ⑦ 眼型：眼睛开合比（眼高/眼宽，上眼睑 40/94 下眼睑 33/87）
        const float eyeH = (std::fabs(Y(33) - Y(40)) + std::fabs(Y(87) - Y(94))) * 0.5f;
        const float aspectScore = gauss(eyeH / std::max(1.0f, eyeW), 0.40f, 0.13f);

        comp[0] = symScore;
        comp[1] = eyeScore;
        comp[2] = browScore;
        comp[3] = noseScore;
        comp[4] = mouthScore;
        comp[5] = thirdsScore;
        comp[6] = aspectScore;
        if (dbg) {
            dbg[0] = d;
            dbg[1] = eyeW / d;
            dbg[2] = browGapR;
            dbg[3] = browGapL;
            dbg[4] = noseW / d;
            dbg[5] = mouthW / d;
            dbg[6] = thirds;
            dbg[7] = eyeH / std::max(1.0f, eyeW);
        }
        // ★ NaN 清洗 + 权重归一：个别分项因点位 NaN 时置 -1（无效，标签/详情
        //   端按 <0 跳过），总分按有效分项权重求和后归一（全部有效时与原
        //   固定权重完全一致），有效权重不足一半视为不可信返回 -1
        static const float kScoreW[7] = {0.22f, 0.13f, 0.12f, 0.14f,
                                         0.14f, 0.16f, 0.09f};
        for (int k = 0; k < 7; ++k) {
            if (std::isnan(comp[k])) comp[k] = -1.0f;
        }
        float score = 0.0f, wsum = 0.0f;
        for (int k = 0; k < 7; ++k) {
            if (comp[k] >= 0.0f) {
                score += kScoreW[k] * comp[k];
                wsum += kScoreW[k];
            }
        }
        if (wsum < 0.5f) return -1.0f;
        score /= wsum;
        if (score < 0.0f) score = 0.0f;
        if (score > 100.0f) score = 100.0f;
        return score;
    }

    // 裁剪推理辅助：bbox 中心 1.5 倍裁剪到 SxS（insightface 对齐变换），
    // q = (p - c)*s + S/2 的逆映射 → warpAffine 矩阵
    bool warpCrop(const FaceBox &b, int S) {
        const float w = b.x2 - b.x1, h = b.y2 - b.y1;
        if (w < 4.0f || h < 4.0f) return false;
        const float cx = (b.x1 + b.x2) * 0.5f, cy = (b.y1 + b.y2) * 0.5f;
        const float s = (float)S / (std::max(w, h) * 1.5f);
        cv::Mat M = (cv::Mat_<float>(2, 3) <<
                     1.0f / s, 0.0f, cx - (S * 0.5f) / s,
                     0.0f, 1.0f / s, cy - (S * 0.5f) / s);
        // 注意：M 是逆变换（dst→src），必须带 WARP_INVERSE_MAP。
        // 此前漏了该标志：OpenCV 把 M 当正向变换再内部求逆，采样点全落负
        // 坐标 → 裁剪图全黑 → genderage/106det 一直吃全零输入（性别恒
        // 女35、连线框实为黑图模板脸按框缩放、颜值分项失真）
        cv::warpAffine(g_rgb, g_warp, M, cv::Size(S, S),
                       cv::INTER_LINEAR | cv::WARP_INVERSE_MAP,
                       cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
        return true;
    }

    // 106 关键点：raw RGB 0-255（图内自带 Sub127.5/Mul0.0078 归一化），输出逆映射回原图
    bool runLandmarks(const FaceBox &b, float *ptsOut) {
        const int S = g_lm_size;
        if (!warpCrop(b, S)) return false;
        const size_t px = (size_t)S * S;
        g_lm_blob.resize(px * 3);
        {
            const uint8_t *p = g_warp.data;
            float *rp = g_lm_blob.data();
            float *gp = rp + px;
            float *bp = gp + px;
            for (size_t i = 0; i < px; ++i, p += 3) {
                rp[i] = (float)p[0];
                gp[i] = (float)p[1];
                bp[i] = (float)p[2];
            }
        }
        const int64_t inShape[4] = {1, 3, S, S};
        const char *inNames[] = {g_lm_in.c_str()};
        const char *outNames[] = {g_lm_out.c_str()};
        try {
            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
                g_lm_blob.data(), px * 3, inShape, 4);
            auto outputs = g_lm_sess->Run(Ort::RunOptions{nullptr},
                                          inNames, &inTensor, 1, outNames, 1);
            if (outputs.empty() || !outputs[0].IsTensor()) return false;
            const size_t cnt = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (cnt < 212) return false;
            // 多出的行（如有）取最后 106 对
            const float *pred = outputs[0].GetTensorData<float>() + (cnt - 212);
            const float half = S * 0.5f;
            const float w = b.x2 - b.x1, h = b.y2 - b.y1;
            const float cx = (b.x1 + b.x2) * 0.5f, cy = (b.y1 + b.y2) * 0.5f;
            const float s = (float)S / (std::max(w, h) * 1.5f);
            for (int j = 0; j < 106; ++j) {
                // 越界 pred（<−1 或 >1，回归头输出可略超界）标记 NaN：
                // 逆映射后为远离脸的坐标，drawFacePoly 跳段、faceBeautyScore
                // 取有效点均值/回退。注："眉线拉向左上角"的真根因是分项与
                // 104/105 号点在输出布局重叠（已修，分项移至 [220..226]），
                // 此检查仅拦真正越界的模型输出
                const float qx = (pred[j * 2] + 1.0f) * half;
                const float qy = (pred[j * 2 + 1] + 1.0f) * half;
                if (!(qx > -24.0f && qx < (float)S + 24.0f &&
                      qy > -24.0f && qy < (float)S + 24.0f)) {
                    ptsOut[j * 2] = std::numeric_limits<float>::quiet_NaN();
                    ptsOut[j * 2 + 1] = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                ptsOut[j * 2] = (qx - half) / s + cx;
                ptsOut[j * 2 + 1] = (qy - half) / s + cy;
            }
            return true;
        } catch (const Ort::Exception &e) {
            LOGE("lm run Ort::Exception: %{public}s", e.what());
        } catch (...) {
            LOGE("lm run: unknown exception");
        }
        return false;
    }

    // 性别年龄：raw RGB 0-255，输出 [1,3]：gender=argmax(前2)，age=第3位×100；
    //   margin=|前2位 logit 差|，低于门槛不参与性别投票（低置信样本剔除）
    bool runGenderAge(const FaceBox &b, int &gender, float &age, float &margin) {
        if (!g_ga_sess) return false;
        const int S = g_ga_size;
        if (!warpCrop(b, S)) return false;
        const size_t px = (size_t)S * S;
        g_ga_blob.resize(px * 3);
        {
            const uint8_t *p = g_warp.data;
            float *rp = g_ga_blob.data();
            float *gp = rp + px;
            float *bp = gp + px;
            for (size_t i = 0; i < px; ++i, p += 3) {
                rp[i] = (float)p[0];
                gp[i] = (float)p[1];
                bp[i] = (float)p[2];
            }
        }
        const int64_t inShape[4] = {1, 3, S, S};
        const char *inNames[] = {g_ga_in.c_str()};
        const char *outNames[] = {g_ga_out.c_str()};
        // 诊断：GA 输入链路（每 10 次一条）——若 wmean/bmean≈0 则裁剪图全黑，
        // 若 bmean 正常但输出仍恒定则会话侧问题（见 init 处 DISABLE_ALL 注释）
        {
            static std::atomic<int> s_gaw{0};
            if ((s_gaw.fetch_add(1) % 10) == 0) {
                float wsum = 0.0f;
                const uint8_t *wp = g_warp.data;
                for (int i = 0; i < 192; ++i) wsum += (float)wp[i * 3];
                float bsum = 0.0f;
                const float *bp = g_ga_blob.data();
                for (int i = 0; i < 192; ++i) bsum += bp[i];
                OH_LOG_INFO(LOG_APP,
                            "[gaw] S=%{public}d warp=%{public}dx%{public}d "
                            "wmean=%{public}.1f bmean=%{public}.1f",
                            S, g_warp.cols, g_warp.rows,
                            wsum / 192.0f, bsum / 192.0f);
            }
        }
        try {
            Ort::Value inTensor = Ort::Value::CreateTensor<float>(
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
                g_ga_blob.data(), px * 3, inShape, 4);
            auto outputs = g_ga_sess->Run(Ort::RunOptions{nullptr},
                                          inNames, &inTensor, 1, outNames, 1);
            if (outputs.empty() || !outputs[0].IsTensor()) return false;
            const size_t cnt = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (cnt < 3) return false;
            const float *o = outputs[0].GetTensorData<float>();
            gender = (o[1] >= o[0]) ? 1 : 0;
            age = o[2] * 100.0f;
            margin = std::fabs(o[0] - o[1]);
            // 诊断：genderage 原始输出 + 裁剪框尺寸 + 框到画面四边余量
            //（余量≈0 说明脸被画面切边，性别年龄会严重失准）
            OH_LOG_INFO(LOG_APP,
                        "[ga] pred=[%{public}.3f %{public}.3f %{public}.3f] "
                        "g=%{public}d age=%{public}.1f margin=%{public}.2f "
                        "box=%{public}.0fx%{public}.0f "
                        "cut=[%{public}.0f %{public}.0f %{public}.0f %{public}.0f]",
                        o[0], o[1], o[2], gender, age, margin,
                        b.x2 - b.x1, b.y2 - b.y1,
                        b.x1, b.y1,
                        (float)g_frame_w - b.x2, (float)g_frame_h - b.y2);
            return true;
        } catch (const Ort::Exception &e) {
            LOGE("ga run Ort::Exception: %{public}s", e.what());
        } catch (...) {
            LOGE("ga run: unknown exception");
        }
        return false;
    }

}  // namespace

extern "C" {

/* 加载三个人脸模型（det/lm 必需，ga 可为 NULL），numThreads<=0 用 3 */
int face_init(const char *detPath, const char *lmPath, const char *gaPath, int numThreads) {
    std::lock_guard<std::mutex> lk(g_face_mutex);
    g_det_sess.reset();
    g_lm_sess.reset();
    g_ga_sess.reset();
    g_face_ready.store(false);
    g_tracks.clear();
    if (!detPath || !*detPath || !lmPath || !*lmPath) {
        LOGE("face_init: path empty");
        return 0;
    }
    if (numThreads <= 0) numThreads = 3;
    try {
        Ort::SessionOptions opt;
        opt.SetIntraOpNumThreads(numThreads);
        opt.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        g_det_sess = std::make_unique<Ort::Session>(faceEnv(), detPath, opt);
        g_lm_sess = std::make_unique<Ort::Session>(faceEnv(), lmPath, opt);
        if (gaPath && *gaPath) {
            g_ga_sess = std::make_unique<Ort::Session>(faceEnv(), gaPath, opt);
        }
        Ort::AllocatorWithDefaultOptions alloc;
        {
            auto n = g_det_sess->GetInputNameAllocated(0, alloc);
            g_det_in = n.get();
        }
        if (g_det_sess->GetOutputCount() != 9) {
            LOGE("face_init: det outputs=%{public}d, expect 9",
                 (int)g_det_sess->GetOutputCount());
            g_det_sess.reset();
            g_lm_sess.reset();
            g_ga_sess.reset();
            return 0;
        }
        for (int i = 0; i < 9; ++i) {
            auto n = g_det_sess->GetOutputNameAllocated(i, alloc);
            g_det_outs[i] = n.get();
        }
        {
            auto n = g_lm_sess->GetInputNameAllocated(0, alloc);
            g_lm_in = n.get();
            auto o = g_lm_sess->GetOutputNameAllocated(0, alloc);
            g_lm_out = o.get();
            auto info = g_lm_sess->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.size() == 4 && shape[2] > 0) g_lm_size = (int)shape[2];
            else g_lm_size = 192;
        }
        if (g_ga_sess) {
            auto n = g_ga_sess->GetInputNameAllocated(0, alloc);
            g_ga_in = n.get();
            auto o = g_ga_sess->GetOutputNameAllocated(0, alloc);
            g_ga_out = o.get();
            auto info = g_ga_sess->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo();
            auto shape = info.GetShape();
            if (shape.size() == 4 && shape[2] > 0) g_ga_size = (int)shape[2];
            else g_ga_size = 96;
        }
        g_face_ready.store(true);
        LOGI("face models ready: lm=%{public}d ga=%{public}d threads=%{public}d",
             g_lm_size, g_ga_size, numThreads);
        return 1;
    } catch (const Ort::Exception &e) {
        LOGE("face_init Ort::Exception: %{public}s", e.what());
    } catch (...) {
        LOGE("face_init: unknown exception");
    }
    g_det_sess.reset();
    g_lm_sess.reset();
    g_ga_sess.reset();
    g_face_ready.store(false);
    return 0;
}

int face_is_ready(void) {
    return g_face_ready.load() ? 1 : 0;
}

void face_release(void) {
    std::lock_guard<std::mutex> lk(g_face_mutex);
    g_det_sess.reset();
    g_lm_sess.reset();
    g_ga_sess.reset();
    g_face_ready.store(false);
    g_tracks.clear();
    g_det_blob.clear();
    g_det_blob.shrink_to_fit();
    g_lm_blob.clear();
    g_lm_blob.shrink_to_fit();
    g_ga_blob.clear();
    g_ga_blob.shrink_to_fit();
    LOGI("face released");
}

/* 同步检测一帧 RGBA。facesOut 每脸 kFaceFloats=227 个 float：
 * [0..3] box x1,y1,x2,y2（图像坐标） [4] 检测分
 * [5] 性别(0女1男,-1未知) [6] 年龄(-1未知) [7] 颜值 0~100(-1未知)
 * [8..219] 106 关键点 x,y（图像坐标）
 * [220..226] 颜值分项 0~100(-1未知)：对称/眼形/眉形/鼻形/嘴形/三庭/眼型
 * 返回脸数（<=min(maxFaces,4)），未就绪返回 0 */
int face_detect_rgba(const uint8_t *rgba, int w, int h, float *facesOut, int maxFaces) {
    if (!g_face_ready.load() || !rgba || !facesOut || w <= 0 || h <= 0 || maxFaces <= 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(g_face_mutex);
    if (!g_det_sess || !g_lm_sess) return 0;

    auto now = std::chrono::steady_clock::now();
    // track GC：3 秒未命中删除
    g_tracks.erase(std::remove_if(g_tracks.begin(), g_tracks.end(),
                                  [&](const FaceTrack &t) {
                                      return std::chrono::duration_cast<std::chrono::milliseconds>(
                                          now - t.lastSeen).count() > 3000;
                                  }),
                   g_tracks.end());

    // ① RGBA → RGB → letterbox 640x640（长边贴合等比缩放、短边 pad 114，insightface 官方方式），
    //    blob = (v-127.5)/128。
    //    此前按拉伸填满 640x640：宽画幅（折叠/展开比例不同）下人脸横/纵向畸变不一致，
    //    det 置信度与框质量随画幅比劣化，导致连线框错位/倾斜、展开后更严重
    cv::Mat rgbaMat(h, w, CV_8UC4, (void *)rgba);
    cv::cvtColor(rgbaMat, g_rgb, cv::COLOR_RGBA2RGB);
    g_frame_w = w;
    g_frame_h = h;
    const float detScale = std::min((float)kDetInput / (float)w,
                                    (float)kDetInput / (float)h);
    const int detNw = std::max(1, (int)std::lround((float)w * detScale));
    const int detNh = std::max(1, (int)std::lround((float)h * detScale));
    cv::resize(g_rgb, g_det_small, cv::Size(detNw, detNh), 0, 0, cv::INTER_LINEAR);
    if (g_det_mat.rows != kDetInput || g_det_mat.cols != kDetInput) {
        g_det_mat.create(kDetInput, kDetInput, CV_8UC3);
    }
    g_det_mat.setTo(cv::Scalar(114, 114, 114));
    g_det_small.copyTo(g_det_mat(cv::Rect((kDetInput - detNw) / 2,
                                          (kDetInput - detNh) / 2, detNw, detNh)));
    const float detPadX = (float)(kDetInput - detNw) * 0.5f;
    const float detPadY = (float)(kDetInput - detNh) * 0.5f;
    const size_t detPx = (size_t)kDetInput * kDetInput;
    g_det_blob.resize(detPx * 3);
    {
        const uint8_t *p = g_det_mat.data;
        float *rp = g_det_blob.data();
        float *gp = rp + detPx;
        float *bp = gp + detPx;
        for (size_t i = 0; i < detPx; ++i, p += 3) {
            rp[i] = ((float)p[0] - 127.5f) / 128.0f;
            gp[i] = ((float)p[1] - 127.5f) / 128.0f;
            bp[i] = ((float)p[2] - 127.5f) / 128.0f;
        }
    }

    // ② SCRFD 推理（9 输出）
    std::vector<FaceBox> cands;
    try {
        const int64_t inShape[4] = {1, 3, kDetInput, kDetInput};
        Ort::Value inTensor = Ort::Value::CreateTensor<float>(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault),
            g_det_blob.data(), detPx * 3, inShape, 4);
        const char *inNames[] = {g_det_in.c_str()};
        const char *outNames[9] = {g_det_outs[0].c_str(), g_det_outs[1].c_str(),
                                   g_det_outs[2].c_str(), g_det_outs[3].c_str(),
                                   g_det_outs[4].c_str(), g_det_outs[5].c_str(),
                                   g_det_outs[6].c_str(), g_det_outs[7].c_str(),
                                   g_det_outs[8].c_str()};
        auto outputs = g_det_sess->Run(Ort::RunOptions{nullptr},
                                       inNames, &inTensor, 1, outNames, 9);
        if (outputs.size() < 9) return 0;

        // ③ 解码：anchor 在整数 stride 网格点，每格 2 anchor（i/2=cell），
        //    bbox/kps 增量 ×stride
        for (int si = 0; si < 3; ++si) {
            const int stride = kDetStrides[si];
            const int cols = kDetInput / stride;
            const int K = (kDetInput / stride) * cols;
            const size_t nScores = outputs[si].GetTensorTypeAndShapeInfo().GetElementCount();
            const size_t nBbox = outputs[si + 3].GetTensorTypeAndShapeInfo().GetElementCount();
            const size_t nKps = outputs[si + 6].GetTensorTypeAndShapeInfo().GetElementCount();
            if (nScores < (size_t)K * 2 || nBbox < (size_t)K * 8 || nKps < (size_t)K * 20) {
                continue;
            }
            const float *scores = outputs[si].GetTensorData<float>();
            const float *bbox = outputs[si + 3].GetTensorData<float>();
            const float *kps = outputs[si + 6].GetTensorData<float>();
            for (int i = 0; i < K * 2; ++i) {
                const float score = scores[i];
                if (score < kDetThresh) continue;
                const int cell = i / 2;
                const float ax = (float)((cell % cols) * stride);
                const float ay = (float)((cell / cols) * stride);
                FaceBox b;
                b.x1 = ax - bbox[i * 4 + 0] * stride;
                b.y1 = ay - bbox[i * 4 + 1] * stride;
                b.x2 = ax + bbox[i * 4 + 2] * stride;
                b.y2 = ay + bbox[i * 4 + 3] * stride;
                for (int n = 0; n < 5; ++n) {
                    b.kps[n * 2] = ax + kps[i * 10 + n * 2] * stride;
                    b.kps[n * 2 + 1] = ay + kps[i * 10 + n * 2 + 1] * stride;
                }
                b.score = score;
                cands.push_back(b);
            }
        }
    } catch (const Ort::Exception &e) {
        LOGE("det run Ort::Exception: %{public}s", e.what());
        g_face_ready.store(false);
        return 0;
    } catch (...) {
        LOGE("det run: unknown exception");
        return 0;
    }
    if (cands.empty()) return 0;

    // ④ NMS
    std::sort(cands.begin(), cands.end(),
              [](const FaceBox &a, const FaceBox &b) { return a.score > b.score; });
    std::vector<FaceBox> keep;
    for (const auto &c : cands) {
        bool ok = true;
        for (const auto &k : keep) {
            if (boxIou(c, k) > kNmsThresh) { ok = false; break; }
        }
        if (ok) keep.push_back(c);
        if (keep.size() >= 8) break;
    }
    if (keep.empty()) return 0;

    // 诊断：det 节流日志（每 120 帧约 30s 一条，看 letterbox 参数与检测置信度）
    {
        static std::atomic<int> s_det_log{0};
        if ((s_det_log.fetch_add(1) % 120) == 0) {
            OH_LOG_INFO(LOG_APP,
                        "[det] in=%{public}dx%{public}d lb=%{public}dx%{public}d "
                        "scale=%{public}.4f best=%{public}.3f n=%{public}zu",
                        w, h, detNw, detNh, detScale, keep.front().score,
                        keep.size());
        }
    }

    // ⑤ 坐标回原图（letterbox 逆映射：减 pad 再除 scale）
    for (auto &b : keep) {
        b.x1 = (b.x1 - detPadX) / detScale;
        b.y1 = (b.y1 - detPadY) / detScale;
        b.x2 = (b.x2 - detPadX) / detScale;
        b.y2 = (b.y2 - detPadY) / detScale;
        for (int n = 0; n < 5; ++n) {
            b.kps[n * 2] = (b.kps[n * 2] - detPadX) / detScale;
            b.kps[n * 2 + 1] = (b.kps[n * 2 + 1] - detPadY) / detScale;
        }
    }

    // ⑥ 每脸：106 关键点（必有）+ 性别年龄（track 节流）+ 颜值
    const size_t outCap = std::min((size_t)maxFaces, kMaxFacesOut);
    int n = 0;
    for (const auto &b : keep) {
        if ((size_t)n >= outCap) break;
        float *o = facesOut + (size_t)n * kFaceFloats;
        o[0] = b.x1; o[1] = b.y1; o[2] = b.x2; o[3] = b.y2;
        o[4] = b.score;

        // track 匹配（IoU≥0.3）
        FaceTrack *tr = nullptr;
        float bestIoU = 0.0f;
        for (auto &t : g_tracks) {
            FaceBox tb = {};
            tb.x1 = t.x1; tb.y1 = t.y1; tb.x2 = t.x2; tb.y2 = t.y2;
            const float io = boxIou(b, tb);
            if (io > bestIoU) { bestIoU = io; tr = &t; }
        }
        if (bestIoU < 0.3f) tr = nullptr;
        bool needGa = false;
        if (!tr) {
            if (g_tracks.size() >= kMaxTracks) g_tracks.erase(g_tracks.begin());
            FaceTrack nt;
            nt.x1 = b.x1; nt.y1 = b.y1; nt.x2 = b.x2; nt.y2 = b.y2;
            nt.lastSeen = now;
            nt.lastGa = {};
            g_tracks.push_back(nt);
            tr = &g_tracks.back();
            needGa = true;
        } else {
            // 面积剧变（人脸大幅靠近/远离，或换人）→ 旧性别投票与年龄失效，清空重投
            const float area = (b.x2 - b.x1) * (b.y2 - b.y1);
            const float tArea = (tr->x2 - tr->x1) * (tr->y2 - tr->y1);
            if (tArea > 1.0f && (area > tArea * 1.6f || area < tArea * 0.625f)) {
                for (int vi = 0; vi < 5; ++vi) tr->votes[vi] = -1;
                tr->voteIdx = 0;
                tr->voteCnt = 0;
                tr->gender = -1;
                tr->age = -1.0f;
                tr->ageIdx = 0;
                tr->ageCnt = 0;
                needGa = true;
            }
            tr->x1 = b.x1; tr->y1 = b.y1; tr->x2 = b.x2; tr->y2 = b.y2;
            tr->lastSeen = now;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - tr->lastGa).count() >= kGaRefreshMs) {
                needGa = true;
            }
        }
        // 性别年龄：按需推理，性别 5 票多数表决（置信度门槛），年龄 EMA 平滑
        if (needGa) {
            int g = -1;
            float a = -1.0f;
            float m = 0.0f;
            if (runGenderAge(b, g, a, m)) {
                if (m >= kGaVoteMargin) {
                    tr->votes[tr->voteIdx] = g;
                    tr->voteIdx = (tr->voteIdx + 1) % 5;
                    tr->voteCnt = std::min(5, tr->voteCnt + 1);
                }
                int cnt[2] = {0, 0};
                for (int i = 0; i < tr->voteCnt; ++i) {
                    if (tr->votes[i] >= 0) cnt[tr->votes[i]]++;
                }
                // 零有效票时保持 -1（未知），不能默认判女
                tr->gender = (tr->voteCnt == 0) ? -1 : ((cnt[1] > cnt[0]) ? 1 : 0);
                // 年龄：环形 7 样本中值——光线突变只是少数离群样本，
                // 中值稳健，比 EMA 抗光线抖动（EMA 每次都被拉偏）
                tr->ages[tr->ageIdx] = a;
                tr->ageIdx = (tr->ageIdx + 1) % 7;
                tr->ageCnt = std::min(7, tr->ageCnt + 1);
                // ★ 年龄校准：genderage 模型输出普遍偏大（实测同一人偏大约 4 岁），
                //   中值滤波抗抖后统一减 4，下限 1 岁
                tr->age = medianOf(tr->ages, tr->ageCnt) - 4.0f;
                if (tr->age < 1.0f) tr->age = 1.0f;
                tr->lastGa = now;
            }
        }
        o[5] = (float)tr->gender;
        o[6] = tr->age;

        // 106 关键点 + 颜值（含 7 分项：对称/眼形/眉形/鼻形/嘴形/三庭/眼型）
        float *pts = o + 8;
        if (runLandmarks(b, pts)) {
            float comp[7] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
            float dbg[8] = {0};
            const float beauty = faceBeautyScore(pts, comp, dbg);
            o[7] = beauty;
            // ★ 分项写 [220..226]：106 点占 [8..219]，旧版写在 [216..222]
            //   与末 4 个 float（104/105 号点=左眉上沿）重叠——分项分值
            //   （0~100 小值）当作帧坐标画出，就是"直线拉向左上角"的根因
            for (int ci = 0; ci < 7; ++ci) o[220 + ci] = comp[ci];
            // 诊断：分项与原始比值（每 30 脸一条）——定位某分项异常是关键点
            // 错位还是理想值校准问题
            {
                static std::atomic<int> s_beauty{0};
                if ((s_beauty.fetch_add(1) % 30) == 0) {
                    OH_LOG_INFO(LOG_APP,
                                "[beauty] d=%{public}.0f sym=%{public}.0f eye=%{public}.0f "
                                "brow=%{public}.0f nose=%{public}.0f mouth=%{public}.0f "
                                "thirds=%{public}.0f aspect=%{public}.0f | "
                                "r: eye=%{public}.2f browR=%{public}.2f browL=%{public}.2f "
                                "nose=%{public}.2f mouth=%{public}.2f thirds=%{public}.2f "
                                "aspect=%{public}.2f",
                                dbg[0], comp[0], comp[1], comp[2], comp[3], comp[4],
                                comp[5], comp[6], dbg[1], dbg[2], dbg[3], dbg[4],
                                dbg[5], dbg[6], dbg[7]);
                }
            }
        } else {
            std::memset(pts, 0, sizeof(float) * 212);
            o[7] = -1.0f;
            for (int ci = 0; ci < 7; ++ci) o[220 + ci] = -1.0f;
        }
        ++n;
    }
    return n;
}

}  // extern "C"
