export const initYolo: (paramPath: string, binPath: string, inputSize?: number) => boolean;
export const detect: (buf: ArrayBuffer, w: number, h: number, th: number) => number[][];
export const releaseYolo: () => void;
export const createYuvReceiver: (w: number, h: number) => string;
export const startPipeline: () => void;
export const updateFrameSize: (w: number, h: number) => boolean;
export const stopPipeline: () => void;
export const getLatestResults: () => number[][];
export const getInferCount: () => number;
export const setConfThresh: (th: number) => void;
export const captureLatestFrame: () => ArrayBuffer;
export const getFrameSize: () => number[];
export const setYoloEnabled: (b: boolean) => void;
// ★ 识别框/标签画面显示开关（false 时检测照常，仅不绘制）
export const setDrawBoxesEnabled: (b: boolean) => void;
// ★ USB 画质增强（仅 USB 摄像头画面生效）：
//   mode 0 关 / 1 非AI(亮度+锐度+对比度+饱和度) / 2 QuickSRNet 超分
export const setUsbEnhance: (mode: number, brightness: number, sharpness: number,
  contrast: number, saturation: number, srStrength: number) => void;
// ★ 人脸识别（SCRFD det_500m + 106 关键点连线框 + 性别年龄颜值）
export const initFace: (detPath: string, lmPath: string, gaPath: string, threads: number) => Promise<boolean>;
export const releaseFace: () => void;
export const setFaceEnabled: (b: boolean) => void;
export const getFaceInfo: () => string;
// ★ AI 超分模型加载（rawfile 提取到沙箱后的 onnx 路径 + 推理线程数）
export const initSr: (onnxPath: string, threads: number) => boolean;
export const releaseSr: () => void;
export const setMotionEnabled: (b: boolean) => void;
export const getMotionLevel: () => number;
export const setDepthEnabled: (b: boolean) => void;
export const getDepthCm: () => number;
export const setDepthParams: (weightG: number, turbidity: number) => void;
export const attachPreviewSurface: (surfaceId: string) => void;
export const attachRecordSurface: (surfaceId: string, width: number, height: number) => void;
export const detachRecordSurface: () => void;
export const setFontPath: (path: string) => void;
export const setClassNames: (names: string[]) => void;
export const setRotation: (degrees: number) => void;
export const setMirror: (mirror: boolean) => void;
export const clearSurface: () => void;
export const setUvcZoom: (scale: number) => void;
export const setLocationText: (text: string) => void;
export const setLocationBottomRatio: (ratio: number) => void;
export const initOcr: (modelDir: string) => Promise<boolean>;
export const initOcrPaths: (detPath: string, clsPath: string, recPath: string, keysPath: string) => Promise<boolean>;
export const releaseOcr: () => void;
export const setOcrEnabled: (b: boolean) => void;
export const getOcrText: () => string;
export const getOcrProgress: () => number[];
// ★ 能耗模式：节奏参数 + 推理线程数（立即生效）
export const setPipelineTiming: (renderMs: number, yoloMs: number, yoloIdleMs: number, sensorMs: number) => void;
export const setYoloThreads: (n: number) => void;
// ★ OCR 参数：定时识别间隔 / det 阈值三件套 / 推理线程数
export const setOcrTriggerInterval: (ms: number) => void;
export const setOcrDetParams: (dbScore: number, dbBox: number, unclip: number) => void;
export const setOcrNumThreads: (n: number) => void;
// ★ 表格结构识别（PP-Structure SLANet）：加载模型 / 单帧识别（返回 JSON）
export const initTable: (tableModelPath: string) => Promise<boolean>;
export const recognizeTable: () => Promise<string>;
export const setupExternalPipeline: (w: number, h: number) => boolean;
export const uvcConnect: (fd: number, vid: number, pid: number, busnum: number, devaddr: number) => boolean;
export const uvcStartStreaming: (w: number, h: number, fps: number) => boolean;
export const uvcStopStreaming: () => void;
export const uvcDisconnect: () => void;
export const uvcGetLastError: () => string;
export const uvcDumpFormats: () => string;
export const uvcGetFrameCount: () => number;
export const uvcGetDecodeFailCount: () => number;
export const uvcGetFrameInfo: () => string;