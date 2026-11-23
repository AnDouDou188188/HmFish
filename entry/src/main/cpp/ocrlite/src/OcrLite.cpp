// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrLite.cpp —— 门面类实现（纯转发到 pImpl）                                   ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】OcrLite 每个公有方法一行转发给 OcrLiteImpl，本文件不含任何            ║
//   业务逻辑——pImpl 惯用法让对外头文件 OcrLite.h 不暴露 OpenCV/ORT 细节，       ║
//   调用方（RapidOCR.cpp）只依赖轻量接口。                                      ║
// ║ 【流水线位置】库的最外层；唯一例外是 Logger——它不走 pImpl 转发，              ║
//   而是借助 friend 声明直接读取 impl 的输出开关与文件句柄，                     ║
//   并用独立的 malloc/free 缓冲格式化（与 impl 的 loggerBuffer 分开）。          ║
// ║ 【与 Android 原版差异】新增 setDetStageCallback 转发；其余一致。              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#include "OcrLite.h"
#include "OcrLiteImpl.h"

// ★ 构造：在堆上创建实现对象（生命周期与门面一致）
OcrLite::OcrLite() {
    pImpl = new OcrLiteImpl();
}

// ★ 析构：销毁实现对象（级联释放三网会话、日志缓冲、txt 句柄）
OcrLite::~OcrLite() {
    delete pImpl;
}

// ★ setDetStageCallback（HmFish 定制新增）：转发注册 det 完成回调，
//   回调在 detect 调用线程内、DbNet 出框后同步触发
void OcrLite::setDetStageCallback(std::function<void(const std::vector<std::vector<cv::Point>> &)> cb) {
    pImpl->setDetStageCallback(std::move(cb));
}

// ★ setNumThread：转发设置三网推理线程数
void OcrLite::setNumThread(int numOfThread) {
    pImpl->setNumThread(numOfThread);
}

// ★ initLogger：转发日志/调试图开关
void OcrLite::initLogger(bool isConsole, bool isPartImg, bool isResultImg) {
    pImpl->initLogger(isConsole, isPartImg, isResultImg);
}

// ★ enableResultTxt：转发开启结果 txt 输出
void OcrLite::enableResultTxt(const char *path, const char *imgName) {
    pImpl->enableResultTxt(path, imgName);
}

// ★ setGpuIndex：转发 GPU 设备号（CPU 构建为空操作）
void OcrLite::setGpuIndex(int gpuIndex) {
    pImpl->setGpuIndex(gpuIndex);
}

// ★ initModels：转发加载 det/cls/rec 三模型与字表
bool OcrLite::initModels(const std::string &detPath, const std::string &clsPath,
                         const std::string &recPath, const std::string &keysPath) {
    return pImpl->initModels(detPath, clsPath, recPath, keysPath);
}

// ★ Logger：格式化日志——经 friend 关系直接读 impl 的开关（isOutputConsole/
//   isOutputResultTxt）与 resultTxt 句柄；临时 malloc 8KB 缓冲格式化，
//   用完即 free。未开任何输出时直接返回
void OcrLite::Logger(const char *format, ...) {
    if (!(pImpl->isOutputConsole || pImpl->isOutputResultTxt)) return;
    char *buffer = (char *) malloc(8192);
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);
    if (pImpl->isOutputConsole) printf("%s", buffer);
    if (pImpl->isOutputResultTxt) fprintf(pImpl->resultTxt, "%s", buffer);
    free(buffer);
}

// ★ detect（文件版）：转发路径入口识别
OcrResult OcrLite::detect(const char *path, const char *imgName,
                          int padding, int maxSideLen,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle) {
    return pImpl->detect(path, imgName, padding, maxSideLen, boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle);
}

// ★ detect（Mat 版）：转发内存图入口识别（HmFish 主用路径）；
//   maxDetCandidates 为 det 候选封顶（-1 不限），透传给 impl（语义见 OcrLite.h）
OcrResult OcrLite::detect(const cv::Mat &mat,
                          int padding, int maxSideLen,
                          float boxScoreThresh, float boxThresh, float unClipRatio, bool doAngle, bool mostAngle,
                          int maxDetCandidates) {
    return pImpl->detect(mat, padding, maxSideLen, boxScoreThresh, boxThresh, unClipRatio, doAngle, mostAngle,
                         maxDetCandidates);
}
