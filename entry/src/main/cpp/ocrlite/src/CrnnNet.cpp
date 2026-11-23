// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ CrnnNet.cpp —— 文本识别网实现（rec 阶段，CRNN + CTC 解码）                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】对 det 阶段裁出的单行小图逐行识别：                                   ║
// ║   getTextLine——等比缩放到高 48（宽随行宽按比例变化，不设上限）                ║
// ║   → mean/norm 127.5 前处理（HWC→CHW）→ CRNN onnx 推理 →                       ║
// ║   scoreToTextLine 的 CTC 解码（blank 合并去重）按 keys 字表映射文字。         ║
// ║ 【流水线位置】三网之末，OcrLiteImpl 核心流程第⑤步；                           ║
// ║   输入是"DbNet 出框 + 透视摆正 + AngleNet 转正"后的行小图。                   ║
// ║ 【输入输出】输入 NCHW float32（1x3x48xW，W 可变）；                           ║
// ║   输出 1xTxC 序列概率（T 个时间步、C 类含 blank），解码后得 TextLine。        ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "CrnnNet.h"
#include "OcrUtils.h"
#include <fstream>
#include <numeric>

#ifdef __DIRECTML__
#include <onnxruntime/core/providers/dml/dml_provider_factory.h>
#endif

// ★ setGpuIndex：__CUDA__/__DIRECTML__ 构建时挂对应 GPU 提供者；
//   HmFish 为 CPU 构建，函数体被宏裁掉为空
void CrnnNet::setGpuIndex(int gpuIndex) {
#ifdef __CUDA__
    if (gpuIndex >= 0) {
        OrtCUDAProviderOptions cuda_options;
        cuda_options.device_id = gpuIndex;
        cuda_options.arena_extend_strategy = 0;
        cuda_options.gpu_mem_limit = 2ULL * 1024 * 1024 * 1024;
        cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cuda_options.do_copy_in_default_stream = 1;

        sessionOptions.AppendExecutionProvider_CUDA(cuda_options);
        printf("rec try to use GPU%d\n", gpuIndex);
}
    else {
        printf("rec use CPU\n");
    }
#endif

#ifdef __DIRECTML__
    if (gpuIndex >= 0) {
        OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, gpuIndex);
        printf("rec try to use GPU%d\n", gpuIndex);
    }
    else {
        printf("rec use CPU\n");
    }
#endif
}

// ★ 析构：释放 ONNX 会话并清空输入/输出名
CrnnNet::~CrnnNet() {
    delete session;
    inputNamesPtr.clear();
    outputNamesPtr.clear();
}

// ★ setNumThread：设置 ORT 图内/图间并行线程数并开最高图优化等级；
//   须在 initModel 前调（与 DbNet::setNumThread 同构）
void CrnnNet::setNumThread(int numOfThread) {
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

// ★ initModel：构建识别会话并加载字表——keys.txt 每行一个字；
//   头部插入 "#" 占位 CTC 的 blank 类（index 0），尾部补空格兜底，
//   使 keys[i] 与网络第 i 类输出严格对齐；字表缺失时打印告警并保留空表
void CrnnNet::initModel(const std::string &pathStr, const std::string &keysPath) {
#ifdef _WIN32
    std::wstring crnnPath = strToWstr(pathStr);
    session = new Ort::Session(env, crnnPath.c_str(), sessionOptions);
#else
    session = new Ort::Session(env, pathStr.c_str(), sessionOptions);
#endif
    inputNamesPtr = getInputNames(session);
    outputNamesPtr = getOutputNames(session);

    //load keys
    std::ifstream in(keysPath.c_str());
    std::string line;
    if (in) {
        while (getline(in, line)) {// line中不包括每行的换行符
            keys.push_back(line);
        }
    } else {
        printf("The keys.txt file was not found\n");
        return;
    }
    keys.insert(keys.begin(), "#");
    keys.emplace_back(" ");
    printf("total keys size(%lu)\n", keys.size());
}

// ★ argmax：返回 [first,last) 内最大元素的下标（CTC 逐时间步取最优类用）
template<class ForwardIterator>
inline static size_t argmax(ForwardIterator first, ForwardIterator last) {
    return std::distance(first, std::max_element(first, last));
}

// ★ scoreToTextLine：CTC 贪心解码。输入 outputData 为展平的 TxC 概率表
//   （h=T 时间步、w=C 类数），解码规则：
//   ① 每个时间步取 argmax 类及最大概率值；
//   ② maxIndex==0（blank 占位符 "#"）直接跳过——blank 帧不产出字符；
//   ③ 与上一时间步同类（maxIndex==lastIndex）的重复帧跳过——CTC 的
//      "相同连续字符合并"规则，靠 blank 分隔的相同字符（如"aa"）才保留；
//   ④ 其余帧按 keys[maxIndex] 追加字符，并记录该字符置信度。
//   返回（文字串， 逐字符置信度）；时间与耗时由调用方补齐
TextLine CrnnNet::scoreToTextLine(const std::vector<float> &outputData, size_t h, size_t w) {
    auto keySize = keys.size();
    auto dataSize = outputData.size();
    std::string strRes;
    std::vector<float> scores;
    size_t lastIndex = 0;
    size_t maxIndex;
    float maxValue;

    for (size_t i = 0; i < h; i++) {
        size_t start = i * w;
        size_t stop = (i + 1) * w;
        if (stop > dataSize - 1) {
            stop = (i + 1) * w - 1;
        }
        maxIndex = int(argmax(&outputData[start], &outputData[stop]));
        maxValue = float(*std::max_element(&outputData[start], &outputData[stop]));

        if (maxIndex > 0 && maxIndex < keySize && (!(i > 0 && maxIndex == lastIndex))) {
            scores.emplace_back(maxValue);
            strRes.append(keys[maxIndex]);
        }
        lastIndex = maxIndex;
    }
    return {strRes, scores};
}

// ★ getTextLine：单行识别全流程。
//   ① 等比缩放：高统一到 dstHeight=48，宽随行宽等比缩放；
//   ② ★ 提速钳制（v2.1 滞后修复）：CRNN 耗时与输入宽度近线性，超宽行
//      （如整幅横幅文字）单行推理可达数百毫秒拖慢整批识别 → 输入宽度
//      上限 800（水下文字行多为短行，超宽行裁边识别远快于整行），
//      下限 16 防极窄图缩放退化；
//   ③ substractMeanNormalize（(pix-127.5)/127.5，HWC→CHW）；
//   ④ 建 NCHW 张量推理，输出展平拷出；
//   ⑤ scoreToTextLine 做 CTC 解码
TextLine CrnnNet::getTextLine(const cv::Mat &src) {
    float scale = (float) dstHeight / (float) src.rows;
    int dstWidth = int((float) src.cols * scale);
    // ★ 提速钳制：宽度上限 800 / 下限 16（与 RapidOCR 侧 publishPending
    //   节奏匹配，保证移动场景下识别结果 1.5s 内跟上画面）
    if (dstWidth > 800) {
        dstWidth = 800;
    }
    if (dstWidth < 16) {
        dstWidth = 16;
    }
    cv::Mat srcResize;
    resize(src, srcResize, cv::Size(dstWidth, dstHeight));
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
    return scoreToTextLine(outputData, outputShape[1], outputShape[2]);
}

// ★ getTextLines：批量识别入口——逐行调 getTextLine（串行推理），
//   每行单独计时（getCurrentTime 差值）写入 TextLine.time；
//   isOutputDebugImg 开启时先落盘识别输入小图便于排查
std::vector<TextLine> CrnnNet::getTextLines(std::vector<cv::Mat> &partImg, const char *path, const char *imgName) {
    int size = partImg.size();
    std::vector<TextLine> textLines(size);
    for (int i = 0; i < size; ++i) {
        //OutPut DebugImg
        if (isOutputDebugImg) {
            std::string debugImgFile = getDebugImgFilePath(path, imgName, i, "-debug-");
            saveImg(partImg[i], debugImgFile.c_str());
        }

        //getTextLine
        double startCrnnTime = getCurrentTime();
        TextLine textLine = getTextLine(partImg[i]);
        double endCrnnTime = getCurrentTime();
        textLine.time = endCrnnTime - startCrnnTime;
        textLines[i] = textLine;
    }
    return textLines;
}