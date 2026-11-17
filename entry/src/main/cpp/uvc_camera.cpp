// ★ UVC 摄像头取流（libuvc fd 注入）：格式协商（NegotiateMax 最高分辨率优先）、
//   MJPEG/YUYV 帧解码为 RGBA 送渲染管线、帧诊断计数
#include "uvc_camera.h"

#include <cerrno>
#include <cstring>
#include <hilog/log.h>
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from_argb.h>

#include "libuvc/libuvc_internal.h"
#include "yuv_pipeline.h"

#define UVC_TAG "UvcCamera"
#define LOG_I(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0, UVC_TAG, __VA_ARGS__)
#define LOG_W(...) OH_LOG_Print(LOG_APP, LOG_WARN, 0, UVC_TAG, __VA_ARGS__)
#define LOG_E(...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0, UVC_TAG, __VA_ARGS__)

namespace {
constexpr const char *kUsbFs = "/dev/bus/usb";
}

UvcCamera &UvcCamera::Instance() {
    static UvcCamera inst;
    return inst;
}

UvcCamera::~UvcCamera() {
    Disconnect();
}

// 记录失败阶段与错误码（含 errno/strerror，便于定位 fd 读取/描述符解析类问题）
void UvcCamera::SetError(const char *stage, int code) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s ret=%d errno=%d(%s)",
             stage, code, errno, strerror(errno));
    lastError_ = buf;
    LOG_E("%{public}s", buf);
}

bool UvcCamera::Connect(int fd, int vid, int pid, int busnum, int devaddr) {
    Disconnect();
    lastError_.clear();

    if (fd <= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "invalid fd=%d", fd);
        lastError_ = buf;
        LOG_E("invalid fd=%{public}d", fd);
        return false;
    }

    uvc_error_t r = uvc_init2(&ctx_, nullptr, kUsbFs);
    if (r != UVC_SUCCESS) {
        SetError("uvc_init2", (int)r);
        ctx_ = nullptr;
        return false;
    }

    r = uvc_get_device_with_fd(ctx_, &dev_, vid, pid, nullptr, fd, busnum, devaddr);
    if (r != UVC_SUCCESS) {
        char stage[96];
        snprintf(stage, sizeof(stage),
                 "uvc_get_device_with_fd vid=%x pid=%x bus=%d addr=%d",
                 vid, pid, busnum, devaddr);
        SetError(stage, (int)r);
        uvc_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    r = uvc_open(dev_, &devh_);
    if (r != UVC_SUCCESS) {
        SetError("uvc_open", (int)r);
        uvc_unref_device(dev_);
        dev_ = nullptr;
        uvc_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    connected_.store(true);
    LOG_I("UVC connected vid=%{public}x pid=%{public}x fd=%{public}d", vid, pid, fd);
    return true;
}

// ★ 设备实际上报的格式/分辨率清单：诊断 -51 INVALID_MODE 的关键
//   （GUID 前缀 MJPG=4d4a5047、YUY2=59555932）
std::string UvcCamera::DumpFormats() const {
    std::string out;
    if (!devh_ || !devh_->info) return "(no device handle)";
    uvc_streaming_interface_t *stream_if;
    DL_FOREACH(devh_->info->stream_ifs, stream_if) {
        uvc_format_desc_t *fmt;
        DL_FOREACH(stream_if->format_descs, fmt) {
            char guid[36] = {0};
            for (int i = 0; i < 4; i++) {
                snprintf(guid + i * 2, 3, "%02x", fmt->guidFormat[i]);
            }
            out += " if" + std::to_string((int)stream_if->bInterfaceNumber)
                 + " fmt#" + std::to_string((int)fmt->bFormatIndex)
                 + " guid=" + std::string(guid);
            uvc_frame_desc_t *fr;
            int shown = 0;
            DL_FOREACH(fmt->frame_descs, fr) {
                if (shown++ < 10) {
                    out += " " + std::to_string((int)fr->wWidth)
                         + "x" + std::to_string((int)fr->wHeight);
                }
            }
            if (shown > 10) out += " ...(" + std::to_string(shown) + " frames)";
            out += ";";
        }
    }
    if (out.empty()) return "(no stream_ifs/format_descs parsed)";
    return out;
}

// ★ 直接上设备支持的最高分辨率（用户要求，不再默认 640x480）：
//   遍历 MJPG/YUY2 格式描述，挑面积最大的帧协商；
//   优先选最高帧率 ≥15fps 的帧（避免协商到 5fps 的静态大头照档位变幻灯片），
//   该条件下无帧时退回绝对最大面积（哪怕低帧率）
bool UvcCamera::NegotiateMax() {
    if (!devh_ || !devh_->info) return false;
    static const struct {
        const char *tag;
        enum uvc_frame_format fmt;
    } prefs[] = {
        {"MJPG", UVC_FRAME_FORMAT_MJPEG},
        {"YUY2", UVC_FRAME_FORMAT_YUYV},
    };
    for (int p = 0; p < 2; p++) {
        uvc_streaming_interface_t *stream_if;
        // 先在 ≥15fps 的帧里挑最大面积，找不到再无条件挑最大面积
        for (int pass = 0; pass < 2; pass++) {
            uvc_frame_desc_t *bestFr = nullptr;
            long bestArea = 0;
            int minFps = 1, maxFps = 120;
            DL_FOREACH(devh_->info->stream_ifs, stream_if) {
                uvc_format_desc_t *fmt;
                DL_FOREACH(stream_if->format_descs, fmt) {
                    if (memcmp(fmt->guidFormat, prefs[p].tag, 4) != 0) continue;
                    uvc_frame_desc_t *fr;
                    DL_FOREACH(fmt->frame_descs, fr) {
                        int frMin = 1, frMax = 120;
                        if (fr->intervals && fr->intervals[0]) {
                            uint32_t maxI = fr->intervals[0], minI = fr->intervals[0];
                            for (uint32_t *it = fr->intervals; *it; ++it) {
                                if (*it > maxI) maxI = *it;
                                if (*it < minI) minI = *it;
                            }
                            if (maxI > 0 && minI > 0) {
                                frMin = (int)(10000000ULL / maxI);
                                frMax = (int)(10000000ULL / minI);
                                if (frMin < 1) frMin = 1;
                                if (frMax < frMin) frMax = frMin;
                            }
                        }
                        if (pass == 0 && frMax < 15) continue;
                        const long area = (long)fr->wWidth * (long)fr->wHeight;
                        if (area > bestArea) {
                            bestArea = area;
                            bestFr = fr;
                            minFps = frMin;
                            maxFps = frMax;
                        }
                    }
                }
            }
            if (!bestFr) continue;
            uvc_error_t r = uvc_get_stream_ctrl_format_size_fps(
                devh_, &ctrl_, prefs[p].fmt,
                (int)bestFr->wWidth, (int)bestFr->wHeight, minFps, maxFps);
            if (r == UVC_SUCCESS) {
                LOG_I("NegotiateMax ok: %{public}s %{public}dx%{public}d @%{public}d-%{public}d fps",
                      prefs[p].tag, (int)bestFr->wWidth, (int)bestFr->wHeight, minFps, maxFps);
                lastNegW_ = (int)bestFr->wWidth;
                lastNegH_ = (int)bestFr->wHeight;
                return true;
            }
            LOG_W("NegotiateMax %{public}s %{public}dx%{public}d ret=%{public}d",
                  prefs[p].tag, (int)bestFr->wWidth, (int)bestFr->wHeight, (int)r);
        }
    }
    return false;
}

// ★ 遍历设备上报的 MJPEG/YUYV 格式与分辨率逐个协商：
//   部分相机不支持 640x480（固定 1080p/720p 等），固定参数协商会得到 -51 INVALID_MODE
bool UvcCamera::AutoNegotiate() {
    if (!devh_ || !devh_->info) return false;
    static const struct {
        const char *tag;
        enum uvc_frame_format fmt;
    } prefs[] = {
        {"MJPG", UVC_FRAME_FORMAT_MJPEG},
        {"YUY2", UVC_FRAME_FORMAT_YUYV},
    };
    for (int p = 0; p < 2; p++) {
        uvc_streaming_interface_t *stream_if;
        DL_FOREACH(devh_->info->stream_ifs, stream_if) {
            uvc_format_desc_t *fmt;
            DL_FOREACH(stream_if->format_descs, fmt) {
                if (memcmp(fmt->guidFormat, prefs[p].tag, 4) != 0) continue;
                uvc_frame_desc_t *fr;
                DL_FOREACH(fmt->frame_descs, fr) {
                    // fps 范围从该帧的 intervals 推导；无 intervals 用宽范围兜底
                    int minFps = 1, maxFps = 120;
                    if (fr->intervals && fr->intervals[0]) {
                        uint32_t maxI = fr->intervals[0], minI = fr->intervals[0];
                        for (uint32_t *it = fr->intervals; *it; ++it) {
                            if (*it > maxI) maxI = *it;
                            if (*it < minI) minI = *it;
                        }
                        if (maxI > 0 && minI > 0) {
                            minFps = (int)(10000000ULL / maxI);
                            maxFps = (int)(10000000ULL / minI);
                            if (minFps < 1) minFps = 1;
                            if (maxFps < minFps) maxFps = minFps;
                        }
                    }
                    uvc_error_t r = uvc_get_stream_ctrl_format_size_fps(
                        devh_, &ctrl_, prefs[p].fmt,
                        (int)fr->wWidth, (int)fr->wHeight, minFps, maxFps);
                    if (r == UVC_SUCCESS) {
                        LOG_I("AutoNegotiate ok: %{public}s %{public}dx%{public}d @%{public}d-%{public}d fps",
                              prefs[p].tag, (int)fr->wWidth, (int)fr->wHeight, minFps, maxFps);
                        lastNegW_ = (int)fr->wWidth;
                        lastNegH_ = (int)fr->wHeight;
                        return true;
                    }
                    LOG_W("AutoNegotiate %{public}s %{public}dx%{public}d@%{public}d-%{public}d ret=%{public}d",
                          prefs[p].tag, (int)fr->wWidth, (int)fr->wHeight, minFps, maxFps, (int)r);
                }
            }
        }
    }
    return false;
}

bool UvcCamera::Start(YuvPipeline *pipeline, int width, int height, int fps) {
    if (!devh_ || !pipeline || !connected_.load()) {
        SetError("Start: not ready (no device or not connected)", 0);
        return false;
    }
    if (streaming_.load()) {
        Stop();
    }
    pipeline_ = pipeline;

    // 对齐 Android 端：MJPEG 优先（等时带宽占用低），
    // 失败依次回退 YUYV@fps、YUYV@15
    // ★ 诊断增强：三次尝试的返回码与 errno 全部记入 lastError_
    //   （固定参数协商成功时即为请求尺寸，AutoNegotiate 成功时会被实际尺寸覆盖）
    lastNegW_ = width;
    lastNegH_ = height;
    // ★ 优先直接上设备最高分辨率；失败回退固定参数链（MJPEG w×h → YUYV → YUYV@15），
    //   再失败 AutoNegotiate 兜底
    uvc_error_t r;
    if (!NegotiateMax()) {
        r = uvc_get_stream_ctrl_format_size_fps(
            devh_, &ctrl_, UVC_FRAME_FORMAT_MJPEG, width, height, 0, fps);
        if (r != UVC_SUCCESS) {
            const int r1 = (int)r;
            const int e1 = errno;
            LOG_W("MJPEG %{public}dx%{public}d@0-%{public}d unavailable: %{public}d errno=%{public}d, try YUYV",
                  width, height, fps, r1, e1);
            r = uvc_get_stream_ctrl_format_size_fps(
                devh_, &ctrl_, UVC_FRAME_FORMAT_YUYV, width, height, 0, fps);
            const int r2 = (int)r;
            const int e2 = errno;
            if (r != UVC_SUCCESS) {
                LOG_W("YUYV@%{public}d unavailable: %{public}d errno=%{public}d, try YUYV@15", fps, r2, e2);
                r = uvc_get_stream_ctrl_format_size_fps(
                    devh_, &ctrl_, UVC_FRAME_FORMAT_YUYV, width, height, 0, 15);
                const int r3 = (int)r;
                const int e3 = errno;
                if (r != UVC_SUCCESS) {
                    char buf[224];
                    snprintf(buf, sizeof(buf),
                             "negotiate MJPEG ret=%d errno=%d | YUYV@%d ret=%d errno=%d | YUYV@15 ret=%d errno=%d",
                             r1, e1, fps, r2, e2, r3, e3);
                    lastError_ = buf;
                    LOG_E("%{public}s", buf);
                    // ★ 固定参数全部失败（如 -51 INVALID_MODE：相机不支持该分辨率）：
                    //   按设备实际上报的格式/分辨率自动协商兜底
                    if (AutoNegotiate()) {
                        lastError_.clear();
                    } else {
                        // 自动协商也失败：带上设备格式清单，便于诊断 GUID 异常/流接口缺失
                        lastError_ += " | dev formats: " + DumpFormats();
                        return false;
                    }
                }
            }
        }
    }

    // ★ MJPEG 解码输出帧缓冲按首帧尺寸一次性分配：换分辨率重开时（如先 640x480
    //   后 1920x1080）旧缓冲过小会解码失败，这里统一释放按新协商尺寸重建
    if (mjpegOut_) {
        uvc_free_frame(mjpegOut_);
        mjpegOut_ = nullptr;
    }

    // ★ 画面比例修复：setupExternalPipeline(640,480) 只是先建管线占位，
    //   相机实出 1920x1080(16:9) 时若仍按 640x480(4:3) 缩放会纵向压扁变形。
    //   按实际协商尺寸重配管线（含预览 surface buffer geometry），消除中间变形
    if (pipeline_) {
        pipeline_->ConfigureExternal(lastNegW_, lastNegH_);
    }

    // ★ 记录实际协商结果并重置帧诊断计数（看门狗据此判断"有连接无画面"）
    LOG_I("negotiated %{public}dx%{public}d bFormatIndex=%{public}u bFrameIndex=%{public}u interval=%{public}u",
          lastNegW_, lastNegH_, ctrl_.bFormatIndex, ctrl_.bFrameIndex, ctrl_.dwFrameInterval);
    frameCount_.store(0);
    decodeFailCount_.store(0);
    lastStatTime_ = std::chrono::steady_clock::now();

    r = uvc_start_streaming_bandwidth(devh_, &ctrl_, &UvcCamera::FrameCb, this, 1.0f, 0);
    if (r != UVC_SUCCESS) {
        SetError("uvc_start_streaming_bandwidth", (int)r);
        return false;
    }

    streaming_.store(true);
    LOG_I("UVC streaming %{public}dx%{public}d", lastNegW_, lastNegH_);
    return true;
}

void UvcCamera::Stop() {
    if (devh_ && streaming_.load()) {
        uvc_stop_streaming(devh_);
        LOG_I("UVC streaming stopped");
    }
    streaming_.store(false);
}

void UvcCamera::Disconnect() {
    Stop();
    if (devh_) {
        uvc_close(devh_);
        devh_ = nullptr;
    }
    if (dev_) {
        uvc_unref_device(dev_);
        dev_ = nullptr;
    }
    if (ctx_) {
        uvc_exit(ctx_);
        ctx_ = nullptr;
    }
    if (mjpegOut_) {
        uvc_free_frame(mjpegOut_);
        mjpegOut_ = nullptr;
    }
    connected_.store(false);
    pipeline_ = nullptr;
    LOG_I("UVC disconnected");
}

void UvcCamera::FrameCb(uvc_frame_t *frame, void *user) {
    static_cast<UvcCamera *>(user)->OnFrame(frame);
}

// ★ 首帧诊断信息：帧描述尺寸 vs JPEG 头真实尺寸 vs actual_bytes/帧头字节。
//   "连接成功但黑屏"时据此一次定位：
//   actual_bytes=0 → 帧空（内核取流/接触问题）；
//   jpeg_dim != 帧描述尺寸 → 相机固件尺寸造假（已由 mjpeg2rgbx 补丁兼容）；
//   head 非 ffd8 开头 → 帧数据损坏（带宽不足/接触不良）
void UvcCamera::RecordFrameInfo(const uvc_frame_t *frame) {
    const uint8_t *d = (const uint8_t *)frame->data;
    const size_t ab = frame->actual_bytes;
    char head[20] = {0};
    const size_t headN = ab < 8 ? ab : 8;
    for (size_t i = 0; i < headN; i++) {
        snprintf(head + i * 2, 3, "%02x", d[i]);
    }
    // 尾部 4 字节 + EOI(ffd9) 存在性：帧被截断（带宽/接触）时 EOI 缺失
    char tail[12] = {0};
    const size_t tailOff = ab >= 4 ? ab - 4 : 0;
    const size_t tailN = ab >= 4 ? 4 : ab;
    for (size_t i = 0; i < tailN; i++) {
        snprintf(tail + i * 2, 3, "%02x", d[tailOff + i]);
    }
    int eoi = 0;
    const size_t eoiFrom = ab > 64 ? ab - 64 : 0;
    for (size_t i = eoiFrom; i + 1 < ab; i++) {
        if (d[i] == 0xff && d[i + 1] == 0xd9) { eoi = 1; break; }
    }
    // 扫描 SOF0/SOF2 标记取 JPEG 真实宽高（在 SOS 前的头部区域）
    int jw = 0, jh = 0;
    const size_t scanLen = ab < 4096 ? ab : 4096;
    for (size_t i = 0; i + 9 < scanLen; i++) {
        if (d[i] == 0xff && (d[i + 1] == 0xc0 || d[i + 1] == 0xc2)) {
            jh = (d[i + 5] << 8) | d[i + 6];
            jw = (d[i + 7] << 8) | d[i + 8];
            break;
        }
    }
    char buf[256];
    snprintf(buf, sizeof(buf),
             "first frame: desc=%dx%d actual_bytes=%zu head=%s tail=%s eoi=%d jpeg_dim=%dx%d",
             (int)frame->width, (int)frame->height, ab, head, tail, eoi, jw, jh);
    {
        std::lock_guard<std::mutex> lock(frameInfoMutex_);
        lastFrameInfo_ = buf;
    }
    LOG_I("%{public}s", buf);
}

std::string UvcCamera::GetFrameInfo() {
    std::lock_guard<std::mutex> lock(frameInfoMutex_);
    return lastFrameInfo_.empty() ? "(no frame yet)" : lastFrameInfo_;
}

void UvcCamera::OnFrame(uvc_frame_t *frame) {
    if (!streaming_.load() || !pipeline_ || !frame || !frame->data) {
        return;
    }
    const int w = (int)frame->width;
    const int h = (int)frame->height;
    if (w <= 0 || h <= 0) {
        return;
    }

    // ★ 帧诊断：首帧记录格式尺寸，之后每 5s 输出一次统计
    //   （"连接成功但黑屏"时：帧数 0=接触/带宽问题，帧数涨=取流正常）
    const uint32_t n = frameCount_.fetch_add(1) + 1;
    if (n == 1) {
        LOG_I("first frame %{public}dx%{public}d fmt=%{public}d", w, h, (int)frame->frame_format);
        if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
            RecordFrameInfo(frame);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (n == 1 || std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - lastStatTime_).count() >= 5000) {
        LOG_I("frame stats: total=%{public}u decodeFail=%{public}u", n, decodeFailCount_.load());
        lastStatTime_ = now;
    }

    if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
        if (!mjpegOut_) {
            mjpegOut_ = uvc_allocate_frame((size_t)w * h * 4);
            if (!mjpegOut_) {
                LOG_E("uvc_allocate_frame failed");
                return;
            }
        }
        // JCS_EXT_RGBA 直接输出 RGBA，与管线字节序一致
        const uvc_error_t dr = uvc_mjpeg2rgbx(frame, mjpegOut_);
        if (dr == UVC_SUCCESS && mjpegOut_->data) {
            pipeline_->ProcessExternalRgba((const uint8_t *)mjpegOut_->data,
                                           (int)mjpegOut_->width, (int)mjpegOut_->height);
        } else {
            // ★ MJPEG 解码失败也计数：帧在到达但全解码失败时画面同样黑，需区分
            const uint32_t df = decodeFailCount_.fetch_add(1) + 1;
            if (df == 1) {
                // ★ 附带 libjpeg 具体报错（frame-mjpeg.c 无条件捕获）：
                //   "Unsupported color conversion request"/"Premature end"...一次定位真因
                char buf[352];
                snprintf(buf, sizeof(buf), " | mjpeg2rgbx ret=%d jpeg_err='%s'",
                         (int)dr, uvc_jpeg_last_error());
                {
                    std::lock_guard<std::mutex> lock(frameInfoMutex_);
                    lastFrameInfo_ += buf;
                }
                LOG_W("mjpeg2rgbx failed ret=%{public}d jpeg_err='%{public}s' frame=%{public}dx%{public}d",
                      (int)dr, uvc_jpeg_last_error(), w, h);
            }
        }
    } else if (frame->frame_format == UVC_FRAME_FORMAT_YUYV) {
        const size_t rgbaSize = (size_t)w * h * 4;
        if (yuyvBuf_.size() < rgbaSize) {
            yuyvBuf_.resize(rgbaSize);
        }
        // libyuv ARGB 内存序为 B,G,R,A → SIMD 原地交换 R/B 得到 R,G,B,A
        // （ARGBToBGRA 内部 ARGBShuffleRow 支持原地，替换旧版逐像素手写交换循环）
        libyuv::YUY2ToARGB((const uint8_t *)frame->data, w * 2,
                           yuyvBuf_.data(), w * 4, w, h);
        libyuv::ARGBToBGRA(yuyvBuf_.data(), w * 4,
                           yuyvBuf_.data(), w * 4, w, h);
        pipeline_->ProcessExternalRgba(yuyvBuf_.data(), w, h);
    }
}
