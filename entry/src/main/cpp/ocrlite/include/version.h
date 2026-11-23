// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ version.h —— ocrlite 移植版版本号定义                                         ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】仅定义一个 VERSION 宏，标识本 ocrlite 目录所移植的 RapidOcrOnnx       ║
// ║   （benjaminwan/OpenCV-OCRAlpha，Android 端离线 OCR）版本基线，               ║
// ║   便于与上游仓库对账、排查模型与代码不匹配的问题。                            ║
// ║ 【流水线位置】独立于 det/cls/rec 三网，不参与任何推理逻辑；                   ║
// ║   仅在需要打印/上报版本信息时引用。                                           ║
// ║ 【与 Android 原版差异】无逻辑差异，纯宏定义，原样保留。                       ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
#ifndef __OCR_VERSION_H__
#define __OCR_VERSION_H__

// ★ VERSION：ocrlite 移植基线版本号（对应 RapidOcrOnnx 上游 1.2.3 tag），
//   与三个 onnx 模型（dbnet/angnet/crnn）及 keys.txt 配套，升级模型时需同步核对
#define VERSION "1.2.3"

#endif //__OCR_VERSION_H__
