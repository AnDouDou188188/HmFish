// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrLiteImpl.cpp —— OCR 流程编排实现（det → 可选 cls → rec 三网调度）          ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】OcrLite 门面的全部实现。多入口（文件/Mat/字节流/位图）统一归一到      ║
// ║   私有核心 detect(src, originRect, scale)，核心流程依次：                     ║
// ║   ① DbNet.getTextBoxes 文本检测（DB 后处理出四点框）；                        ║
// ║   ② drawTextBoxes 画结果图 +（HmFish 定制）detStageCallback 先把检测框        ║
// ║      回调给 UI 画"识别中"灰虚线框；                                           ║
// ║   ③ getPartImages 透视校正裁小图；                                            ║
// ║   ④ AngleNet.getAngles 方向分类（doAngle=false 时整网跳过）并旋转倒置行；     ║
// ║   ⑤ CrnnNet.getTextLines 逐行识别（CTC 解码）；                               ║
// ║   ⑥ 聚合 TextBlock/OcrResult，坐标扣除 padding 白边后返回。                   ║
// ║ 【流水线位置】HmFish 中被 RapidOCR.cpp 以 g_ocr 唯一实例调用：                ║
// ║   detect(bgr, padding=50, maxSideLen=1024, ..., doAngle=false)。              ║
// ║ 【与 Android 原版差异】新增 detStageCallback 两阶段懒加载回调段               ║
// ║   （try/catch 包裹，回调异常不影响识别主流程）；其余与原版一致。              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "OcrLiteImpl.h"
#include "OcrUtils.h"
#include <stdarg.h> //windows&linux

// ───────────────────────── 生命周期与配置 ─────────────────────────

// ★ 构造：只分配 8KB 日志缓冲；三个网络对象（dbNet/angleNet/crnnNet）按
//   成员默认构造，真正的 ONNX 会话等 initModels 才创建
OcrLiteImpl::OcrLiteImpl() {
    loggerBuffer = (char *)malloc(8192);
}

// ★ 析构：若开启过结果 txt 则关闭文件句柄，再释放日志缓冲
OcrLiteImpl::~OcrLiteImpl() {
    if (isOutputResultTxt) {
        fclose(resultTxt);
    }
    free(loggerBuffer);
}

// ★ setNumThread：三网统一设置 ORT 推理线程数（须在 initModels 前调，
//   线程配置在建会话时生效）
void OcrLiteImpl::setNumThread(int numOfThread) {
    dbNet.setNumThread(numOfThread);
    angleNet.setNumThread(numOfThread);
    crnnNet.setNumThread(numOfThread);
}

// ★ initLogger：输出开关初始化——isConsole 日志打印控制台、isPartImg
//   中间小图落盘、isResultImg 结果图落盘
void OcrLiteImpl::initLogger(bool isConsole, bool isPartImg, bool isResultImg) {
    isOutputConsole = isConsole;
    isOutputPartImg = isPartImg;
    isOutputResultImg = isResultImg;
}

// ★ enableResultTxt：打开 <path><imgName>-result.txt，此后 Logger 输出
//   会同时追加写入该文件
void OcrLiteImpl::enableResultTxt(const char *path, const char *imgName) {
    isOutputResultTxt = true;
    std::string resultTxtPath = getResultTxtFilePath(path, imgName);
    printf("resultTxtPath(%s)\n", resultTxtPath.c_str());
    resultTxt = fopen(resultTxtPath.c_str(), "w");
}

// ★ setGpuIndex：三网统一设置 GPU 设备号（CPU 构建/ HarmonyOS 下为空操作）
void OcrLiteImpl::setGpuIndex(int gpuIndex) {
    dbNet.setGpuIndex(gpuIndex);
    angleNet.setGpuIndex(gpuIndex);
    crnnNet.setGpuIndex(gpuIndex);
}

// ★ initModels：依次加载 DbNet（det）、AngleNet（cls）、CrnnNet（rec + 字表），
//   固定返回 true——模型缺失/损坏时 Ort::Session 构造会直接抛异常，
//   由上层（RapidOCR.cpp 的 try/catch）兜底
bool OcrLiteImpl::initModels(const std::string &detPath, const std::string &clsPath,
                         const std::string &recPath, const std::string &keysPath) {
    Logger("=====Init Models=====\n");
    Logger("--- Init DbNet ---\n");
    dbNet.initModel(detPath);

    Logger("--- Init AngleNet ---\n");
    angleNet.initModel(clsPath);

    Logger("--- Init CrnnNet ---\n");
    crnnNet.initModel(recPath, keysPath);

    Logger("Init Models Success!\n");
    return true;
}

// ★ Logger：printf 风格格式化日志——按开关输出到控制台和/或结果 txt；
//   未开任何开关时直接返回，零开销
void OcrLiteImpl::Logger(const char *format, ...) {
    if (!(isOutputConsole || isOutputResultTxt)) return;
    memset(loggerBuffer, 0, 8192);
    va_list args;
    va_start(args, format);
    vsprintf(loggerBuffer, format, args);
    va_end(args);
    if (isOutputConsole) printf("%s", loggerBuffer);
    if (isOutputResultTxt) fprintf(resultTxt, "%s", loggerBuffer);
}

// ───────────────────────── 多入口归一 ─────────────────────────

// ★ makePadding：四周加 padding 像素的白边（BORDER_ISOLATED 隔离边界 +
//   纯白填充）——防止贴边文字在 DB 检测中被裁断，也让 unClip 扩边后有像素可取
cv::Mat makePadding(cv::Mat &src, const int padding) {
    if (padding <= 0) return src;
    cv::Scalar paddingScalar = {255, 255, 255};
    cv::Mat paddingSrc;
    cv::copyMakeBorder(src, paddingSrc, padding, padding, padding, padding, cv::BORDER_ISOLATED, paddingScalar);
    return paddingSrc;
}

// ★ detect（文件版入口）：从 <path><imgName> 读 BGR 图，按 maxSideLen 与
//   原图长边取小者作为目标尺寸（padding 计入），加白边、算缩放参数后
//   转入核心 detect
OcrResult OcrLiteImpl::detect(const char *path, const char *imgName,
                          const int padding, const int maxSideLen,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle) {
    std::string imgFile = getSrcImgFilePath(path, imgName);
    cv::Mat originSrc = imread(imgFile, cv::IMREAD_COLOR);//default : BGR
    int originMaxSide = (std::max)(originSrc.cols, originSrc.rows);
    int resize;
    if (maxSideLen <= 0 || maxSideLen > originMaxSide) {
        resize = originMaxSide;
    } else {
        resize = maxSideLen;
    }
    resize += 2 * padding;
    cv::Rect paddingRect(padding, padding, originSrc.cols, originSrc.rows);
    cv::Mat paddingSrc = makePadding(originSrc, padding);
    ScaleParam scale = getScaleParam(paddingSrc, resize);
    OcrResult result;
    result = detect(path, imgName, paddingSrc, paddingRect, scale,
                    boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
    return result;
}


// ★ detectImageBytes：图片编码字节流入口——imdecode 解码（grey=1 灰度）后
//   转 Mat 版 detect；Android 原版 JNI 使用，HmFish 不走此路径
OcrResult OcrLiteImpl::detectImageBytes(const uint8_t *data, const long dataLength, const int grey,
                                    const int padding, const int maxSideLen,
                                    float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle,
                                    bool mostAngle) {
    std::vector<uint8_t> vecData(data, data + dataLength);
    cv::Mat originSrc = cv::imdecode(vecData, grey == 1 ? cv::IMREAD_GRAYSCALE : cv::IMREAD_COLOR);//default : BGR
    OcrResult result;
    result = detect(originSrc, padding, maxSideLen,
                    boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
    return result;

}

// ★ detectBitmap：位图内存入口——按通道数把 RGBA/RGB 统一转 BGR 后走
//   Mat 版 detect；同为 Android 原版遗留接口
OcrResult OcrLiteImpl::detectBitmap(uint8_t *bitmapData, int width, int height, int channels, int padding,
                                int maxSideLen, float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle,
                                bool mostAngle) {
    cv::Mat originSrc(height, width, CV_8UC(channels), bitmapData);
    if (channels > 3) {
        cv::cvtColor(originSrc, originSrc, cv::COLOR_RGBA2BGR);
    } else if (channels == 3) {
        cv::cvtColor(originSrc, originSrc, cv::COLOR_RGB2BGR);
    }
    OcrResult result;
    result = detect(originSrc, padding, maxSideLen,
                    boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
    return result;
}


// ★ detect（Mat 版入口，HmFish 主入口）：内存图直接识别——与文件版逻辑
//   相同（目标长边取 min(maxSideLen, 原图长边)，padding 计入），path/imgName
//   传 NULL 表示无调试落盘目录
OcrResult OcrLiteImpl::detect(const cv::Mat &mat, int padding, int maxSideLen, float boxScoreThresh, float boxThresh,
                          float unClipRatio, bool doAngle, bool mostAngle) {
    cv::Mat originSrc = mat;
    int originMaxSide = (std::max)(originSrc.cols, originSrc.rows);
    int resize;
    if (maxSideLen <= 0 || maxSideLen > originMaxSide) {
        resize = originMaxSide;
    } else {
        resize = maxSideLen;
    }
    resize += 2 * padding;
    cv::Rect paddingRect(padding, padding, originSrc.cols, originSrc.rows);
    cv::Mat paddingSrc = makePadding(originSrc, padding);
    ScaleParam scale = getScaleParam(paddingSrc, resize);
    OcrResult result;
    result = detect(NULL, NULL, paddingSrc, paddingRect, scale,
                    boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
    return result;
}

// ───────────────────────── 核心流程 ─────────────────────────

// ★ getPartImages：对每个检测框调 getRotateCropImage 透视校正出"摆正"的
//   行小图集合，作为 cls/rec 两阶段的输入；isOutputPartImg 开启时逐张落盘
//   便于排查"裁歪/裁小"问题
std::vector<cv::Mat> OcrLiteImpl::getPartImages(cv::Mat &src, std::vector<TextBox> &textBoxes,
                                            const char *path, const char *imgName) {
    std::vector<cv::Mat> partImages;
    for (size_t i = 0; i < textBoxes.size(); ++i) {
        cv::Mat partImg = getRotateCropImage(src, textBoxes[i].boxPoint);
        partImages.emplace_back(partImg);
        //OutPut DebugImg
        if (isOutputPartImg) {
            std::string debugImgFile = getDebugImgFilePath(path, imgName, i, "-part-");
            saveImg(partImg, debugImgFile.c_str());
        }
    }
    return partImages;
}

// ★ detect（核心私有重载）：三网编排主流程，分六步（见文件头）。
//   参数：src=已加白边的图；originRect=原图在 padding 图中的位置（坐标回扣
//   与结果图裁剪的基准）；scale=缩放参数；其余为 DB 阈值与开关。
//   坐标系说明：textBoxes 的坐标在"src 坐标系"（含 padding、未缩放的原图
//   尺寸），最终 TextBlock/OcrResult 输出时统一减去 padding 回到原图坐标
OcrResult OcrLiteImpl::detect(const char *path, const char *imgName,
                          cv::Mat &src, cv::Rect &originRect, ScaleParam &scale,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle) {

    cv::Mat textBoxPaddingImg = src.clone();
    int thickness = getThickness(src);

    Logger("=====Start detect=====\n");
    Logger("ScaleParam(sw:%d,sh:%d,dw:%d,dh:%d,%f,%f)\n", scale.srcWidth, scale.srcHeight,
           scale.dstWidth, scale.dstHeight,
           scale.ratioWidth, scale.ratioHeight);

    // ---------- 第①步：DbNet 文本检测 ----------
    // 检测框含 DB 后处理（二值化/轮廓/unClip 扩边），耗时计入 dbNetTime
    Logger("---------- step: dbNet getTextBoxes ----------\n");
    double startTime = getCurrentTime();
    std::vector<TextBox> textBoxes = dbNet.getTextBoxes(src, scale, boxScoreThresh, boxThresh, unClipRatio);
    double endDbNetTime = getCurrentTime();
    double dbNetTime = endDbNetTime - startTime;
    Logger("dbNetTime(%fms)\n", dbNetTime);

    for (size_t i = 0; i < textBoxes.size(); ++i) {
        Logger("TextBox[%d](+padding)[score(%f),[x: %d, y: %d], [x: %d, y: %d], [x: %d, y: %d], [x: %d, y: %d]]\n", i,
               textBoxes[i].score,
               textBoxes[i].boxPoint[0].x, textBoxes[i].boxPoint[0].y,
               textBoxes[i].boxPoint[1].x, textBoxes[i].boxPoint[1].y,
               textBoxes[i].boxPoint[2].x, textBoxes[i].boxPoint[2].y,
               textBoxes[i].boxPoint[3].x, textBoxes[i].boxPoint[3].y);
    }

    // ---------- 第②步：结果图绘制 + det 阶段先出框（HmFish 定制） ----------
    Logger("---------- step: drawTextBoxes ----------\n");
    drawTextBoxes(textBoxPaddingImg, textBoxes, thickness);

    // 两阶段懒加载：det 完成即回调检测框（扣除 padding，转回原始坐标），
    // 供调用方先绘制灰色虚线框，rec 期间用户即可看到"框已出现，文字识别中"
    // ★ detStageCallback 触发段：把 src 坐标系的四点框逐点减去 padding
    //   （originRect.x == padding）转回原始图坐标后同步回调；回调抛出的
    //   任何异常都被吞掉，保证不干扰识别主流程
    if (detStageCallback) {
        const int pad = originRect.x;
        std::vector<std::vector<cv::Point>> detBoxes;
        detBoxes.reserve(textBoxes.size());
        for (size_t i = 0; i < textBoxes.size(); ++i) {
            const auto &bp = textBoxes[i].boxPoint;
            if (bp.size() < 4) continue;
            std::vector<cv::Point> quad(4);
            quad[0] = cv::Point(bp[0].x - pad, bp[0].y - pad);
            quad[1] = cv::Point(bp[1].x - pad, bp[1].y - pad);
            quad[2] = cv::Point(bp[2].x - pad, bp[2].y - pad);
            quad[3] = cv::Point(bp[3].x - pad, bp[3].y - pad);
            detBoxes.emplace_back(std::move(quad));
        }
        try {
            detStageCallback(detBoxes);
        } catch (...) {
            // 回调异常不影响识别主流程
        }
    }

    //---------- 第③步：透视校正裁小图 ----------
    // 每个检测框 → 一张摆正的行小图，cls/rec 阶段的输入
    //---------- getPartImages ----------
    std::vector<cv::Mat> partImages = getPartImages(src, textBoxes, path, imgName);

    // ---------- 第④步：AngleNet 方向分类（doAngle=false 时整网跳过） ----------
    // 返回的 angles[i].index：0 正向 / 1 倒置 / -1 未分类（跳过时）
    Logger("---------- step: angleNet getAngles ----------\n");
    std::vector<Angle> angles;
    angles = angleNet.getAngles(partImages, path, imgName, doAngle, mostAngle);

    //Log Angles
    for (size_t i = 0; i < angles.size(); ++i) {
        Logger("angle[%d][index(%d), score(%f), time(%fms)]\n", i, angles[i].index, angles[i].score, angles[i].time);
    }

    //Rotate partImgs
    // 判为倒置（index==1）的行小图旋转 180 度后再送识别，
    // 避免倒置行识别出乱码
    for (size_t i = 0; i < partImages.size(); ++i) {
        if (angles[i].index == 1) {
            partImages.at(i) = matRotateClockWise180(partImages[i]);
        }
    }

    // ---------- 第⑤步：CrnnNet 逐行识别（CTC 解码） ----------
    Logger("---------- step: crnnNet getTextLine ----------\n");
    std::vector<TextLine> textLines = crnnNet.getTextLines(partImages, path, imgName);
    //Log TextLines
    for (size_t i = 0; i < textLines.size(); ++i) {
        Logger("textLine[%d](%s)\n", i, textLines[i].text.c_str());
        std::ostringstream txtScores;
        for (size_t s = 0; s < textLines[i].charScores.size(); ++s) {
            if (s == 0) {
                txtScores << textLines[i].charScores[s];
            } else {
                txtScores << " ," << textLines[i].charScores[s];
            }
        }
        Logger("textScores[%d]{%s}\n", i, std::string(txtScores.str()).c_str());
        Logger("crnnTime[%d](%fms)\n", i, textLines[i].time);
    }

    // ---------- 第⑥步：聚合 TextBlock（坐标扣除 padding 回原图坐标系） ----------
    // 注意 HmFish 调用链的坐标约定：此处"原图坐标"指含 padding 的输入 Mat
    // 扣白边后的坐标；RapidOCR.cpp 会再乘缩放因子还原到显示帧坐标
    std::vector<TextBlock> textBlocks;
    for (size_t i = 0; i < textLines.size(); ++i) {
        std::vector<cv::Point> boxPoint = std::vector<cv::Point>(4);
        int padding = originRect.x;//padding conversion
        boxPoint[0] = cv::Point(textBoxes[i].boxPoint[0].x - padding, textBoxes[i].boxPoint[0].y - padding);
        boxPoint[1] = cv::Point(textBoxes[i].boxPoint[1].x - padding, textBoxes[i].boxPoint[1].y - padding);
        boxPoint[2] = cv::Point(textBoxes[i].boxPoint[2].x - padding, textBoxes[i].boxPoint[2].y - padding);
        boxPoint[3] = cv::Point(textBoxes[i].boxPoint[3].x - padding, textBoxes[i].boxPoint[3].y - padding);
        TextBlock textBlock{boxPoint, textBoxes[i].score, angles[i].index, angles[i].score,
                            angles[i].time, textLines[i].text, textLines[i].charScores, textLines[i].time,
                            angles[i].time + textLines[i].time};
        textBlocks.emplace_back(textBlock);
    }

    double endTime = getCurrentTime();
    double fullTime = endTime - startTime;
    Logger("=====End detect=====\n");
    Logger("FullDetectTime(%fms)\n", fullTime);

    //cropped to original size
    // 结果图裁掉 padding 白边：paddingRect 即原图在 padding 图中的区域；
    // padding=0 时 originRect.x=0，直接整图返回
    cv::Mat textBoxImg;

    if (originRect.x > 0 && originRect.y > 0) {
        textBoxPaddingImg(originRect).copyTo(textBoxImg);
    } else {
        textBoxImg = textBoxPaddingImg;
    }

    //Save result.jpg
    if (isOutputResultImg) {
        std::string resultImgFile = getResultImgFilePath(path, imgName);
        imwrite(resultImgFile, textBoxImg);
    }

    // strRes：全部文本块按行拼接（每块后补 \n），方便调用方整体取文本
    std::string strRes;
    for (auto &textBlock: textBlocks) {
        strRes.append(textBlock.text);
        strRes.append("\n");
    }

    return OcrResult{dbNetTime, textBlocks, textBoxImg, fullTime, strRes};
}
