// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrUtils.cpp —— ocrlite 通用工具函数实现                                      ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】声明于 OcrUtils.h 的全部工具实现，按功能分五区：                      ║
// ║   ① 缩放参数（getScaleParam x2）——决定网络输入尺寸与坐标还原系数；            ║
// ║   ② DB 后处理几何（getMinBoxes/boxScoreFast/getContourArea/unClip）           ║
// ║     ——把 DbNet 概率图变成可用四点文本框；                                     ║
// ║   ③ 小图获取（getRotateCropImage 透视校正 + adjustTargetImg 白底适配）；      ║
// ║   ④ 旋转与绘制（matRotate*/drawTextBox*）；                                   ║
// ║   ⑤ 张量前处理 / ONNX 节点名 / 调试落盘与路径拼接。                           ║
// ║ 【流水线位置】被三网与 OcrLiteImpl 共用；unClip 依赖同目录 clipper.cpp        ║
// ║   的 Clipper 多边形偏移库。                                                   ║
// ║ 【与 Android 原版差异】逻辑一致；仅头文件差异——OpenCV 5 的                    ║
// ║   minAreaRect/getPerspectiveTransform 迁入 geometry/2d.hpp，                  ║
// ║   故新增对应 include（见下）。                                                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
// OpenCV 5：getPerspectiveTransform/minAreaRect 从 imgproc 迁移至 geometry 模块
#include <opencv2/geometry/2d.hpp>
#include <numeric>
#include "OcrUtils.h"
#include "clipper.hpp"

// ───────────────────────── 时间与基础工具 ─────────────────────────

// ★ getCurrentTime：当前时刻（ms）。用 OpenCV 的 tick 计数而非系统时钟，
//   保证与视频流时间轴同源且高精度；各网络阶段耗时均由前后两次调用相减得到
double getCurrentTime() {
    return (static_cast<double>(cv::getTickCount())) / cv::getTickFrequency() * 1000;//单位毫秒
}

// ★ strToWstr：窄串转宽串——仅 Windows 平台的 Ort::Session 需要 wchar_t* 路径。
//   逐字符赋值的简化转换，只对 ASCII 路径安全；HarmonyOS 构建不会走到此函数
//onnxruntime init windows
std::wstring strToWstr(std::string str) {
    if (str.length() == 0)
        return L"";
    std::wstring wstr;
    wstr.assign(str.begin(), str.end());
    return wstr;
}

// ───────────────────────── 缩放参数计算 ─────────────────────────

// ★ getScaleParam（按倍率版）：给整图乘一个 scale 系数得到目标尺寸。
//   关键点：DbNet 含 5 次 stride-2 下采样，输入宽高必须对齐 32 的倍数，
//   因此非整倍数时向下回退一档（(n/32-1)*32）再钳到最小 32，宁可略小也不越界
ScaleParam getScaleParam(cv::Mat &src, const float scale) {
    int srcWidth = src.cols;
    int srcHeight = src.rows;
    int dstWidth = int((float) srcWidth * scale);
    int dstHeight = int((float) srcHeight * scale);
    if (dstWidth % 32 != 0) {
        dstWidth = (dstWidth / 32 - 1) * 32;
        dstWidth = (std::max)(dstWidth, 32);
    }
    if (dstHeight % 32 != 0) {
        dstHeight = (dstHeight / 32 - 1) * 32;
        dstHeight = (std::max)(dstHeight, 32);
    }
    float scaleWidth = (float) dstWidth / (float) srcWidth;
    float scaleHeight = (float) dstHeight / (float) srcHeight;
    return {srcWidth, srcHeight, dstWidth, dstHeight, scaleWidth, scaleHeight};
}

// ★ getScaleParam（按目标长边版）：等比缩放使"长边 ≤ targetSize"。
//   与按倍率版的差异：取整方向相反——先算等比 ratio，非 32 倍数时直接向下取
//   整到 32 倍数（dstWidth/32*32）而非再减 32，缩得更充分，适合限制输入上限
//   （HmFish 的 maxSideLen=1024 即走此路径）
ScaleParam getScaleParam(cv::Mat &src, const int targetSize) {
    int srcWidth, srcHeight, dstWidth, dstHeight;
    srcWidth = dstWidth = src.cols;
    srcHeight = dstHeight = src.rows;

    float ratio = 1.f;
    if (srcWidth > srcHeight) {
        ratio = float(targetSize) / float(srcWidth);
    } else {
        ratio = float(targetSize) / float(srcHeight);
    }
    dstWidth = int(float(srcWidth) * ratio);
    dstHeight = int(float(srcHeight) * ratio);
    if (dstWidth % 32 != 0) {
        dstWidth = (dstWidth / 32) * 32;
        dstWidth = (std::max)(dstWidth, 32);
    }
    if (dstHeight % 32 != 0) {
        dstHeight = (dstHeight / 32) * 32;
        dstHeight = (std::max)(dstHeight, 32);
    }
    float ratioWidth = (float) dstWidth / (float) srcWidth;
    float ratioHeight = (float) dstHeight / (float) srcHeight;
    return {srcWidth, srcHeight, dstWidth, dstHeight, ratioWidth, ratioHeight};
}

// ───────────────────────── 检测框几何 ─────────────────────────

// ★ getBox：取旋转矩形四顶点。cv::RotatedRect::points 输出顺序为绕矩形一周
//   （bottom-left 起逆时针），此函数保持该顺序仅转为 vector 形式
std::vector<cv::Point2f> getBox(const cv::RotatedRect &rect) {
    cv::Point2f vertices[4];
    rect.points(vertices);
    //std::vector<cv::Point2f> ret(4);
    std::vector<cv::Point2f> ret2(vertices, vertices + sizeof(vertices) / sizeof(vertices[0]));
    //memcpy(vertices, &ret[0], ret.size() * sizeof(ret[0]));
    return ret2;
}

// ★ getThickness：结果图上画框线宽——按图短边 /1000 自适应，最小 2px；
//   小图线细防遮字，大图线粗保可见
int getThickness(cv::Mat &boxImg) {
    int minSize = boxImg.cols > boxImg.rows ? boxImg.rows : boxImg.cols;
    int thickness = minSize / 1000 + 2;
    return thickness;
}

// ★ drawTextBox（旋转矩形版）：按四顶点顺次连线画红色框（调试结果图用）
void drawTextBox(cv::Mat &boxImg, cv::RotatedRect &rect, int thickness) {
    cv::Point2f vertices[4];
    rect.points(vertices);
    for (int i = 0; i < 4; i++)
        cv::line(boxImg, vertices[i], vertices[(i + 1) % 4], cv::Scalar(0, 0, 255), thickness);
    //cv::polylines(srcmat, textpoint, true, cv::Scalar(0, 255, 0), 2);
}

// ★ drawTextBox（四点框版）：对已还原成整型四点的文本框连线画红框；
//   box 顺序须为绕框一周（getMinBoxes 保证：左上/右上/右下/左下）
void drawTextBox(cv::Mat &boxImg, const std::vector<cv::Point> &box, int thickness) {
    auto color = cv::Scalar(0, 0, 255);// B(0) G(0) R(255)
    cv::line(boxImg, box[0], box[1], color, thickness);
    cv::line(boxImg, box[1], box[2], color, thickness);
    cv::line(boxImg, box[2], box[3], color, thickness);
    cv::line(boxImg, box[3], box[0], color, thickness);
}

// ★ drawTextBoxes：批量画框（OcrLiteImpl 核心流程的"drawTextBoxes"步骤）
void drawTextBoxes(cv::Mat &boxImg, std::vector<TextBox> &textBoxes, int thickness) {
    for (auto & textBox : textBoxes) {
        drawTextBox(boxImg, textBox.boxPoint, thickness);
    }
}

// ───────────────────────── 图像旋转与透视校正 ─────────────────────────

// ★ matRotateClockWise180：顺时针旋转 180 度——先垂直翻转再水平翻转，
//   两次原位 flip 避免转置时的内存重排，比 warpAffine 更快；
//   OcrLiteImpl 用它把 AngleNet 判为倒置（index==1）的行小图转正
cv::Mat matRotateClockWise180(cv::Mat src) {
    flip(src, src, 0);
    flip(src, src, 1);
    return src;
}

// ★ matRotateClockWise90：顺时针旋转 90 度——转置后水平翻转；
//   供竖排文本等场景使用（当前流水线主路径未调用，保留工具性质）
cv::Mat matRotateClockWise90(cv::Mat src) {
    transpose(src, src);
    flip(src, src, 1);
    return src;
}

// ★ getRotateCropImage：从原图裁出"摆正"的行小图——det 与 cls/rec 之间的
//   关键桥梁。步骤（对应代码段）：
//   ① 拷贝原图，取四点框的外接正矩形并裁出 imgCrop（减少透视变换的计算量）；
//   ② 四点坐标平移到 imgCrop 局部坐标系（减去外接矩形左上角）；
//   ③ 以框的"上边长度"为宽、"左边长度"为高（欧氏距离）定义目标矩形 ptsDst，
//      与源四点 ptsSrc 求透视变换矩阵 M——倾斜/透视变形的文本被拉正；
//   ④ warpPerspective 时用 BORDER_REPLICATE 让越界边缘复制邻近像素，
//      避免黑边污染识别；
//   ⑤ 若结果"高 ≥ 宽x1.5"判定为竖排文本：转置+垂直翻转转成横排再返回
cv::Mat getRotateCropImage(const cv::Mat &src, std::vector<cv::Point> box) {
    cv::Mat image;
    src.copyTo(image);
    std::vector<cv::Point> points = box;

    int collectX[4] = {box[0].x, box[1].x, box[2].x, box[3].x};
    int collectY[4] = {box[0].y, box[1].y, box[2].y, box[3].y};
    int left = int(*std::min_element(collectX, collectX + 4));
    int right = int(*std::max_element(collectX, collectX + 4));
    int top = int(*std::min_element(collectY, collectY + 4));
    int bottom = int(*std::max_element(collectY, collectY + 4));

    cv::Mat imgCrop;
    image(cv::Rect(left, top, right - left, bottom - top)).copyTo(imgCrop);

    for (auto &point: points) {
        point.x -= left;
        point.y -= top;
    }

    int imgCropWidth = int(sqrt(pow(points[0].x - points[1].x, 2) +
                                pow(points[0].y - points[1].y, 2)));
    int imgCropHeight = int(sqrt(pow(points[0].x - points[3].x, 2) +
                                 pow(points[0].y - points[3].y, 2)));

    cv::Point2f ptsDst[4];
    ptsDst[0] = cv::Point2f(0., 0.);
    ptsDst[1] = cv::Point2f(imgCropWidth, 0.);
    ptsDst[2] = cv::Point2f(imgCropWidth, imgCropHeight);
    ptsDst[3] = cv::Point2f(0.f, imgCropHeight);

    cv::Point2f ptsSrc[4];
    ptsSrc[0] = cv::Point2f(points[0].x, points[0].y);
    ptsSrc[1] = cv::Point2f(points[1].x, points[1].y);
    ptsSrc[2] = cv::Point2f(points[2].x, points[2].y);
    ptsSrc[3] = cv::Point2f(points[3].x, points[3].y);

    cv::Mat M = cv::getPerspectiveTransform(ptsSrc, ptsDst);

    cv::Mat partImg;
    cv::warpPerspective(imgCrop, partImg, M,
                        cv::Size(imgCropWidth, imgCropHeight),
                        cv::BORDER_REPLICATE);

    if (float(partImg.rows) >= float(partImg.cols) * 1.5) {
        cv::Mat srcCopy = cv::Mat(partImg.rows, partImg.cols, partImg.depth());
        cv::transpose(partImg, srcCopy);
        cv::flip(srcCopy, srcCopy, 0);
        return srcCopy;
    } else {
        return partImg;
    }
}

// ★ adjustTargetImg：定尺寸适配——先把图等比缩放到目标高度（宽随比例），
//   再贴入目标宽高的白底图：窄于目标则靠左贴、右侧补白；宽于目标则居中
//   裁掉两侧。供需要固定输入尺寸的轻量网络使用（如表格/分类场景）
cv::Mat adjustTargetImg(cv::Mat &src, int dstWidth, int dstHeight) {
    cv::Mat srcResize;
    float scale = (float) dstHeight / (float) src.rows;
    int angleWidth = int((float) src.cols * scale);
    cv::resize(src, srcResize, cv::Size(angleWidth, dstHeight));
    cv::Mat srcFit = cv::Mat(dstHeight, dstWidth, CV_8UC3, cv::Scalar(255, 255, 255));
    if (angleWidth < dstWidth) {
        cv::Rect rect(0, 0, srcResize.cols, srcResize.rows);
        srcResize.copyTo(srcFit(rect));
    } else {
        cv::Rect rect(0, 0, dstWidth, dstHeight);
        srcResize(rect).copyTo(srcFit);
    }
    return srcFit;
}

// ───────────────────────── DB 后处理几何 ─────────────────────────

// ★ cvPointCompare：按 x 升序比较两点——getMinBoxes 排序用
bool cvPointCompare(const cv::Point &a, const cv::Point &b) {
    return a.x < b.x;
}

// ★ getMinBoxes：把旋转矩形的四顶点重排为"左上/右上/右下/左下"规范序。
//   算法：先按 x 升序排序，前两点中 y 小者为左上（index1）、大者为左下
//   （index4）；后两点中 y 小者为右上（index2）、大者为右下（index3）。
//   同时经 maxSideLen 带出矩形长边，供调用方过滤尺寸过小的候选框
std::vector<cv::Point2f> getMinBoxes(const cv::RotatedRect &boxRect, float &maxSideLen) {
    maxSideLen = std::max(boxRect.size.width, boxRect.size.height);
    std::vector<cv::Point2f> boxPoint = getBox(boxRect);
    std::sort(boxPoint.begin(), boxPoint.end(), cvPointCompare);
    int index1, index2, index3, index4;
    if (boxPoint[1].y > boxPoint[0].y) {
        index1 = 0;
        index4 = 1;
    } else {
        index1 = 1;
        index4 = 0;
    }
    if (boxPoint[3].y > boxPoint[2].y) {
        index2 = 2;
        index3 = 3;
    } else {
        index2 = 3;
        index3 = 2;
    }
    std::vector<cv::Point2f> minBox(4);
    minBox[0] = boxPoint[index1];
    minBox[1] = boxPoint[index2];
    minBox[2] = boxPoint[index3];
    minBox[3] = boxPoint[index4];
    return minBox;
}

// ★ boxScoreFast：计算四点框内概率图平均得分（DB 后处理的核心过滤指标）。
//   步骤：① 取框外接正矩形（floor/ceil 后钳入图内）；② 在外接矩形上建
//   同尺寸掩码，fillPoly 只把四边形内部置 1；③ 用掩码求概率图均值——
//   只统计框内像素，避免外接矩形里的背景拉低得分
float boxScoreFast(const std::vector<cv::Point2f> &boxes, const cv::Mat &pred) {
    int width = pred.cols;
    int height = pred.rows;

    float arrayX[4] = {boxes[0].x, boxes[1].x, boxes[2].x, boxes[3].x};
    float arrayY[4] = {boxes[0].y, boxes[1].y, boxes[2].y, boxes[3].y};

    int minX = clamp(int(std::floor(*(std::min_element(arrayX, arrayX + 4)))), 0, width - 1);
    int maxX = clamp(int(std::ceil(*(std::max_element(arrayX, arrayX + 4)))), 0, width - 1);
    int minY = clamp(int(std::floor(*(std::min_element(arrayY, arrayY + 4)))), 0, height - 1);
    int maxY = clamp(int(std::ceil(*(std::max_element(arrayY, arrayY + 4)))), 0, height - 1);

    cv::Mat mask = cv::Mat::zeros(maxY - minY + 1, maxX - minX + 1, CV_8UC1);

    cv::Point box[4];
    box[0] = cv::Point(int(boxes[0].x) - minX, int(boxes[0].y) - minY);
    box[1] = cv::Point(int(boxes[1].x) - minX, int(boxes[1].y) - minY);
    box[2] = cv::Point(int(boxes[2].x) - minX, int(boxes[2].y) - minY);
    box[3] = cv::Point(int(boxes[3].x) - minX, int(boxes[3].y) - minY);
    const cv::Point *pts[1] = {box};
    int npts[] = {4};
    cv::fillPoly(mask, pts, npts, 1, cv::Scalar(1));

    cv::Mat croppedImg;
    pred(cv::Rect(minX, minY, maxX - minX + 1, maxY - minY + 1))
            .copyTo(croppedImg);

    auto score = (float) cv::mean(croppedImg, mask)[0];
    return score;
}

// ★ getContourArea：按 DB 论文计算多边形的偏移距离——先用鞋带公式求面积
//   （顶点序叉积累加取绝对值一半），再累加各边长度得周长，返回
//   "面积 x unClipRatio / 周长"。这正是把框外扩 unClipRatio 倍面积时，
//   边界应向外平移的距离（面积增量 ≈ 距离 x 周长）
float getContourArea(const std::vector<cv::Point2f> &box, float unClipRatio) {
    size_t size = box.size();
    float area = 0.0f;
    float dist = 0.0f;
    for (size_t i = 0; i < size; i++) {
        area += box[i].x * box[(i + 1) % size].y -
                box[i].y * box[(i + 1) % size].x;
        dist += sqrtf((box[i].x - box[(i + 1) % size].x) *
                      (box[i].x - box[(i + 1) % size].x) +
                      (box[i].y - box[(i + 1) % size].y) *
                      (box[i].y - box[(i + 1) % size].y));
    }
    area = fabs(float(area / 2.0));

    return area * unClipRatio / dist;
}

// ★ unClip：DB 后处理扩边——把二值化收缩后的文本框外扩回文字真实边界。
//   步骤：① getContourArea 算出偏移距离；② 用 Clipper 的 ClipperOffset
//   做 Vatti 多边形偏移（jtRound 圆角连接、闭合多边形模式）；
//   ③ 把偏移结果的整型点集转回浮点；④ minAreaRect 取最小外接旋转矩形。
//   扩边失败/退化时返回 1x1 的占位矩形，由调用方按尺寸阈值丢弃
cv::RotatedRect unClip(std::vector<cv::Point2f> box, float unClipRatio) {
    float distance = getContourArea(box, unClipRatio);

    ClipperLib::ClipperOffset offset;
    ClipperLib::Path p;
    p << ClipperLib::IntPoint(int(box[0].x), int(box[0].y))
      << ClipperLib::IntPoint(int(box[1].x), int(box[1].y))
      << ClipperLib::IntPoint(int(box[2].x), int(box[2].y))
      << ClipperLib::IntPoint(int(box[3].x), int(box[3].y));
    offset.AddPath(p, ClipperLib::jtRound, ClipperLib::etClosedPolygon);

    ClipperLib::Paths soln;
    offset.Execute(soln, distance);
    std::vector<cv::Point2f> points;

    for (size_t j = 0; j < soln.size(); j++) {
        for (size_t i = 0; i < soln[soln.size() - 1].size(); i++) {
            points.emplace_back(soln[j][i].X, soln[j][i].Y);
        }
    }
    cv::RotatedRect res;
    if (points.empty()) {
        res = cv::RotatedRect(cv::Point2f(0, 0), cv::Size2f(1, 1), 0);
    } else {
        res = cv::minAreaRect(points);
    }
    return res;
}

// ───────────────────────── 张量前处理 ─────────────────────────

// ★ substractMeanNormalize：三网共用的输入张量前处理。
//   ① 遍历 HWC 排列的像素（BGR，uchar）；
//   ② 按通道做 pix*norm - mean*norm——这是 (pix-mean)/std 的向量化等价
//      （DbNet 的 mean/std 展开成乘常数形式，避免每像素两次除法）；
//   ③ 写入时重排为 CHW（NCHW 要求 plane 连续），即 inputTensorValues
//      的布局为 [channel][pixel]。输出向量直接作为 ORT 输入张量的数据源
std::vector<float> substractMeanNormalize(cv::Mat &src, const float *meanVals, const float *normVals) {
    auto inputTensorSize = src.cols * src.rows * src.channels();
    std::vector<float> inputTensorValues(inputTensorSize);
    size_t numChannels = src.channels();
    size_t imageSize = src.cols * src.rows;

    for (size_t pid = 0; pid < imageSize; pid++) {
        for (size_t ch = 0; ch < numChannels; ++ch) {
            float data = (float) (src.data[pid * numChannels + ch] * normVals[ch] - meanVals[ch] * normVals[ch]);
            inputTensorValues[ch * imageSize + pid] = data;
        }
    }
    return inputTensorValues;
}

// ───────────────────────── ONNX 辅助与调试输出 ─────────────────────────

// ★ getAngleIndexes：抽取 Angle 序列的 index 数组——mostAngle 模式下
//   对全部行的方向下标求和做多数投票
std::vector<int> getAngleIndexes(std::vector<Angle> &angles) {
    std::vector<int> angleIndexes;
    angleIndexes.reserve(angles.size());
    for (auto &angle: angles) {
        angleIndexes.push_back(angle.index);
    }
    return angleIndexes;
}

// ★ getInputNames：提取会话全部输入节点名（AllocatedStringPtr 智能持有，
//   防字符串泄漏）；注释保留了原版打印输入形状的调试代码
std::vector<Ort::AllocatedStringPtr> getInputNames(Ort::Session *session) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t numInputNodes = session->GetInputCount();

    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    inputNamesPtr.reserve(numInputNodes);
    std::vector<int64_t> input_node_dims;

    // iterate over all input nodes
    for (size_t i = 0; i < numInputNodes; i++) {
        auto inputName = session->GetInputNameAllocated(i, allocator);
        inputNamesPtr.push_back(std::move(inputName));
        /*printf("inputName[%zu] = %s\n", i, inputName.get());

        // print input node types
        auto typeInfo = session->GetInputTypeInfo(i);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();

        ONNXTensorElementDataType type = tensorInfo.GetElementType();
        printf("inputType[%zu] = %u\n", i, type);

        // print input shapes/dims
        input_node_dims = tensorInfo.GetShape();
        printf("Input num_dims = %zu\n", input_node_dims.size());
        for (size_t j = 0; j < input_node_dims.size(); j++) {
            printf("Input dim[%zu] = %llu\n",j, input_node_dims[j]);
        }*/
    }
    return inputNamesPtr;
}

// ★ getOutputNames：同 getInputNames，提取输出节点名（推理时按名取回张量）
std::vector<Ort::AllocatedStringPtr> getOutputNames(Ort::Session *session) {
    Ort::AllocatorWithDefaultOptions allocator;
    const size_t numOutputNodes = session->GetOutputCount();

    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;
    outputNamesPtr.reserve(numOutputNodes);
    std::vector<int64_t> output_node_dims;

    for (size_t i = 0; i < numOutputNodes; i++) {
        auto outputName = session->GetOutputNameAllocated(i, allocator);
        outputNamesPtr.push_back(std::move(outputName));
        /*printf("outputName[%zu] = %s\n", i, outputName.get());

        // print input node types
        auto type_info = session->GetOutputTypeInfo(i);
        auto tensor_info = type_info.GetTensorTypeAndShapeInfo();

        ONNXTensorElementDataType type = tensor_info.GetElementType();
        printf("outputType[%zu] = %u\n", i, type);

        // print input shapes/dims
        output_node_dims = tensor_info.GetShape();
        printf("output num_dims = %zu\n", output_node_dims.size());
        for (size_t j = 0; j < output_node_dims.size(); j++) {
            printf("output dim[%zu] = %llu\n",j, output_node_dims[j]);
        }*/
    }
    return outputNamesPtr;
}

// ★ saveImg：调试图片落盘（imwrite 薄封装）
void saveImg(cv::Mat &img, const char *imgPath) {
    cv::imwrite(imgPath, img);
}

// ★ getSrcImgFilePath：拼接源图路径 path+imgName（detect 文件版入口读图用）
std::string getSrcImgFilePath(const char *path, const char *imgName) {
    std::string filePath;
    filePath.append(path).append(imgName);
    return filePath;
}

// ★ getResultTxtFilePath：结果 txt 路径（imgName+"-result.txt"，enableResultTxt 用）
std::string getResultTxtFilePath(const char *path, const char *imgName) {
    std::string filePath;
    filePath.append(path).append(imgName).append("-result.txt");
    return filePath;
}

// ★ getResultImgFilePath：结果图路径（imgName+"-result.jpg"）
std::string getResultImgFilePath(const char *path, const char *imgName) {
    std::string filePath;
    filePath.append(path).append(imgName).append("-result.jpg");
    return filePath;
}

// ★ getDebugImgFilePath：中间调试图路径——imgName+tag+序号+".jpg"，
//   配合 isOutputPartImg/isOutputDebugImg/isOutputAngleImg 开关使用
std::string getDebugImgFilePath(const char *path, const char *imgName, size_t i, const char *tag) {
    std::string filePath;
    filePath.append(path).append(imgName).append(tag).append(std::to_string(i)).append(".jpg");
    return filePath;
}