// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ AngleNet.cpp —— 文本方向分类网实现（cls 阶段，0/180 度二分类）                ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】getAngles 批量判断行小图是否整体倒置：doAngle=true 时逐行             ║
// ║   resize 到 192x48 → mean/norm 127.5 前处理 → 推理 → argmax 取                ║
// ║   二分类结果（0=正向 / 1=倒置）；doAngle=false 时整网跳过推理，               ║
// ║   全部返回 index=-1 占位；mostAngle=true 时按多数行的方向统一覆盖。           ║
// ║ 【流水线位置】三网居中：OcrLiteImpl 核心流程第④步，产出 angles[]              ║
// ║   决定哪些行需旋转 180 度再送 CrnnNet。                                       ║
// ║ 【注意】HmFish 的调用方（RapidOCR.cpp）固定传 doAngle=false/mostAngle=false， ║
// ║   即本网在探鱼相机场景完全不推理（省时延），仅保留代码路径完整性。            ║
// ║ 【与 Android 原版差异】无逻辑差异。                                           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "AngleNet.h"
#include "OcrUtils.h"
#include <numeric>

#ifdef __DIRECTML__
#include <onnxruntime/core/providers/dml/dml_provider_factory.h>
#endif

// ★ setGpuIndex：__CUDA__/__DIRECTML__ 构建时挂对应 GPU 提供者；
//   HmFish 为 CPU 构建，函数体被宏裁掉为空
void AngleNet::setGpuIndex(int gpuIndex) {
#ifdef __CUDA__
    if (gpuIndex >= 0) {
        OrtCUDAProviderOptions cuda_options;
        cuda_options.device_id = gpuIndex;
        cuda_options.arena_extend_strategy = 0;
        cuda_options.gpu_mem_limit = 2ULL * 1024 * 1024 * 1024;
        cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchDefault;
        cuda_options.do_copy_in_default_stream = 1;

        sessionOptions.AppendExecutionProvider_CUDA(cuda_options);
        printf("cls try to use GPU%d\n", gpuIndex);
    }
    else {
        printf("cls use CPU\n");
    }
#endif

#ifdef __DIRECTML__
    if (gpuIndex >= 0) {
        OrtSessionOptionsAppendExecutionProvider_DML(sessionOptions, gpuIndex);
        printf("cls try to use GPU%d\n", gpuIndex);
    }
    else {
        printf("cls use CPU\n");
    }
#endif
}

// ★ 析构：释放 ONNX 会话并清空输入/输出名
AngleNet::~AngleNet() {
    delete session;
    inputNamesPtr.clear();
    outputNamesPtr.clear();
}

// ★ setNumThread：设置 ORT 图内/图间并行线程数并开最高图优化等级；
//   须在 initModel 前调（与 DbNet/CrnnNet 同构）
void AngleNet::setNumThread(int numOfThread) {
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

// ★ initModel：按平台构建方向分类会话（Windows 需 wchar 路径），
//   提取输入/输出节点名缓存备用
void AngleNet::initModel(const std::string &pathStr) {
#ifdef _WIN32
    std::wstring clsPath = strToWstr(pathStr);
    session = new Ort::Session(env, clsPath.c_str(), sessionOptions);
#else
    session = new Ort::Session(env, pathStr.c_str(), sessionOptions);
#endif
    inputNamesPtr = getInputNames(session);
    outputNamesPtr = getOutputNames(session);
}

// ★ scoreToAngle：对 2 类输出概率取最大者——maxIndex 即方向判定
//   （0=正向 / 1=倒置），maxScore 为置信度；time 由调用方回填
Angle scoreToAngle(const std::vector<float> &outputData) {
    int maxIndex = 0;
    float maxScore = 0;
    for (size_t i = 0; i < outputData.size(); i++) {
        if (outputData[i] > maxScore) {
            maxScore = outputData[i];
            maxIndex = i;
        }
    }
    return {maxIndex, maxScore};
}

// ★ getAngle：单行方向分类全流程——substractMeanNormalize
//   （(pix-127.5)/127.5，HWC→CHW）→ 建 NCHW 张量推理 → 输出展平拷出 →
//   scoreToAngle 取二分类结果
Angle AngleNet::getAngle(cv::Mat &src) {
    std::vector<float> inputTensorValues = substractMeanNormalize(src, meanValues, normValues);
    std::array<int64_t, 4> inputShape{1, src.channels(), src.rows, src.cols};
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
    return scoreToAngle(outputData);
}

// ★ getAngles：批量方向判定入口。三分支：
//   ① doAngle=true：逐行 resize 到 192x48 后推理（resize 破坏宽高比但
//      方向特征对此不敏感），单行计时；
//   ② doAngle=false：整网跳过推理，全部填 Angle{-1, 0} 占位（-1 表示
//      "未分类"，上游据此不做旋转）——HmFish 固定走此分支，零推理开销；
//   ③ mostAngle 投票（仅 doAngle=true 时）：对全部行的 index 求和，
//      与行数一半比较得多数方向，再统一覆盖所有行——印刷品整页倒置时，
//      个别行的误判被多数票纠正，避免同一页出现旋转不一致
std::vector<Angle> AngleNet::getAngles(std::vector<cv::Mat> &partImgs, const char *path,
                                       const char *imgName, bool doAngle, bool mostAngle) {
    size_t size = partImgs.size();
    std::vector<Angle> angles(size);
    if (doAngle) {
        for (size_t i = 0; i < size; ++i) {
            double startAngle = getCurrentTime();
            cv::Mat angleImg;
            cv::resize(partImgs[i], angleImg, cv::Size(dstWidth, dstHeight));
            Angle angle = getAngle(angleImg);
            double endAngle = getCurrentTime();
            angle.time = endAngle - startAngle;

            angles[i] = angle;

            //OutPut AngleImg
            if (isOutputAngleImg) {
                std::string angleImgFile = getDebugImgFilePath(path, imgName, i, "-angle-");
                saveImg(angleImg, angleImgFile.c_str());
            }
        }
    } else {
        for (size_t i = 0; i < size; ++i) {
            angles[i] = Angle{-1, 0.f};
        }
    }
    //Most Possible AngleIndex
    // 多数投票：index 求和 < 行数一半 → 多数为正向(0)，否则统一倒置(1)
    if (doAngle && mostAngle) {
        auto angleIndexes = getAngleIndexes(angles);
        double sum = std::accumulate(angleIndexes.begin(), angleIndexes.end(), 0.0);
        double halfPercent = angles.size() / 2.0f;
        int mostAngleIndex;
        if (sum < halfPercent) {//all angle set to 0
            mostAngleIndex = 0;
        } else {//all angle set to 1
            mostAngleIndex = 1;
        }
        //printf("Set All Angle to mostAngleIndex(%d)\n", mostAngleIndex);
        for (size_t i = 0; i < angles.size(); ++i) {
            Angle angle = angles[i];
            angle.index = mostAngleIndex;
            angles.at(i) = angle;
        }
    }

    return angles;
}