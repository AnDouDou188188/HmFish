// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ DbNet.cpp —— 文本检测网实现（det 阶段，DB 算法 + DB 后处理）                  ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】getTextBoxes 主流程：resize 到 32 对齐尺寸 → ImageNet 归一化          ║
// ║   前处理（HWC→CHW）→ ONNX 推理得概率图 → 概率图转灰度 → 阈值二值化 →          ║
// ║   3x3 膨胀 → findRsBoxes 后处理（找轮廓→最小外接矩形→小框/低分过滤→           ║
// ║   unClip 扩边→坐标还原回原图）→ 输出 TextBox 列表。                           ║
// ║ 【流水线位置】三网之首，OcrLiteImpl 核心流程第①步；                           ║
// ║   产出的四点框直接决定后续裁图与识别质量。                                    ║
// ║ 【输入输出】输入 NCHW float32（宽高为 scale.dst*）；输出 1x1xHxW              ║
// ║   概率图，H/W 与输入相同（DB 网络是对称 FPN 结构）。                          ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "DbNet.h"
#include "OcrUtils.h"

// ★ setGpuIndex：__CUDA__ 构建时挂 CUDA 执行提供者（2GB 显存上限、
//   cuDNN 默认卷积搜索）；HmFish 为 CPU 构建，整函数体被宏裁掉
void DbNet::setGpuIndex(int gpuIndex) {
#ifdef __CUDA__
    if (gpuIndex >= 0) {
        OrtCUDAProviderOptions cuda_options;
        cuda_options.device_id = gpuIndex;
        cuda_options.arena_extend_strategy = 0;
        cuda_options.gpu_mem_limit = 2ULL * 1024 * 1024 * 1024;
        cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cuda_options.do_copy_in_default_stream = 1;

        sessionOptions.AppendExecutionProvider_CUDA(cuda_options);
        printf("det try to use GPU%d\n", gpuIndex);
    }
    else {
        printf("det use CPU\n");
    }
#endif
}

// ★ 析构：释放 ONNX 会话并清空输入/输出名
DbNet::~DbNet() {
    delete session;
    inputNamesPtr.clear();
    outputNamesPtr.clear();
}

// ★ setNumThread：设置 ORT 图内（IntraOp，算子内部并行）与图间
//   （InterOp，节点级并行）线程数，并开到最高图优化等级
//   （算子融合/冗余节点消除等，对 CNN 提速明显）；须在 initModel 前调
void DbNet::setNumThread(int numOfThread) {
    numThread = numOfThread;
    //===session options===
    // Sets the number of threads used to parallelize the execution within nodes
    // A value of 0 means ORT will pick a default
    //sessionOptions.SetIntraOpNumThreads(numThread);
    //set OMP_NUM_THREADS=16

    // Sets the number of threads used to parallelize the execution of the graph (across nodes)
    // If sequential execution is enabled this value is ignored
    // A value of 0 means ORT will pick a default
    sessionOptions.SetInterOpNumThreads(numThread);
    sessionOptions.SetIntraOpNumThreads(numThread);

    // Sets graph optimization level
    // ORT_DISABLE_ALL -> To disable all optimizations
    // ORT_ENABLE_BASIC -> To enable basic optimizations (Such as redundant node removals)
    // ORT_ENABLE_EXTENDED -> To enable extended optimizations (Includes level 1 + more complex optimizations like node fusions)
    // ORT_ENABLE_ALL -> To Enable All possible opitmizations
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
}

// ★ initModel：按平台构建 Ort::Session（Windows 需 wchar 路径），
//   随后提取模型输入/输出节点名缓存备用
void DbNet::initModel(const std::string &pathStr) {
#ifdef _WIN32
    std::wstring detPath = strToWstr(pathStr);
    session = new Ort::Session(env, detPath.c_str(), sessionOptions);
#else
    session = new Ort::Session(env, pathStr.c_str(), sessionOptions);
#endif
    inputNamesPtr = getInputNames(session);
    outputNamesPtr = getOutputNames(session);
}

// ───────────────────────── DB 后处理：轮廓 → 文本框 ─────────────────────────

// ★ findRsBoxes：DB 后处理核心——从二值/膨胀图还原最终文本框。逐轮廓流程：
//   ① findContours 提取全部闭合轮廓（RETR_LIST 全层级、CHAIN_APPROX_SIMPLE
//      压缩冗余点），最多处理 1000 个候选防极端耗时；
//   ② 跳过退化轮廓（点数 ≤2），minAreaRect 取最小外接旋转矩形；
//   ③ getMinBoxes 规范四点序，长边 <3px 的噪声框直接丢弃；
//   ④ boxScoreFast 计算框内概率图均分，低于 boxScoreThresh 丢弃——
//      "二值化捡回来的框"必须再经概率图复核，双阈值抑制误检；
//   ⑤ unClip 按 unClipRatio 扩边（补偿二值化收缩）；扩边后退化
//      （宽高均 <1.001）或仍过小（长边 <5px）的丢弃；
//   ⑥ 扩边后坐标除以缩放比（ratioWidth/ratioHeight）还原回原图坐标系，
//      并逐点钳入图内防越界；
//   最后 reverse——findContours 由外向内返回（大框在前），反转后
//   近似"从上到下"的阅读序
std::vector<TextBox> findRsBoxes(const cv::Mat &predMat, const cv::Mat &dilateMat, ScaleParam &s,
                                 const float boxScoreThresh, const float unClipRatio) {
    const int longSideThresh = 3;//minBox 长边门限
    const int maxCandidates = 1000;

    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;

    cv::findContours(dilateMat, contours, hierarchy, cv::RETR_LIST,
                     cv::CHAIN_APPROX_SIMPLE);

    size_t numContours = contours.size() >= maxCandidates ? maxCandidates : contours.size();

    std::vector<TextBox> rsBoxes;

    for (size_t i = 0; i < numContours; i++) {
        if (contours[i].size() <= 2) {
            continue;
        }
        cv::RotatedRect minAreaRect = cv::minAreaRect(contours[i]);

        float longSide;
        std::vector<cv::Point2f> minBoxes = getMinBoxes(minAreaRect, longSide);

        if (longSide < longSideThresh) {
            continue;
        }

        float boxScore = boxScoreFast(minBoxes, predMat);
        if (boxScore < boxScoreThresh)
            continue;

        //-----unClip-----
        cv::RotatedRect clipRect = unClip(minBoxes, unClipRatio);
        if (clipRect.size.height < 1.001 && clipRect.size.width < 1.001) {
            continue;
        }
        //-----unClip-----

        std::vector<cv::Point2f> clipMinBoxes = getMinBoxes(clipRect, longSide);
        if (longSide < longSideThresh + 2)
            continue;

        std::vector<cv::Point> intClipMinBoxes;

        for (auto &clipMinBox: clipMinBoxes) {
            float x = clipMinBox.x / s.ratioWidth;
            float y = clipMinBox.y / s.ratioHeight;
            int ptX = (std::min)((std::max)(int(x), 0), s.srcWidth - 1);
            int ptY = (std::min)((std::max)(int(y), 0), s.srcHeight - 1);
            cv::Point point{ptX, ptY};
            intClipMinBoxes.push_back(point);
        }
        rsBoxes.push_back(TextBox{intClipMinBoxes, boxScore});
    }
    reverse(rsBoxes.begin(), rsBoxes.end());
    return rsBoxes;
}

// ★ getTextBoxes：检测主入口——前处理、推理、概率图后处理一条龙。
//   步骤：① 按 scale resize（32 对齐）；② substractMeanNormalize
//   （ImageNet mean/std，HWC→CHW）；③ 建 NCHW 张量并 Run 推理；
//   ④ 概率图数据拷出（float 原值 predMat + 0-255 灰度 cBufMat 双视图）；
//   ⑤ 阈值二值化（threshold = boxThresh*255，DB 论文中的"可微分二值化"
//      在推理阶段退化为普通阈值化）；⑥ 2x2 矩形核膨胀（把断裂笔画连起来，
//      提升轮廓完整性）；⑦ findRsBoxes 出框
std::vector<TextBox>
DbNet::getTextBoxes(cv::Mat &src, ScaleParam &s, float boxScoreThresh, float boxThresh, float unClipRatio) {
    cv::Mat srcResize;
    resize(src, srcResize, cv::Size(s.dstWidth, s.dstHeight));
    std::vector<float> inputTensorValues = substractMeanNormalize(srcResize, meanValues, normValues);
    std::array<int64_t, 4> inputShape{1, srcResize.channels(), srcResize.rows, srcResize.cols};
    auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(memoryInfo, inputTensorValues.data(),
                                                             inputTensorValues.size(), inputShape.data(),
                                                             inputShape.size());
    assert(inputTensor.IsTensor());
    std::vector<const char *> inputNames = {inputNamesPtr.data()->get()};
    std::vector<const char *> outputNames = {outputNamesPtr.data()->get()};
    auto outputTensor = session->Run(Ort::RunOptions{nullptr}, inputNames.data(), &inputTensor,
                                     inputNames.size(), outputNames.data(), outputNames.size());
    assert(outputTensor.size() == 1 && outputTensor.front().IsTensor());
    std::vector<int64_t> outputShape = outputTensor[0].GetTensorTypeAndShapeInfo().GetShape();
    int64_t outputCount = std::accumulate(outputShape.begin(), outputShape.end(), 1,
                                          std::multiplies<int64_t>());
    float *floatArray = outputTensor.front().GetTensorMutableData<float>();
    std::vector<float> outputData(floatArray, floatArray + outputCount);

    //-----Data preparation-----
    // 概率图双视图：predMat 保留 float 原值（boxScoreFast 复核用），
    // cBufMat 转成 0-255 灰度（二值化输入用）
    int outHeight = (int) outputShape[2];
    int outWidth = (int) outputShape[3];
    size_t area = outHeight * outWidth;

    std::vector<float> predData(area, 0.0);
    std::vector<unsigned char> cbufData(area, ' ');

    for (int i = 0; i < area; i++) {
        predData[i] = float(outputData[i]);
        cbufData[i] = (unsigned char) ((outputData[i]) * 255);
    }

    cv::Mat predMat(outHeight, outWidth, CV_32F, (float *) predData.data());
    cv::Mat cBufMat(outHeight, outWidth, CV_8UC1, (unsigned char *) cbufData.data());

    //-----boxThresh-----
    // 概率图按 boxThresh*255 阈值二值化：>阈值 前景(255) / 否则背景(0)
    const double maxValue = 255;
    const double threshold = boxThresh * 255;
    cv::Mat thresholdMat;
    cv::threshold(cBufMat, thresholdMat, threshold, maxValue, cv::THRESH_BINARY);

    //-----dilate-----
    // 2x2 矩形核膨胀一次：弥合概率图的细小断裂，让相邻笔画连通成完整轮廓
    cv::Mat dilateMat;
    cv::Mat dilateElement = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::dilate(thresholdMat, dilateMat, dilateElement);

    return findRsBoxes(predMat, dilateMat, s, boxScoreThresh, unClipRatio);
}