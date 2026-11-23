// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ HmFish 主页面（Index.ets）—— 探鱼相机应用总装入口                            ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【页面职责】                                                                 ║
// ║  本文件是应用唯一 @Entry 页面，负责把所有子系统"总装"成一个完整相机应用：      ║
// ║   1. 相机预览：双层 XComponent（cam_raw 原始层 + preview 显示层），           ║
// ║      手机摄像头（后置/前置）与 USB/UVC 外接摄像头两条供帧链路                 ║
// ║   2. AI 能力调度：YOLO 目标识别 / OCR 文字识别 / 表格识别(SLANet) /           ║
// ║      人脸识别(SCRFD) / 水深估计 / 运动检测 / AI 超分增强，全部端侧推理         ║
// ║   3. HUD 与交互：TopHudView（顶部状态徽标）、BottomPanel（底部控制）、        ║
// ║      十余个设置弹窗（识别/画质/能耗/记录/位置…）                              ║
// ║   4. 媒体产出：截图入相册、AVRecorder 录像（native 直供 NV12 帧不中断预览）    ║
// ║   5. 设置持久化：SettingsStore（Preferences）统一读写，150ms 防抖落盘          ║
// ║                                                                              ║
// ║ 【核心数据流】                                                               ║
// ║   相机帧 → native 管线(libentry.so, yuv_pipeline.cpp) →                     ║
// ║     ├─ 处理后 RGBA 推到 preview XComponent 的 surface（用户看到的画面）      ║
// ║     ├─ YOLO/水深/运动/人脸 结果 → tsfn 回调 → VisionController 节流分发 →    ║
// ║     │    onResult/onOcrText/onOcrProgress → 本页 @State → HUD 刷新           ║
// ║     └─ 录像帧(NV12) 直供 AVRecorder 的 surface（编码不走 JS 层）             ║
// ║   反向控制：本页调用 entry.xxx()（setOcrEnabled/setPipelineTiming/           ║
// ║     setUsbEnhance/uvcStartStreaming…）原子热更新 native 管线参数              ║
// ║                                                                              ║
// ║ 【两条供帧链路】                                                             ║
// ║   手机摄像头：CameraService → ImageReceiver(createYuvReceiver) → native      ║
// ║   USB/UVC：UsbDeviceScanner(usbManager) 授权拿 fd → libuvc 取流 →            ║
// ║            setupExternalPipeline 外部供帧（无 ImageReceiver）                ║
// ║   两者最终汇入同一套 native 处理/显示/录像管线，页面状态无差别消费             ║
// ║                                                                              ║
// ║ 【生命周期骨架】                                                             ║
// ║   aboutToAppear：权限申请→设置恢复→模型加载→回调注册→各功能恢复               ║
// ║   XComponent.onLoad → startOrRestoreCamera → startCamera/USB 重连            ║
// ║   onPageHide：停管线/停 USB 取流/带超时释放相机（息屏不占带宽）               ║
// ║   onPageShow：按退出前摄像头恢复（USB 优先重连，拔出则回退后置）              ║
// ║   aboutToDisappear：清全部定时器/监听/TTS，带超时释放相机与录像资源           ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { CameraService } from '../camera/CameraService';
import { CameraSource, CameraStatusEvent } from '../camera/UsbCameraState';
import { SettingsStore } from '../common/SettingsStore';
import { ModelManager } from '../common/ModelManager';
import { OcrManager, OcrModelSet } from '../common/OcrManager';
import { AlertService } from '../common/AlertService';
import { UsbDeviceScanner, UsbCamInfo, UsbPipeHandle } from '../common/UsbDeviceScanner';
import { LoadingOverlay } from '../components/LoadingOverlay';
import { TopHudView, DepthRecord, FaceHudRow } from '../view/TopHudView';
import { GeoService } from '../core/GeoService';
import { BottomPanel, PANEL_OCCUPIED_VP } from '../view/BottomPanel';
import { ModelSheet } from '../view/ModelSheet';
import { PictureSheet } from '../view/PictureSheet';
import { MotionSheet } from '../view/MotionSheet';
import { DepthSheet } from '../view/DepthSheet';
import { RecordSheet } from '../view/RecordSheet';
import { PowerSheet } from '../view/PowerSheet';
import { OcrSheet } from '../view/OcrSheet';
import { RecTimerPopup } from '../view/RecTimerPopup';
import { OcrResultPopup } from '../view/OcrResultPopup';
import { TableResultPopup, TableCellData } from '../view/TableResultPopup';
import { UsbDeviceSheet } from '../view/UsbDeviceSheet';
import { VisionController } from '../core/VisionController';
import { MediaController } from '../core/MediaController';
import { Det, Merged } from '../common/Types';
import { SpeechService } from '../common/SpeechService';
import { POWER_PRESETS } from '../common/PowerPresets';
import { common, Want, abilityAccessCtrl, Permissions } from '@kit.AbilityKit';
import { promptAction, display, window } from '@kit.ArkUI';
import entry from 'libentry.so';
import { image } from '@kit.ImageKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { fileUri } from '@kit.CoreFileKit';
import { picker } from '@kit.CoreFileKit';
import { photoAccessHelper } from '@kit.MediaLibraryKit';
import { dataSharePredicates } from '@kit.ArkData';
import { BusinessError } from '@kit.BasicServicesKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { batteryInfo } from '@kit.BasicServicesKit';

const SENSOR_ROTATION = 90;

// ★ 表格识别 native JSON 原始模型（字段可选，逐个校验后再转 TableCellData）
interface TableParsed {
  rows?: number;
  cols?: number;
  cells?: CellParsed[];
  html?: string;
  // ★ 结构框标注图路径（loading/结果态弹窗顶部预览）
  boxed?: string;
  ms?: number;
  t_struct?: number;
  t_ocr?: number;
  error?: string;
}

interface CellParsed {
  r?: number;
  c?: number;
  rs?: number;
  cs?: number;
  h?: number;
  t?: string;
}

// ★ 人脸识别 HUD 信息（native getFaceInfo 返回 JSON 的结构）
interface FaceHudItem {
  // 人脸序号（=数组位置，兼容保留）
  i?: number;
  // ★ 跨帧跟踪 id（libocr g_tracks 分配：同一张脸持续存在时不变，离场 3s 回收
  //   再入为新 id）。配色/详情面板/箭头跟随一律按 tid 认脸——数组下标随
  //   检测置信度排序跳变，不能作为身份
  tid?: number;
  gender: number;
  age: number;
  beauty: number;
  // 颜值分项（各 0~100，-1=未知）：对称/眼形/眉形/鼻形/嘴形/三庭/眼型
  comps?: number[];
  x: number;
  y: number;
  w: number;
  h: number;
}

interface FaceHudInfo {
  count: number;
  // display 像素尺寸：box 归一化后映射到屏幕坐标画详情箭头
  dw?: number;
  dh?: number;
  faces: FaceHudItem[];
}

// ★ 人脸详情面板分项条数据
interface FaceCompBar {
  name: string;
  v: number;
}

// ★ 人脸框映射到屏幕坐标后的矩形（vp）：箭头终点 / 点框命中判定共用
interface FaceScreenRect {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

// ★ 每人脸配色（6 色循环，与 C++ 端 kFacePalette 完全一致）：
//   连线框/外框/标注/HUD 圆点/详情箭头/详情面板描边同色
const FACE_PALETTE: string[] = ['#00EBB2', '#FF9F40', '#50AAFF', '#FF64AA', '#BC8CFF', '#FFD750'];

// ★ 按跨帧跟踪 id 取色：key=(tid-1)%6 与 C++ DrawFaceOverlay 的 (trackId-1)%6
//   完全一致——tid 是稳定身份，配色/箭头/详情不随置信度排序跳变；
//   tid 无效（<=0，旧数据兜底）时退回数组序号 fallbackIdx
function faceColorOfTid(tid: number, fallbackIdx: number): string {
  const key = tid > 0 ? tid - 1 : fallbackIdx;
  const i = key % 6;
  return FACE_PALETTE[i < 0 ? i + 6 : i];
}

// ★ 能耗三档预设固定值已抽到 common/PowerPresets.ets（Index 应用 + PowerSheet 展示共享）

// ★ 量词映射：识别播报人性化（"发现人1个、电脑1台、草鱼3条"而不是一律"条"）
//   先按中英文关键词匹配常见类名，未命中兜底"个"
function measureFor(name: string): string {
  const n = name.toLowerCase();
  // 人
  if (name.indexOf('人') >= 0 || n.indexOf('person') >= 0 || n.indexOf('people') >= 0 ||
    n.indexOf('man') >= 0 || n.indexOf('child') >= 0 || n.indexOf('baby') >= 0) {
    return '个';
  }
  // 电脑/显示器/相机等台式设备
  if (name.indexOf('电脑') >= 0 || name.indexOf('笔记本') >= 0 || name.indexOf('显示器') >= 0 ||
    name.indexOf('屏幕') >= 0 || name.indexOf('相机') >= 0 || name.indexOf('电视') >= 0 ||
    n.indexOf('computer') >= 0 || n.indexOf('laptop') >= 0 || n.indexOf('monitor') >= 0 ||
    n.indexOf('screen') >= 0 || n.indexOf('camera') >= 0 || n.indexOf('tv') >= 0) {
    return '台';
  }
  // 手机/电话
  if (name.indexOf('手机') >= 0 || name.indexOf('电话') >= 0 || n.indexOf('phone') >= 0) {
    return '部';
  }
  // 车辆（含自行车/摩托车/卡车等）
  if (name.indexOf('车') >= 0 || n.indexOf('car') >= 0 || n.indexOf('truck') >= 0 ||
    n.indexOf('bus') >= 0 || n.indexOf('vehicle') >= 0 || n.indexOf('bike') >= 0 || n.indexOf('bicycle') >= 0) {
    return '辆';
  }
  // 船
  if (name.indexOf('船') >= 0 || n.indexOf('boat') >= 0 || n.indexOf('ship') >= 0) {
    return '艘';
  }
  // 飞机
  if (name.indexOf('飞机') >= 0 || n.indexOf('plane') >= 0 || n.indexOf('airplane') >= 0 || n.indexOf('aircraft') >= 0) {
    return '架';
  }
  // 鱼（钓鱼场景主力类名）
  if (name.indexOf('鱼') >= 0 || n.indexOf('fish') >= 0) {
    return '条';
  }
  // 虾/蟹/猫/狗/鸟等动物
  if (name.indexOf('虾') >= 0 || name.indexOf('蟹') >= 0 || name.indexOf('猫') >= 0 ||
    name.indexOf('狗') >= 0 || name.indexOf('鸟') >= 0 || n.indexOf('cat') >= 0 ||
    n.indexOf('dog') >= 0 || n.indexOf('bird') >= 0 || n.indexOf('shrimp') >= 0 ||
    n.indexOf('crab') >= 0 || n.indexOf('animal') >= 0) {
    return '只';
  }
  // 书
  if (name.indexOf('书') >= 0 || n.indexOf('book') >= 0) {
    return '本';
  }
  // 花
  if (name.indexOf('花') >= 0 || n.indexOf('flower') >= 0) {
    return '朵';
  }
  // 树
  if (name.indexOf('树') >= 0 || n.indexOf('tree') >= 0) {
    return '棵';
  }
  // 伞/椅子
  if (name.indexOf('伞') >= 0 || name.indexOf('椅') >= 0 || n.indexOf('umbrella') >= 0 || n.indexOf('chair') >= 0) {
    return '把';
  }
  // 门/窗
  if (name.indexOf('门') >= 0 || name.indexOf('窗') >= 0 || n.indexOf('door') >= 0 || n.indexOf('window') >= 0) {
    return '扇';
  }
  // 瓶装
  if (name.indexOf('瓶') >= 0 || n.indexOf('bottle') >= 0) {
    return '瓶';
  }
  // 衣物
  if (name.indexOf('衣') >= 0 || n.indexOf('shirt') >= 0 || n.indexOf('cloth') >= 0 || n.indexOf('jacket') >= 0) {
    return '件';
  }
  // 图片/照片
  if (name.indexOf('照片') >= 0 || name.indexOf('图') >= 0 || n.indexOf('picture') >= 0 || n.indexOf('image') >= 0) {
    return '张';
  }
  return '个';
}

// ═══════════════ 页面主体（struct Index）═══════════════
// 成员排布约定（自上而下）：
//   ① @State 响应式状态区（按功能分组：画面/识别/OCR/能耗/人脸/录像/USB…）
//   ② private 服务实例与非响应式内部状态（定时器句柄、锁标志、Canvas 上下文）
//   ③ 功能方法区（位置循环/录像计时/持久化/生命周期/模型/人脸/表格/相机/USB）
//   ④ build()：全屏 Stack 逐层叠放（预览层 → HUD → 弹层 → 弹窗 → loading 遮罩）
@Entry
@Component
struct Index {
  // ───────── ① 画面与加载状态 ─────────
  @State loading: boolean = true;
  @State loadingMsg: string = '正在初始化…';
  @State yoloInited: boolean = false;
  @State previewReady: boolean = false;

  @State cameraPosition: number = 0;
  @State usbConnected: boolean = false;

  // ───────── ② AI 识别结果（native 回调灌入）─────────
  // detList：原始检测框；mergedList：按类名合并后的 HUD 徽标数据
  @State detList: Det[] = [];
  @State mergedList: Merged[] = [];
  @State motionLevel: number = 0;
  @State depthCm: number = 0;
  // ═══════════════ 位置水印循环（GeoService 逆地理）═══════════════
  // ★ 位置水印：启动即取一次；首次成功前每 60s 重试，成功后降频 180s 刷新
  //   （失败保持原水印不动并提示一次原因）
  private geoTimer: number = -1;
  private geoBusy: boolean = false;
  private geoFailToasted: boolean = false;
  private geoSucceeded: boolean = false;

  private startGeoLoop(): void {
    this.refreshGeoPlace();
  }

  private scheduleGeoNext(delayMs: number): void {
    if (this.geoTimer !== -1) clearTimeout(this.geoTimer);
    this.geoTimer = setTimeout((): void => {
      this.geoTimer = -1;
      this.refreshGeoPlace();
    }, delayMs);
  }

  private async refreshGeoPlace(): Promise<void> {
    if (this.geoBusy) return;
    this.geoBusy = true;
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const place = await GeoService.getPlace(ctx);
      if (place !== null) {
        this.geoSucceeded = true;
        // ★ HUD 位置行 + 复制弹窗共享的地名/坐标状态
        this.geoPlaceName = place.name;
        this.geoLat = place.lat;
        this.geoLon = place.lon;
        // ★ 画面/图片/录像底部居中：地名 + 经纬度（如 "深圳市南山区 xx路 22.5431°N,113.9512°E"）
        const latStr = `${Math.abs(place.lat).toFixed(4)}°${place.lat >= 0 ? 'N' : 'S'}`;
        const lonStr = `${Math.abs(place.lon).toFixed(4)}°${place.lon >= 0 ? 'E' : 'W'}`;
        const text = place.name.length > 0 ? `${place.name} ${latStr},${lonStr}` : `${latStr},${lonStr}`;
        try { entry.setLocationText(text); } catch (_) {}
      } else if (!this.geoFailToasted) {
        this.geoFailToasted = true;
        const reason = GeoService.lastFailReason();
        if (reason === 'permission') {
          this.showToast('未授予位置权限，画面不显示位置');
        } else if (reason === 'disabled') {
          this.showToast('系统定位开关未开启，画面不显示位置');
        } else {
          this.showToast('暂未获取到位置，稍后自动重试');
        }
      }
    } catch (_) {} finally {
      this.geoBusy = false;
      // ★ 首次成功后 180s 降频刷新；未成功前 60s 重试
      this.scheduleGeoNext(this.geoSucceeded ? 180000 : 60000);
    }
  }

  // ★ 复制地址/坐标到剪贴板（位置弹窗按钮）
  private copyGeoText(text: string, label: string): void {
    try {
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      pasteboard.getSystemPasteboard().setData(data)
        .then((): void => { this.showToast(`已复制${label}`); })
        .catch((): void => { this.showToast('复制失败'); });
    } catch (_) {
      this.showToast('复制失败');
    }
  }

  private formatGeoLat(): string {
    return `${Math.abs(this.geoLat).toFixed(4)}°${this.geoLat >= 0 ? 'N' : 'S'}`;
  }

  private formatGeoLon(): string {
    return `${Math.abs(this.geoLon).toFixed(4)}°${this.geoLon >= 0 ? 'E' : 'W'}`;
  }
  // ★ 硬件变焦支持：后摄真机范围 >1 走 setZoomRatio；前摄多为 [1,1] 走管线数字变焦兜底
  private hwZoomSupported: boolean = true;
  // ★ 水深历史：最近 3 次读数+时间，避免读数被下一次刷新覆盖丢失
  @State depthHistory: DepthRecord[] = [];
  private lastDepthRecMs: number = 0;
  private lastDepthRecCm: number = -1;

  @State hudOpacity: number = 0.45;
  // ★ HUD 顶部偏移：状态栏安全区高度+小间距（不覆盖系统信号/电量栏，折叠/展开自适应）
  @State hudTopOffset: number = 44;
  @State confThresh: number = 0.25;
  @State modelId: string = '';

  @State yoloEnabled: boolean = true;
  @State ocrEnabled: boolean = false;
  @State motionEnabled: boolean = false;
  // ★ 运动触发灵敏度（默认 20%，弹窗可调、持久化）
  @State motionSens: number = 0.35;
  @State motionSheetVisible: boolean = false;
  @State depthEnabled: boolean = false;

  @State isRecording: boolean = false;
  @State zoomRatio: number = 1.0;
  // ★ 双指捏合缩放起点倍数（手势期间以起点×缩放比换算）
  private pinchStartZoom: number = 1.0;
  // ★ 变焦上限按相机支持范围：手机用 getZoomRatioRange，USB 默认 4x
  @State zoomMax: number = 4.0;
  // ★ 预览区实际尺寸（vp）：按当前内容宽高比适配，避免 USB 16:9 被拉伸变形
  @State previewW: number = 0;
  @State previewH: number = 0;
  @State rotateAngle: number = 0;
  @State rotateText: string = '水平';
  @State currentMode: string = '截图';
  // ★ 摄像头主要参数（预览分辨率）：变焦气泡旁展示，信息更丰富
  @State cameraInfo: string = '';

  @State sheetVisible: boolean = false;
  @State modelLabels: string[] = [];
  @State alertLabels: number[] = [];
  // ★ 水深设置弹窗（浑浊度/重量）与记录设置弹窗（随时保存/自动录像/时长）
  @State depthSheetVisible: boolean = false;
  @State depthTurbidity: number = 1;
  @State depthWeightG: number = 30;
  @State recordSheetVisible: boolean = false;
  @State recordDurationSec: number = 10;
  @State ocrText: string = '';
  @State ocrPopupVisible: boolean = false;
  // ★ 表格结构识别（SLANet）：识别中防重入 + 结果弹窗数据
  @State tableBusy: boolean = false;
  @State tablePopupVisible: boolean = false;
  @State tableRows: number = 0;
  @State tableCols: number = 0;
  @State tableCells: TableCellData[] = [];
  @State tableHtml: string = '';
  @State tableElapsedMs: number = 0;
  // ★ 表格识别"先弹窗后加载"交互：点击立即弹 loading 面板，
  //   模型加载/推理后台进行，完成后面板原地切换为结果
  @State tableLoading: boolean = false;
  @State tableLoadingHint: string = '';
  // ★ 表格识别预览图路径：loading 态=采集帧/结构框标注图，结果态=结构框标注图
  @State tablePreviewPath: string = '';
  private tableSeq: number = 0;          // 放弃识别（识别中关闭弹窗）后使在途回调失效
  private tableTimerId: number = -1;     // loading 已用时计时器
  private tableElapsedSec: number = 0;
  private tableStage: string = '';
  // ★ OCR 识别进度：只在 HUD"识别中"徽标显示，不再画在画面上
  @State ocrDone: number = 0;
  @State ocrTotal: number = 0;

  // ★ 能耗模式（0省电/1标准/2性能/3自定义）+ 六项节奏参数 + 设置弹窗
  @State powerMode: number = 1;
  @State powerRenderMs: number = 40;
  @State powerYoloMs: number = 300;
  @State powerYoloIdleMs: number = 800;
  @State powerSensorMs: number = 250;
  @State powerOcrMs: number = 1500;
  @State powerThreads: number = 8;
  // ★ 自定义档参数（最近一次手动微调）：点"自定义"按钮恢复这套参数
  @State powerCustom: number[] = [40, 300, 800, 1500, 250, 8];
  @State powerSheetVisible: boolean = false;

  // ★ OCR 模型集（长按 OCR 弹窗选择）+ det 阈值三件套
  @State ocrSheetVisible: boolean = false;
  @State ocrModelId: string = 'paddle';
  @State ocrModelSets: OcrModelSet[] = [];
  @State ocrDbScore: number = 0.5;
  @State ocrDbBox: number = 0.3;
  @State ocrUnclip: number = 1.6;

  // ★ 位置信息弹窗（复制地址/坐标）+ 当前地名（HUD 位置行展示）
  @State geoPopupVisible: boolean = false;
  @State geoPlaceName: string = '';
  @State geoLat: number = 0;
  @State geoLon: number = 0;

  @State albumThumb: image.PixelMap | undefined = undefined;
  // ★ 相册按钮：预览手机相册最新一张照片
  @State albumPreviewVisible: boolean = false;
  @State albumPreviewPm: image.PixelMap | undefined = undefined;
  // ★ AI 旧照片修复：PhotoViewPicker 多选 URI（≤9 张）与修复进行中标志
  @State pendingRestoreUris: string[] = [];
  @State restoring: boolean = false;
  @State autoRecordOn: boolean = false;

  // ★ 语音播报（识别设置弹窗开关+音量）+ 自动录像触发音效（开关+自定义 MP3）
  @State speechOn: boolean = true;
  @State speechVolume: number = 30;
  // ★ YOLO 识别结果朗读（识别设置内独立开关，默认关闭）：
  //   开启后实时朗读"识别到草鱼3条、鲫鱼1条"；语音播报总开关关闭时同样不播
  @State detectSpeakOn: boolean = false;
  @State recordSoundOn: boolean = true;
  @State recordSoundName: string = '';
  private recordSoundPath: string = '';

  // ★ 识别框/标签画面显示开关（默认开）：关闭后画面干净不画框/文字，
  //   检测/朗读/自动录像照常；立即保存，重启恢复
  @State drawBoxesOn: boolean = true;
  // ★ 画质增强：亮度/锐度/对比度/饱和度滑条（USB + 手机前后置全部相机生效）
  @State usbBrightness: number = 0;
  // ★ 画质增强模式：0关/1非AI/2 AI增强（超分+降噪+去浊+色彩还原）
  @State usbEnhanceMode: number = 0;
  @State usbSharpness: number = 0;
  @State usbSrStrength: number = 100;
  @State usbContrast: number = 0;
  @State usbSaturation: number = 0;
  // ★ AI 增强附加模块（mode 2 内独立滑条 0~100，修复层异步合成）
  @State aiDenoise: number = 50;
  @State aiClarity: number = 50;
  @State aiColorRestore: number = 60;
  // ★ 人脸识别（SCRFD + 106 关键点连线框 + 性别年龄颜值标注 + HUD 同步）
  @State faceEnabled: boolean = false;
  // ★ 每人脸一行摘要（彩色圆点+文本+"详细"链接），250ms 轮询刷新
  @State faceHudRows: FaceHudRow[] = [];
  // 最新一帧人脸数据（详情面板/箭头实时取数）；必须 @State：detailSummary/
  // detailBeautyText/detailCompList 经 detailFace() 读它，private 时无响应式
  // 依赖 → 详情面板分值永不刷新（HUD 行读 @State faceHudRows 所以正常）
  @State faceHudFaces: FaceHudItem[] = [];
  // display 像素尺寸（box → 屏幕坐标映射用）
  private faceHudDw: number = 0;
  private faceHudDh: number = 0;
  // ★ 人脸详情面板：点某脸"详细"弹出，箭头从面板指向该脸连线框顶中心
  @State faceDetailVisible: boolean = false;
  // ★ 详情面板锁定的跨帧跟踪 id（libocr tid，同脸不变）：
  //   按 tid 而非数组下标跟人——下标随置信度排序跳变，会跟错人/弹错详情
  @State faceDetailTid: number = -1;
  // 面板实测高度（onAreaChange 回填，箭头起点=面板底部中心才准）
  @State faceDetailPanelH: number = 0;
  // ★ 人脸头像：从最新显示帧（含连线框的 rgbaBuf_）按人脸框裁出小图，面板头部展示
  @State faceDetailAvatar: image.PixelMap | null = null;
  private lastAvatarMs: number = 0;
  private faceArrowCtx: CanvasRenderingContext2D =
    new CanvasRenderingContext2D(new RenderingContextSettings(true));
  @State pictureSheetVisible: boolean = false;
  private faceInfoTimer: number = -1;

  // ★ 电池电量（置信度旁显示）：batterySOC 0~100，-1 表示尚未取到；60s 轮询刷新
  @State batterySoc: number = -1;
  @State batteryCharging: boolean = false;
  private batteryTimer: number = -1;

  // ★ USB 层（usbManager）设备列表弹窗，照抄 Android UsbManager + 设备选择 Dialog
  @State usbSheetVisible: boolean = false;
  @State usbDevices: UsbCamInfo[] = [];

  private cameraService: CameraService = new CameraService();
  private models: ModelManager = new ModelManager();
  private ocr: OcrManager = new OcrManager();
  private alertService: AlertService = new AlertService();
  private speech: SpeechService = new SpeechService();
  private vision: VisionController = new VisionController();
  private media: MediaController = new MediaController();
  private usbScanner: UsbDeviceScanner = new UsbDeviceScanner();
  private lastUsbNotifyMs: number = 0;
  // ★ USB 设备 pipe 句柄（connectDevice 结果需保持引用，fd 供 native libuvc 使用）
  private usbPipeHandle: UsbPipeHandle | null = null;

  private rawCtrl: XComponentController = new XComponentController();
  private xcController: XComponentController = new XComponentController();

  private started: boolean = false;
  private switching: boolean = false;
  private fontSandboxPath: string = '';
  private releasingCamera: boolean = false;
  private pageHidden: boolean = false;
  private persistTimer: number = -1;
  private recordDurationMs: number = 10000;
  private autoRecording: boolean = false;
  private autoRecordCooldown: boolean = false;
  private recordStartMs: number = 0;
  private recordTimer: number = -1;
  // ★ USB 帧看门狗定时器（连接成功但无画面时给出明确提示并回退）
  private uvcWatchdogTimer: number = -1;

  // ═══════════════ 录像计时（HUD 红点闪烁 + 00:00 计秒浮窗）═══════════════
  @State recordSec: number = -1;
  @State recBlink: boolean = true;

  private startRecordTimer(): void {
    this.recordStartMs = Date.now();
    this.recordSec = 0;
    this.recBlink = true;
    if (this.recordTimer >= 0) clearInterval(this.recordTimer);
    this.recordTimer = setInterval((): void => {
      const elapsed = Date.now() - this.recordStartMs;
      this.recordSec = Math.floor(elapsed / 1000);
      this.recBlink = Math.floor(elapsed / 500) % 2 === 0;
    }, 250);
  }

  private stopRecordTimer(): void {
    if (this.recordTimer >= 0) { clearInterval(this.recordTimer); this.recordTimer = -1; }
    this.recordSec = -1;
  }

  // ═══════════════ 设置持久化（SettingsStore 统一出口）═══════════════
  // 全部功能开关/参数经 persistSettings() 写入 Preferences；restore 在 aboutToAppear
  private persistSettings(): void {
    if (this.persistTimer >= 0) clearTimeout(this.persistTimer);
    // ★ 所有设置更改立即保存（150ms 防抖合并高频滑条），flush 落盘防丢失
    this.persistTimer = setTimeout(() => {
      this.persistTimer = -1;
      SettingsStore.save({
        hudOpacity: this.hudOpacity,
        cameraPosition: this.cameraPosition,
        zoomRatio: this.zoomRatio,
        confThresh: this.confThresh,
        motionSens: this.motionSens,
        modelId: this.modelId,
        alertLabels: this.alertLabels,
        autoRecord: this.autoRecordOn,
        recordDurationMs: this.recordDurationMs,
        yoloEnabled: this.yoloEnabled,
        ocrEnabled: this.ocrEnabled,
        motionEnabled: this.motionEnabled,
        depthEnabled: this.depthEnabled,
        rotateAngle: this.rotateAngle,
        depthTurbidity: this.depthTurbidity,
        depthWeightG: this.depthWeightG,
        powerMode: this.powerMode,
        powerRenderMs: this.powerRenderMs,
        powerYoloMs: this.powerYoloMs,
        powerYoloIdleMs: this.powerYoloIdleMs,
        powerSensorMs: this.powerSensorMs,
        powerOcrMs: this.powerOcrMs,
        powerThreads: this.powerThreads,
        powerCustom: this.powerCustom,
        ocrModelId: this.ocrModelId,
        ocrDbScore: this.ocrDbScore,
        ocrDbBox: this.ocrDbBox,
        ocrUnclip: this.ocrUnclip,
        speechOn: this.speechOn,
        speechVolume: this.speechVolume,
        detectSpeak: this.detectSpeakOn,
        recordSoundOn: this.recordSoundOn,
        recordSoundName: this.recordSoundName,
        recordSoundPath: this.recordSoundPath,
        drawBoxesOn: this.drawBoxesOn,
        usbBrightness: this.usbBrightness,
        usbSharpness: this.usbSharpness,
        usbEnhanceMode: this.usbEnhanceMode,
        usbSrStrength: this.usbSrStrength,
        usbContrast: this.usbContrast,
        usbSaturation: this.usbSaturation,
        aiDenoise: this.aiDenoise,
        aiClarity: this.aiClarity,
        aiColorRestore: this.aiColorRestore,
        faceEnabled: this.faceEnabled
      });
    }, 150);
  }

  // ═══════════════ 生命周期：aboutToAppear（启动总装）═══════════════
  // 执行顺序：日志自检 → 屏幕/折叠监听 → 权限申请 → 常亮+安全区 →
  //           设置恢复(含旧版迁移) → YOLO 模型加载 → native 开关同步 →
  //           字体/OCR/人脸/超分恢复 → 回调注册 → USB 插拔监听 → 位置循环
  async aboutToAppear(): Promise<void> {
    try {
      // ★ 落盘日志自检：每次启动写一行，重连 PC 后检查 files/uvc_debug.log 是否存在即可确认通路
      this.appendUvcLog('app started (log channel ok)');
      // ★ 折叠屏展开/折叠、屏幕旋转：预览区按最近一次内容比例重新适配屏幕尺寸
      //   （display 尺寸更新略滞后于事件，补一次 500ms 延迟重算确保用上新尺寸）
      this.displayChangeListener = (displayId: number): void => {
        this.fitPreviewToAspect(this.previewAspect);
        setTimeout(() => {
          this.fitPreviewToAspect(this.previewAspect);
          this.recoverPreviewOnSurfaceChanged();
        }, 500);
      };
      try { display.on('change', this.displayChangeListener); } catch (_) {}
      // ★ 折叠开合时内外屏切换，'change' 事件可能不触发，必须监听 foldStatusChange；
      //   折叠后 XComponent surface 可能重建，相机输出与管线推流仍绑旧 surface
      //   → 画面卡死在最后一帧，必须重建预览链路
      this.foldStatusListener = (fs: display.FoldStatus): void => {
        this.fitPreviewToAspect(this.previewAspect);
        setTimeout(() => {
          this.fitPreviewToAspect(this.previewAspect);
          this.recoverPreviewAfterFold();
        }, 500);
      };
      try { display.on('foldStatusChange', this.foldStatusListener); } catch (_) {}
      // ★ 核心：在这里加上动态权限申请，解决 camera permission denied
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      // ★ 识别/录像过程中屏幕不得自动息屏：主窗口常亮
      try {
        const win = await window.getLastWindow(ctx);
        await win.setWindowKeepScreenOn(true);
        // ★ HUD 顶部避让系统状态栏：取安全区上高度，与顶部 0 间距（紧贴状态栏下沿）
        const area = win.getWindowAvoidArea(window.AvoidAreaType.TYPE_SYSTEM);
        this.hudTopOffset = this.getUIContext().px2vp(area.topRect.height);
        win.on('avoidAreaChange', (data: window.AvoidAreaOptions): void => {
          if (data.type === window.AvoidAreaType.TYPE_SYSTEM) {
            this.hudTopOffset = this.getUIContext().px2vp(data.area.topRect.height);
          }
        });
      } catch (_) {}
      const atManager = abilityAccessCtrl.createAtManager();
      const permissions: Array<Permissions> = ['ohos.permission.CAMERA', 'ohos.permission.MICROPHONE'];
      try {
        const result = await atManager.requestPermissionsFromUser(ctx, permissions);
        if (result.authResults[0] !== 0 || result.authResults[1] !== 0) {
          promptAction.showToast({ message: '需要相机和麦克风权限才能使用' });
          this.loading = false;
          return;
        }
      } catch (err) {
        console.error('请求权限失败', err);
      }

      // ---- 下面是你原本的代码，一行没改 ----
      try { entry.stopPipeline(); } catch (_) {}

      await SettingsStore.init(ctx);
      const s = await SettingsStore.load();
      this.hudOpacity = s.hudOpacity;
      this.confThresh = s.confThresh;
      this.motionSens = s.motionSens;
      this.modelId = s.modelId;
      // ★ USB(2) 也恢复：首启/息屏回来按退出前的摄像头走（USB 设备在则重连，含重新授权）
      if (s.cameraPosition === 0 || s.cameraPosition === 1 || s.cameraPosition === 2) {
        this.cameraPosition = s.cameraPosition;
      }
      this.zoomRatio = s.zoomRatio;
      this.alertLabels = s.alertLabels;
      this.recordDurationMs = s.recordDurationMs;
      this.autoRecordOn = s.autoRecord;
      // ★ 所有设置立即保存并完整恢复：功能开关与旋转状态重启不丢
      this.yoloEnabled = s.yoloEnabled;
      this.ocrEnabled = s.ocrEnabled;
      this.motionEnabled = s.motionEnabled;
      this.depthEnabled = s.depthEnabled;
      this.rotateAngle = s.rotateAngle;
      if (this.rotateAngle === 90) this.rotateText = '垂直';
      else if (this.rotateAngle === 180) this.rotateText = '反转';
      else if (this.rotateAngle === 270) this.rotateText = '反向';
      // ★ 水深模型参数 + 自动录像时长恢复
      this.depthTurbidity = s.depthTurbidity;
      this.depthWeightG = s.depthWeightG;
      // ★ 能耗模式 + OCR 模型/参数恢复
      this.powerMode = s.powerMode;
      this.powerRenderMs = s.powerRenderMs;
      this.powerYoloMs = s.powerYoloMs;
      this.powerYoloIdleMs = s.powerYoloIdleMs;
      this.powerSensorMs = s.powerSensorMs;
      this.powerOcrMs = s.powerOcrMs;
      // ★ 旧版迁移：旧标准档 OCR 间隔 4000ms 滞后明显，统一迁到新标准 1500ms
      if (this.powerOcrMs === 4000) {
        this.powerOcrMs = 1500;
      }
      this.powerThreads = s.powerThreads;
      // ★ 自定义档参数恢复（非法长度回退标准预设）
      this.powerCustom = s.powerCustom.length === 6
        ? s.powerCustom : [40, 300, 800, 1500, 250, 8];
      this.ocrModelId = s.ocrModelId;
      this.ocrDbScore = s.ocrDbScore;
      this.ocrDbBox = s.ocrDbBox;
      this.ocrUnclip = s.ocrUnclip;
      // ★ 语音播报 + 触发音效恢复（自定义 MP3 文件校验存在才启用）
      this.speechOn = s.speechOn;
      this.speechVolume = s.speechVolume;
      this.detectSpeakOn = s.detectSpeak;
      this.recordSoundOn = s.recordSoundOn;
      this.recordSoundName = s.recordSoundName;
      this.recordSoundPath = s.recordSoundPath;
      // ★ 识别框显示 + USB 画质增强 + 人脸识别恢复
      this.drawBoxesOn = s.drawBoxesOn;
      this.usbBrightness = s.usbBrightness;
      this.usbSharpness = s.usbSharpness;
      this.usbEnhanceMode = s.usbEnhanceMode;
      this.usbSrStrength = s.usbSrStrength;
      this.usbContrast = s.usbContrast;
      this.usbSaturation = s.usbSaturation;
      this.aiDenoise = s.aiDenoise;
      this.aiClarity = s.aiClarity;
      this.aiColorRestore = s.aiColorRestore;
      this.faceEnabled = s.faceEnabled;
      // ★ 旧版迁移（一次性）：仅存增强参数但模式为关 → 升级为非AI增强模式；
      //   之前每次启动都执行，用户选"关闭"重启后被强行改回——加迁移标记只跑一次
      if (!s.usbParamModeMigrated) {
        if ((this.usbBrightness !== 0 || this.usbSharpness !== 0 ||
          this.usbContrast !== 0 || this.usbSaturation !== 0) && this.usbEnhanceMode === 0) {
          this.usbEnhanceMode = 1;
        }
        SettingsStore.save({ usbParamModeMigrated: true });
      }
      this.speech.enabled = this.speechOn;
      this.speech.volumePct = this.speechVolume;
      this.alertService.enabled = this.recordSoundOn;
      this.alertService.customPath = this.recordSoundPath;
      if (this.recordSoundPath.length > 0) {
        try {
          if (!fs.accessSync(this.recordSoundPath)) {
            this.recordSoundPath = '';
            this.recordSoundName = '';
            this.alertService.customPath = '';
          }
        } catch (_) {
          this.recordSoundPath = '';
          this.recordSoundName = '';
          this.alertService.customPath = '';
        }
      }
      let durSec = Math.round(s.recordDurationMs / 1000);
      if (durSec < 5) durSec = 5;
      if (durSec > 30) durSec = 30;
      this.recordDurationSec = durSec;
      this.recordDurationMs = durSec * 1000;

      await this.models.init(ctx);
      // 兜底：保存的模型不存在时回退默认模型，最终兜底 yolo26m_640
      if (this.modelId === '' || !this.models.findById(this.modelId)) {
        this.modelId = this.models.getDefaultId();
      }
      if (!this.models.findById(this.modelId)) {
        this.modelId = 'yolo26m_640';
      }
      let modelLoaded = await this.loadModel(this.modelId);
      if (!modelLoaded && this.modelId !== 'yolo26m_640') {
        this.modelId = 'yolo26m_640';
        modelLoaded = await this.loadModel(this.modelId);
      }
      if (!modelLoaded) {
        this.showToast('模型加载失败，请检查模型文件');
      }
      this.persistSettings();
      await this.loadModel(this.modelId);

      // ★ 自动录像触发类别按模型分别加载：该模型保存过则用它的，否则回退全局
      const modelAlerts = await SettingsStore.getModelAlerts(this.modelId);
      if (modelAlerts !== null) {
        this.alertLabels = modelAlerts;
      }

      // ★ 恢复上次会话的功能开关（native 侧同步）
      try { entry.setMotionEnabled(this.motionEnabled); } catch (_) {}
      try { entry.setDepthEnabled(this.depthEnabled); } catch (_) {}
      try { entry.setDepthParams(this.depthWeightG, this.depthTurbidity); } catch (_) {}
      // ★ 识别框显示开关 + USB 画质增强恢复（native 原子热更新）
      try { entry.setDrawBoxesEnabled(this.drawBoxesOn); } catch (_) {}
      this.applyUsbEnhanceSettings();
      // ★ AI 超分模式恢复：rawfile 提取模型 + initSr（就绪前自动走普通路径兜底）；
      //   失败降级非AI增强并落盘，避免 AI 模式重启后静默失效被误认为设置没保存
      if (this.usbEnhanceMode >= 2) {
        this.initSrModel(this.usbEnhanceMode).then((ok: boolean): void => {
          if (!ok && this.usbEnhanceMode >= 2) {
            this.usbEnhanceMode = 1;
            this.applyUsbEnhanceSettings();
            this.persistSettings();
            this.showToast('超分模型恢复失败，已切换为非AI增强');
            this.speak('超分模型恢复失败');
          }
        });
      }
      // ★ 人脸识别恢复：rawfile 提取三模型 + initFace + native 开关 + HUD 轮询
      if (this.faceEnabled) {
        this.startFace();
      }
      // ★ 电池电量：置信度旁显示，60s 轮询刷新
      this.startBatteryPoll();
      if (!this.yoloEnabled) {
        try { entry.setYoloEnabled(false); } catch (_) {}
      }
      // ★ 能耗模式：档位参数相对固定——省电/标准/性能强制对齐预设表
      //   （清除旧版本残留值；只有自定义档才保留用户滑条值）
      if (this.powerMode >= 0 && this.powerMode <= 2) {
        const p = POWER_PRESETS[this.powerMode];
        this.powerRenderMs = p[0];
        this.powerYoloMs = p[1];
        this.powerYoloIdleMs = p[2];
        this.powerOcrMs = p[3];
        this.powerSensorMs = p[4];
        this.powerThreads = p[5];
      }
      // ★ 能耗模式：节奏参数 + 推理线程数立即应用（native 原子热更新）
      this.applyPowerParams();

      // ★ OCR 模型集列表刷新（内置 paddle + 外部模型目录扫描）
      this.ocrModelSets = this.ocr.listSets(ctx);
      // ★ OCR 上次是开启的：按退出前的模型集恢复引擎（异步，不阻塞启动）
      if (this.ocrEnabled) {
        const ocrOk = await this.ocr.initSet(ctx, this.ocrModelId);
        if (ocrOk) {
          this.applyOcrParams();
          try { entry.setOcrEnabled(true); } catch (_) {}
        } else {
          this.ocrEnabled = false;
          this.persistSettings();
        }
      }

      try {
        this.fontSandboxPath = await this.copyFontToSandbox(ctx);
        if (this.fontSandboxPath) entry.setFontPath(this.fontSandboxPath);
      } catch (e) {
        console.warn('[font] copy failed:', e);
      }

      this.vision.onResult = (list: Det[], merged: Merged[], motion: number, depth: number): void => {
        this.detList = list;
        this.mergedList = merged;
        this.motionLevel = motion;
        this.depthCm = depth;
        this.pushDepthRecord(depth);
        this.checkAutoRecord(list);
        this.announceDetections(merged);
      };

      this.vision.onOcrText = (text: string): void => {
        // ★ 弹窗打开期间：已有内容则冻结不刷新（防止阅读时被新结果刷掉），无内容允许填充
        if (this.ocrPopupVisible && this.ocrText.length > 0) {
          return;
        }
        this.ocrText = text;
      };

      // ★ OCR 识别进度只进 HUD"识别中"徽标（不再画在画面上）
      this.vision.onOcrProgress = (done: number, total: number): void => {
        this.ocrDone = done;
        this.ocrTotal = total;
      };

      const usb = this.cameraService.getUsbState();
      usb.setOnUsbStatusChange((ev: CameraStatusEvent): void => {
        if (ev.action === 'APPEAR') this.notifyUsbAttached();
        else if (ev.action === 'DISAPPEAR') this.notifyUsbDetached();
      });

      // ★ 照抄 Android USB_DEVICE_ATTACHED/DETACHED 广播：
      //   USB 层插拔公共事件（比相机服务 cameraStatus 更早触发）
      this.usbScanner.subscribe(
        (): void => { this.notifyUsbAttached(); },
        (): void => { this.notifyUsbDetached(); }
      );

      this.refreshAlbumThumb();

      // ★ 位置水印循环：当前地名解析（系统逆地理免费优先，高德/百度 Key 在 GeoService 顶部补充）
      this.startGeoLoop();

      this.loading = false;
    } catch (e) {
      console.error('[aboutToAppear]', e);
      this.loading = false;
    }
  }

  // ═══════════════ 生命周期：onPageHide / onPageShow（后台与前台切换）═══════════════
  // onHide：停管线/停 USB 取流（省带宽防发热）/带超时释放相机；
  // onShow：按退出前摄像头恢复（USB 设备在则重连+重新授权，拔出回退后置）
  onPageHide(): void {
    this.pageHidden = true;
    this.persistSettings();
    // ★ 退后台停止进行中的语音播报
    this.speech.stop();
    this.previewReady = false;
    this.started = false;
    try { entry.stopPipeline(); } catch (_) {}
    this.vision.stop();
    // ★ USB 取流后台继续占 USB 带宽：息屏/退后台先停流，回前台重连恢复到 USB
    if (this.cameraPosition === 2) {
      this.stopUvcWatchdog();
      try { entry.uvcStopStreaming(); } catch (_) {}
      try { entry.uvcDisconnect(); } catch (_) {}
      this.usbPipeHandle = null;
    }
    this.releasingCamera = true;
    const self = this;
    (async (): Promise<void> => {
      // ★ 带 3s 超时释放：onPageHide 时相机服务卡死会锁死 releasingCamera，
      //   onPageShow 恢复重试全部耗尽 → 永远黑屏
      await self.releaseCameraSafe();
      self.releasingCamera = false;
    })();
  }

  onPageShow(): void {
    if (!this.pageHidden) return;
    this.pageHidden = false;
    const self = this;
    const tryStart = (retries: number): void => {
      if ((self.switching || self.releasingCamera) && retries > 0) {
        setTimeout(() => tryStart(retries - 1), 100);
        return;
      }
      if (self.switching || self.releasingCamera) {
        // ★ 重试耗尽仍卡在切换/释放锁（相机服务挂起、释放永不返回）：
        //   强制清锁继续恢复，否则永远黑屏。此时相机多半已死，重建是唯一出路
        self.appendUvcLog('resume: force clear stuck camera flags');
        self.switching = false;
        self.releasingCamera = false;
        self.loading = false;
      }
      const rawId = self.rawCtrl.getXComponentSurfaceId();
      const previewId = self.xcController.getXComponentSurfaceId();
      if (!rawId || !previewId) return;
      // ★ 恢复到退出前选中的摄像头：USB 优先重连（含重新授权），而不是默认后置
      self.startOrRestoreCamera(rawId, previewId);
    };
    tryStart(30);
  }

  // ★ 按退出前选中的摄像头恢复：USB → 重连；否则直接启动手机摄像头
  private startOrRestoreCamera(rawId: string, previewId: string): void {
    if (this.cameraPosition === 2) {
      this.resumeUsbCamera();
      return;
    }
    this.startCamera(rawId, previewId);
  }

  // ★ 恢复 USB 摄像头：设备还在 → 重连（未授权自动弹系统授权框）；
  //   相机服务仍注册 USB → 走服务切换；设备已拔 → 提示并回退后置
  private resumeUsbCamera(): void {
    const cams = UsbDeviceScanner.scan();
    if (cams.length > 0) {
      this.appendUvcLog('resume: reconnect usb camera');
      this.connectUsbDevice(cams[0]);
      return;
    }
    if (this.cameraService.getUsbState().getUsbCamera() !== null) {
      this.appendUvcLog('resume: usb camera via camera service');
      this.restoreCamera(2);
      return;
    }
    this.appendUvcLog('resume: usb camera gone, fallback to back camera');
    this.showToast('USB 摄像头已断开，已切回手机后置摄像头');
    this.cameraPosition = 0;
    this.restoreCamera(0);
  }

  // ═══════════════ 字体与 YOLO 模型加载 ═══════════════
  // copyFontToSandbox：CJK 字体解包到沙箱供 native FreeType 水印绘制；
  // loadModel：释放旧 YOLO → ModelManager 解包 → initYolo → 同步阈值/类名
  private async copyFontToSandbox(ctx: common.UIAbilityContext): Promise<string> {
    const dest = `${ctx.filesDir}/NotoSerifCJKsc-VF.ttf`;
    try { if (fs.accessSync(dest)) return dest; } catch (_) {}
    const data = await ctx.resourceManager.getRawFileContent('NotoSerifCJKsc-VF.ttf');
    const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    fs.writeSync(file.fd, data.buffer);
    fs.closeSync(file);
    return dest;
  }

  private async loadModel(id: string): Promise<boolean> {
    try {
      entry.releaseYolo();
      const bundle = await this.models.prepare(id);
      if (!bundle) return false;
      const ok = entry.initYolo(bundle.paramPath, bundle.binPath, bundle.model.inputSize);
      if (!ok) return false;
      entry.setConfThresh(this.confThresh);
      this.yoloInited = true;
      this.modelLabels = bundle.labels;
      this.vision.setConfig(bundle.labels, this.confThresh, this.yoloEnabled);
      try { entry.setClassNames(bundle.labels); } catch (e) {}
      return true;
    } catch (e) { return false; }
  }

  private toggleYolo(): void {
    this.yoloEnabled = !this.yoloEnabled;
    this.vision.setYoloEnabled(this.yoloEnabled);
    entry.setYoloEnabled(this.yoloEnabled);
    if (!this.yoloEnabled) { this.detList = []; this.mergedList = []; }
    // ★ 照抄 Android toggleRecognize 文案
    this.showToast(this.yoloEnabled ? '已开启识别' : '已关闭识别');
    this.speak(this.yoloEnabled ? '已开启识别' : '已关闭识别');
    this.persistSettings();
  }

  // ═══════════════ OCR 文字识别（开关/参数/模型集切换）═══════════════
  // 引擎：OcrManager 管理 RapidOCR 模型集（内置 paddle/paddle5 + 外部目录扫描）；
  // 触发：native 端场景变化+间隔判定（g_trigger_interval_ms，默认 1500ms），
  //       ArkTS 侧仅控制开关、det 阈值与间隔参数下发
  private async toggleOcr(): Promise<void> {
    if (this.ocrEnabled) {
      this.ocrEnabled = false;
      try { entry.setOcrEnabled(false); } catch (_) {}
      this.ocrText = '';
      this.ocrDone = 0;
      this.ocrTotal = 0;
      this.speak('已关闭文字识别');
      this.persistSettings();
      return;
    }
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const ok = await this.ocr.initSet(ctx, this.ocrModelId);
    if (!ok) {
      this.showToast('OCR 引擎初始化失败');
      return;
    }
    this.ocrEnabled = true;
    this.applyOcrParams();
    try { entry.setOcrEnabled(true); } catch (_) {}
    this.showToast('文字识别已开启');
    this.speak('文字识别已开启');
    this.persistSettings();
  }

  // ★ 能耗模式：节奏参数 + 推理线程数统一应用（启动恢复/预设/滑条共用）
  private applyPowerParams(): void {
    try {
      entry.setPipelineTiming(this.powerRenderMs, this.powerYoloMs,
        this.powerYoloIdleMs, this.powerSensorMs);
    } catch (_) {}
    try { entry.setYoloThreads(this.powerThreads); } catch (_) {}
    try { entry.setOcrTriggerInterval(this.powerOcrMs); } catch (_) {}
  }

  // ★ OCR 参数：det 阈值三件套 + 定时识别间隔（引擎加载后立即生效）
  private applyOcrParams(): void {
    try { entry.setOcrDetParams(this.ocrDbScore, this.ocrDbBox, this.ocrUnclip); } catch (_) {}
    try { entry.setOcrTriggerInterval(this.powerOcrMs); } catch (_) {}
  }

  // ★ 能耗三档预设：固定参数表立即应用 + 立即保存
  private applyPowerPreset(mode: number): void {
    if (mode < 0 || mode >= POWER_PRESETS.length) return;
    const p = POWER_PRESETS[mode];
    this.powerMode = mode;
    this.powerRenderMs = p[0];
    this.powerYoloMs = p[1];
    this.powerYoloIdleMs = p[2];
    this.powerOcrMs = p[3];
    this.powerSensorMs = p[4];
    this.powerThreads = p[5];
    this.applyPowerParams();
    this.persistSettings();
    this.showToast(['已切换省电模式', '已切换标准模式', '已切换性能模式'][mode]);
    this.speak(['已切换到省电模式', '已切换到标准模式', '已切换到性能模式'][mode]);
  }

  // ★ 能耗滑条微调：任一滑条拖动 → 自定义档，立即应用 + 立即保存
  private markPowerCustom(): void {
    if (this.powerMode !== 3) this.powerMode = 3;
  }

  // ★ 自定义档按钮：恢复上次手动微调的参数（powerCustom 数组）并应用
  private applyPowerCustom(): void {
    this.powerMode = 3;
    this.powerRenderMs = this.powerCustom[0];
    this.powerYoloMs = this.powerCustom[1];
    this.powerYoloIdleMs = this.powerCustom[2];
    this.powerOcrMs = this.powerCustom[3];
    this.powerSensorMs = this.powerCustom[4];
    this.powerThreads = this.powerCustom[5];
    this.applyPowerParams();
    this.persistSettings();
    this.showToast('已切换自定义模式');
    this.speak('已切换到自定义模式');
  }

  // ★ OCR 模型集切换：释放旧引擎 → 按新模型集路径初始化 → 恢复参数与开关
  private async selectOcrSet(id: string): Promise<void> {
    if (id === this.ocrModelId) return;
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const wasEnabled = this.ocrEnabled;
    this.ocrModelId = id;
    this.persistSettings();
    const ok = await this.ocr.switchSet(ctx, id);
    if (!ok) {
      this.showToast('模型初始化失败，请检查模型文件');
      // 回退旧模型集
      this.ocrModelId = this.ocr.currentSetId !== '' ? this.ocr.currentSetId : 'paddle';
      this.persistSettings();
      return;
    }
    this.applyOcrParams();
    if (wasEnabled || this.ocrEnabled) {
      try { entry.setOcrEnabled(true); } catch (_) {}
    }
    const set = this.findOcrSetName(id);
    this.showToast(`已切换到 ${set}`);
    this.speak(`已切换到${set}`);
    this.persistSettings();
  }

  private findOcrSetName(id: string): string {
    for (let i = 0; i < this.ocrModelSets.length; i++) {
      if (this.ocrModelSets[i].id === id) return this.ocrModelSets[i].name;
    }
    return id;
  }

  // ★ OCR 模型集短名（HUD 徽标用）：内置两套映射"普通/票据"，自定义取前 4 字
  private shortOcrSetName(): string {
    if (this.ocrModelId === 'paddle') return '普通';
    if (this.ocrModelId === 'paddle5') return '票据';
    const n = this.findOcrSetName(this.ocrModelId);
    return n.length > 4 ? n.substring(0, 4) : n;
  }

  private applyConfThresh(v: number): void {
    this.confThresh = v;
    try { entry.setConfThresh(v); } catch (_) {}
    this.vision.setConfig(this.modelLabels, v, this.yoloEnabled);
    this.persistSettings();
  }

  // ★ 运动检测开关（由运动设置弹窗的 Toggle 调用）
  private toggleMotion(): void {
    this.motionEnabled = !this.motionEnabled;
    entry.setMotionEnabled(this.motionEnabled);
    if (!this.motionEnabled) { this.motionLevel = 0; }
    // ★ 照抄 Android toggleMotion 文案
    this.showToast(this.motionEnabled ? '已开启运动检测' : '已关闭运动检测');
    this.speak(this.motionEnabled ? '已开启运动检测' : '已关闭运动检测');
    this.persistSettings();
  }

  // ★ 水深检测开关（由水深设置弹窗的 Toggle 调用）
  private toggleDepth(): void {
    this.depthEnabled = !this.depthEnabled;
    entry.setDepthEnabled(this.depthEnabled);
    if (!this.depthEnabled) { this.depthCm = 0; }
    this.showToast(this.depthEnabled ? '已开启水深检测' : '已关闭水深检测');
    this.speak(this.depthEnabled ? '已开启水深检测' : '已关闭水深检测');
    this.persistSettings();
  }

  // ★ 语音播报：统一出口（静音模式/开关关闭在 SpeechService 内部拦截）
  private speak(text: string): void {
    this.speech.speak(text);
  }

  // ★ 识别结果语音播报：类名+数量自然语言（"识别到草鱼3条、鲫鱼1条"）
  //   识别循环高频回调，SpeechService 内部节流（内容变化 5 秒/同内容 15 秒）
  private announceDetections(merged: Merged[]): void {
    if (!this.detectSpeakOn || !this.speechOn || !this.yoloEnabled || merged.length === 0) return;
    const parts: string[] = [];
    for (let i = 0; i < merged.length && i < 3; i++) {
      parts.push(`${merged[i].name}${merged[i].count}${measureFor(merged[i].name)}`);
    }
    this.speech.announceDetections(`识别到${parts.join('、')}`);
  }

  // ★ 语音播报开关（识别设置弹窗）
  private toggleSpeech(): void {
    this.speechOn = !this.speechOn;
    this.speech.enabled = this.speechOn;
    this.speak(this.speechOn ? '语音播报已开启' : '语音播报已关闭');
    this.persistSettings();
  }

  // ★ 语音播报音量（0~100，线性映射 TTS 音量 0~1）
  private onSpeechVolumeChange(v: number): void {
    this.speechVolume = v;
    this.speech.volumePct = v;
    this.persistSettings();
    // ★ 松手即试听：播报一句让用户听到调整后的实际音量
    this.speak(`播报音量${v}%`);
  }

  // ★ YOLO 识别结果朗读开关（识别设置内，默认关闭）：只控制"识别到xx N条"的实时朗读
  private toggleDetectSpeak(): void {
    this.detectSpeakOn = !this.detectSpeakOn;
    this.showToast(this.detectSpeakOn ? '已开启识别朗读' : '已关闭识别朗读');
    this.speak(this.detectSpeakOn ? '已开启识别朗读' : '已关闭识别朗读');
    this.persistSettings();
  }

  // ★ 识别框/标签画面显示开关：关闭后画面不画框/文字（识别、朗读、自动录像不受影响）
  private toggleDrawBoxes(): void {
    this.drawBoxesOn = !this.drawBoxesOn;
    try { entry.setDrawBoxesEnabled(this.drawBoxesOn); } catch (_) {}
    this.showToast(this.drawBoxesOn ? '已显示识别框和文字' : '已隐藏识别框和文字');
    this.speak(this.drawBoxesOn ? '已显示识别框和文字' : '已隐藏识别框和文字');
    this.persistSettings();
  }

  // ═══════════════ 画质增强（USB 与手机前后置通用）═══════════════
  // 非AI：亮度/锐度/对比度/饱和度四滑条（native OpenCV 实时处理）
  // AI：QuickSRNet x4 超分 + 降噪/去浊/色彩还原三强度（native 异步合成层）
  // ★ 画质增强统一应用入口：模式/亮度/锐度/对比度/饱和度/AI 四模块强度 → native
  private applyUsbEnhanceSettings(): void {
    try {
      entry.setUsbEnhance(this.usbEnhanceMode, this.usbBrightness, this.usbSharpness,
        this.usbContrast, this.usbSaturation, this.usbSrStrength,
        this.aiDenoise, this.aiClarity, this.aiColorRestore);
    } catch (_) {}
  }

  // ★ 非AI增强恢复默认值（水下场景优化：亮度+15 提亮暗水、锐度 60 清晰优先、
  //   对比度+20 对抗浑浊、饱和度+10 适度还原色彩不艳俗）
  private onUsbResetDefaults(): void {
    this.usbBrightness = 15;
    this.usbSharpness = 60;
    this.usbContrast = 20;
    this.usbSaturation = 10;
    this.applyUsbEnhanceSettings();
    this.persistSettings();
    this.showToast('已恢复水下场景默认值');
    this.speak('已恢复默认值');
  }

  // ★ SR 模型会话已加载标志：srInit 非幂等（重复调用重建 onnxruntime 会话），
  //   加载成功后置 true，USB 增强（mode 2）与相册 AI 修复共用同一会话不再重载
  private srModelReady: boolean = false;

  // ★ 上色模型（DDColor-L，约 241MB）会话已加载标志：colorInit 加载耗时数秒，
  //   成功后常驻（黑白照片上色与相册 AI 修复共用），不重复加载
  private colorModelReady: boolean = false;

  // ★ AI 上色模型确保加载：rawfile 提取 ddcolor.onnx（241MB 一次性拷贝）到沙箱
  //   + native 异步建会话（DDColor-L：黑白/棕褐照片 → 自然彩色，端侧推理）
  private async ensureColorModel(): Promise<boolean> {
    if (this.colorModelReady) return true;
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const name = 'ddcolor.onnx';
      const dest = `${ctx.filesDir}/${name}`;
      if (!fs.accessSync(dest)) {
        const data = await ctx.resourceManager.getRawFileContent(`models/${name}`);
        const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(file.fd, data.buffer);
        fs.closeSync(file);
      }
      const ok = await entry.initColor(dest, this.powerThreads);
      this.colorModelReady = ok;
      return ok;
    } catch (e) {
      return false;
    }
  }

  // ★ AI 超分模型确保加载：rawfile 提取到沙箱 + native 加载（QuickSRNetMedium x4）
  private async ensureSrModel(): Promise<boolean> {
    if (this.srModelReady) return true;
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const name = 'quicksrnetmedium.onnx';
      const dest = `${ctx.filesDir}/${name}`;
      if (!fs.accessSync(dest)) {
        const data = await ctx.resourceManager.getRawFileContent(`models/${name}`);
        const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(file.fd, data.buffer);
        fs.closeSync(file);
      }
      const ok = entry.initSr(dest, this.powerThreads);
      this.srModelReady = ok;
      return ok;
    } catch (e) {
      return false;
    }
  }

  // ★ AI 超分模型加载入口（USB 画质增强 mode 2 专用）：内部复用 ensureSrModel
  private async initSrModel(mode: number): Promise<boolean> {
    if (mode !== 2) {
      return false;
    }
    return this.ensureSrModel();
  }

  // ★ USB 画质增强模式切换（0关/1非AI/2 AI超分）：AI 模式先加载模型，失败降级非AI增强
  private async onUsbModeChange(mode: number): Promise<void> {
    if (mode === this.usbEnhanceMode) return;
    if (mode === 2) {
      const ok = await this.initSrModel(mode);
      if (!ok) {
        this.showToast('超分模型加载失败，已使用非AI增强');
        this.speak('超分模型加载失败');
        mode = 1;
      }
    }
    this.usbEnhanceMode = mode;
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ USB 亮度（-50~+50，正值调亮）：native 立即生效 + 保存
  private onUsbBrightnessChange(v: number): void {
    this.usbBrightness = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ USB 锐度（0~100，unsharp mask）：native 立即生效 + 保存
  private onUsbSharpnessChange(v: number): void {
    this.usbSharpness = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ USB 超分融合强度（0~100，100=全超分）：native 立即生效 + 保存
  private onUsbSrStrengthChange(v: number): void {
    this.usbSrStrength = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ USB 对比度（-50~+50）：native 立即生效 + 保存
  private onUsbContrastChange(v: number): void {
    this.usbContrast = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ USB 饱和度（-50~+50）：native 立即生效 + 保存
  private onUsbSaturationChange(v: number): void {
    this.usbSaturation = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ AI 降噪强度（0~100，SR 引导联合双边滤波层）：native 立即生效 + 保存
  private onAiDenoiseChange(v: number): void {
    this.aiDenoise = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ AI 水下去浊强度（0~100，暗通道先验去雾层）：native 立即生效 + 保存
  private onAiClarityChange(v: number): void {
    this.aiClarity = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ★ AI 色彩还原强度（0~100，灰世界白平衡层）：native 立即生效 + 保存
  private onAiColorRestoreChange(v: number): void {
    this.aiColorRestore = Math.round(v);
    this.applyUsbEnhanceSettings();
    this.persistSettings();
  }

  // ═══════════════ 人脸识别（SCRFD 检测 + 106 关键点 + 性别年龄颜值）═══════════════
  // 数据链：native 每 250ms 轮询 getFaceInfo() → faceHudFaces/faceHudRows →
  //         HUD 每人脸一行摘要；详情面板（头像/分项条/箭头）按需弹出
  // ★ 人脸识别：rawfile 提取三模型（SCRFD det_500m / 2d106det / genderage）+ initFace
  private async initFaceModels(): Promise<boolean> {
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const names: string[] = ['det_500m.onnx', '2d106det.onnx', 'genderage.onnx'];
      const paths: string[] = [];
      for (const name of names) {
        const dest = `${ctx.filesDir}/${name}`;
        if (!fs.accessSync(dest)) {
          const data = await ctx.resourceManager.getRawFileContent(`models/${name}`);
          const file = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
          fs.writeSync(file.fd, data.buffer);
          fs.closeSync(file);
        }
        paths.push(dest);
      }
      return await entry.initFace(paths[0], paths[1], paths[2], this.powerThreads);
    } catch (e) {
      return false;
    }
  }

  // ★ 人脸识别开关：开启时加载模型，失败回退；关闭停 HUD 轮询
  private async toggleFace(): Promise<void> {
    this.faceEnabled = !this.faceEnabled;
    if (this.faceEnabled) {
      const ok = await this.initFaceModels();
      if (!ok) {
        this.showToast('人脸模型加载失败');
        this.speak('人脸模型加载失败');
        this.faceEnabled = false;
      }
    }
    try { entry.setFaceEnabled(this.faceEnabled); } catch (_) {}
    if (this.faceEnabled) {
      this.startFaceInfoPoll();
    } else {
      this.stopFaceInfoPoll();
      this.faceHudRows = [];
      this.faceHudFaces = [];
      this.closeFaceDetail();
    }
    this.showToast(this.faceEnabled ? '已开启人脸识别' : '已关闭人脸识别');
    this.speak(this.faceEnabled ? '已开启人脸识别' : '已关闭人脸识别');
    this.persistSettings();
  }

  // ★ 人脸识别启动（重启恢复路径）：确保模型加载 + native 开关 + HUD 轮询
  private async startFace(): Promise<void> {
    const ok = await this.initFaceModels();
    if (!ok) {
      this.faceEnabled = false;
      this.persistSettings();
      return;
    }
    try { entry.setFaceEnabled(true); } catch (_) {}
    this.startFaceInfoPoll();
  }

  // ★ HUD 人脸信息轮询：250ms 拉一次 getFaceInfo JSON → 每人脸一行摘要 + 详情箭头实时刷新
  private startFaceInfoPoll(): void {
    this.stopFaceInfoPoll();
    this.faceInfoTimer = setInterval((): void => {
      this.refreshFaceHud();
    }, 250);
  }

  private stopFaceInfoPoll(): void {
    if (this.faceInfoTimer !== -1) {
      clearInterval(this.faceInfoTimer);
      this.faceInfoTimer = -1;
    }
  }

  private refreshFaceHud(): void {
    if (!this.faceEnabled) return;
    try {
      const json = entry.getFaceInfo();
      const info = JSON.parse(json) as FaceHudInfo;
      const faces: FaceHudItem[] =
        info.faces !== undefined && info.faces !== null ? info.faces : [];
      this.faceHudFaces = faces;
      this.faceHudDw = info.dw !== undefined && info.dw !== null ? info.dw : 0;
      this.faceHudDh = info.dh !== undefined && info.dh !== null ? info.dh : 0;
      const rows: FaceHudRow[] = [];
      const max = Math.min(faces.length, 4);
      for (let i = 0; i < max; ++i) {
        rows.push(this.buildFaceRow(i, faces[i], info.count));
      }
      // ★ 新数组实例驱动重绘：详情面板/箭头随每次轮询取最新数据
      this.faceHudRows = rows;
    } catch (_) {}
    this.drawFaceArrow();
    this.refreshFaceDetailAvatar();
  }

  // ★ 每人脸一行摘要：性别/年龄/颜值 + 失分项 top3；超过 4 人最后一行带"等N人"
  private buildFaceRow(idx: number, f: FaceHudItem, totalCount: number): FaceHudRow {
    let text = '';
    if (f.gender === 0) {
      text = '女';
    } else if (f.gender === 1) {
      text = '男';
    }
    if (f.age >= 0) {
      text += ` ${f.age}岁`;
    }
    if (f.beauty >= 0) {
      text += ` 颜值${f.beauty}`;
    }
    // 失分项：7 分项中得分最低的 3 项（对称/眼形/眉形/鼻形/嘴形/三庭/眼型）
    const comps = f.comps;
    if (comps !== undefined && comps !== null && comps.length === 7) {
      const names: string[] = ['对称', '眼形', '眉形', '鼻形', '嘴形', '三庭', '眼型'];
      const order: number[] = [0, 1, 2, 3, 4, 5, 6];
      order.sort((a: number, b: number): number => comps[a] - comps[b]);
      let worst = '';
      let shown = 0;
      for (let i = 0; i < 3; ++i) {
        const v = comps[order[i]];
        if (v < 0) continue;
        worst += shown === 0 ? ' 失分:' : ' ';
        worst += `${names[order[i]]}${v}`;
        ++shown;
      }
      if (shown > 0) {
        text += worst;
      }
    }
    if (idx === 3 && totalCount > 4) {
      text += ` 等${totalCount}人`;
    }
    if (text.length === 0) {
      text = '检测到人脸';
    }
    // ★ 配色与"详细"定位都按 tid（跨帧稳定身份）：tid>0 用 tid，旧数据退回序号
    const ftid = f.tid !== undefined && f.tid !== null ? f.tid : 0;
    return { idx: idx, tid: ftid, text: text, color: faceColorOfTid(ftid, idx) };
  }

  // ★ 打开某张人脸的详情面板（HUD 行"详细"胶囊 / 点击预览识别框）：
  //   入参为跨帧跟踪 id（tid）——详情面板按它跨轮询跟踪同一张脸
  private openFaceDetail(tid: number): void {
    if (tid <= 0) return;
    this.faceDetailTid = tid;
    this.faceDetailVisible = true;
    this.lastAvatarMs = 0;
    this.refreshFaceDetailAvatar();
    this.drawFaceArrow();
  }

  // ★ 关闭详情面板并释放头像 PixelMap（防泄漏；延迟释放避免 Image 还引用着已释放图）
  private closeFaceDetail(): void {
    this.faceDetailVisible = false;
    const old = this.faceDetailAvatar;
    this.faceDetailAvatar = null;
    if (old !== null) {
      setTimeout((): void => { try { old.release(); } catch (_) {} }, 200);
    }
  }

  // ★ 详情面板锚点：HUD 顶部偏移 + 固定间距（面板水平居中，箭头从其底部中心出发）
  private faceDetailPanelY(): number {
    return this.hudTopOffset + 186;
  }

  // ★ 人脸框 → 屏幕坐标矩形（vp）：box 归一化（dw/dh）→ 预览区居中映射；
  //   箭头终点与"点识别框弹详情"的命中判定共用
  private faceScreenRect(f: FaceHudItem): FaceScreenRect | null {
    if (this.faceHudDw <= 0 || this.faceHudDh <= 0) {
      return null;
    }
    try {
      const d = display.getDefaultDisplaySync();
      const vpw = d.width / d.densityPixels;
      const vph = d.height / d.densityPixels;
      const pw = this.previewW > 0 ? this.previewW : vpw;
      const ph = this.previewH > 0 ? this.previewH : vph;
      const offX = (vpw - pw) / 2;
      const offY = (vph - ph) / 2;
      const x1 = offX + (f.x / this.faceHudDw) * pw;
      const y1 = offY + (f.y / this.faceHudDh) * ph;
      const x2 = offX + ((f.x + f.w) / this.faceHudDw) * pw;
      const y2 = offY + ((f.y + f.h) / this.faceHudDh) * ph;
      return { x1: x1, y1: y1, x2: x2, y2: y2 };
    } catch (_) {
      return null;
    }
  }

  // ★ 点击预览画面：命中某张人脸的识别框（含 12vp 容差）→ 弹出该脸详情
  //   入参是 XComponent 本地坐标（预览矩形内），先加居中偏移换算到
  //   预览 Stack 旋转前坐标——faceScreenRect 同在该坐标系，旋转角度下也成立
  private handleFaceFrameTap(localX: number, localY: number): void {
    if (!this.faceEnabled || this.faceHudFaces.length === 0) {
      this.appendUvcLog(`[tap] skip enabled=${this.faceEnabled} faces=${this.faceHudFaces.length}`);
      return;
    }
    try {
      const d = display.getDefaultDisplaySync();
      const vpw = d.width / d.densityPixels;
      const vph = d.height / d.densityPixels;
      const pw = this.previewW > 0 ? this.previewW : vpw;
      const ph = this.previewH > 0 ? this.previewH : vph;
      const x = localX + (vpw - pw) / 2;
      const y = localY + (vph - ph) / 2;
      const tol = 12;
      let firstRect = 'null';
      for (let i = 0; i < this.faceHudFaces.length; ++i) {
        const f = this.faceHudFaces[i];
        const rect = this.faceScreenRect(f);
        if (rect === null) {
          continue;
        }
        if (i === 0) {
          firstRect = `(${rect.x1.toFixed(0)},${rect.y1.toFixed(0)})-(${rect.x2.toFixed(0)},${rect.y2.toFixed(0)})`;
        }
        if (x >= rect.x1 - tol && x <= rect.x2 + tol && y >= rect.y1 - tol && y <= rect.y2 + tol) {
          const ftid = f.tid !== undefined && f.tid !== null ? f.tid : 0;
          this.appendUvcLog(`[tap] HIT face tid=${ftid} tap=(${x.toFixed(0)},${y.toFixed(0)})`);
          this.openFaceDetail(ftid);
          return;
        }
      }
      this.appendUvcLog(`[tap] MISS local=(${localX.toFixed(0)},${localY.toFixed(0)}) tap=(${x.toFixed(0)},${y.toFixed(0)})` +
        ` faces=${this.faceHudFaces.length} rect0=${firstRect} pw=${pw.toFixed(0)} ph=${ph.toFixed(0)}`);
    } catch (_) {}
  }

  // ★ 刷新详情面板头像：captureLatestFrame 拿最新显示帧（含连线框的 RGBA），
  //   按人脸框裁正方形小图（周边留 12% 余量带上发型/下巴），≥800ms 节流
  private refreshFaceDetailAvatar(): void {
    if (!this.faceDetailVisible) {
      return;
    }
    const now = Date.now();
    if (now - this.lastAvatarMs < 800) {
      return;
    }
    this.lastAvatarMs = now;
    try {
      const f = this.detailFace();
      if (f === null || this.faceHudDw <= 0 || this.faceHudDh <= 0) {
        return;
      }
      const buf = entry.captureLatestFrame() as ArrayBuffer;
      const s = entry.getFrameSize() as number[];
      if (!buf || buf.byteLength <= 0 || !s || s.length < 2 || s[0] <= 0 || s[1] <= 0) {
        return;
      }
      const imgW = s[0];
      const imgH = s[1];
      if (imgW < 8 || imgH < 8) {
        return;
      }
      // 人脸框（display px）→ 帧像素坐标
      const kx = imgW / this.faceHudDw;
      const ky = imgH / this.faceHudDh;
      let cx = Math.round(f.x * kx);
      let cy = Math.round(f.y * ky);
      let cw = Math.round(f.w * kx);
      let ch = Math.round(f.h * ky);
      // 周边留 12% 余量，并裁成近似正方形（Image objectFit=Cover 下不变形）
      const padX = Math.round(cw * 0.12);
      const padY = Math.round(ch * 0.12);
      cx = Math.max(0, cx - padX);
      cy = Math.max(0, cy - padY);
      cw = Math.min(imgW - cx, cw + padX * 2);
      ch = Math.min(imgH - cy, ch + padY * 2);
      const side = Math.min(cw, ch);
      cx += Math.floor((cw - side) / 2);
      cy += Math.floor((ch - side) / 2);
      if (side < 8) {
        return;
      }
      // 手工裁剪 RGBA（不依赖 PixelMap.crop）：逐行拷贝 side×side
      const src = new Uint8Array(buf);
      const dst = new ArrayBuffer(side * side * 4);
      const dst8 = new Uint8Array(dst);
      for (let row = 0; row < side; ++row) {
        const srcOff = ((cy + row) * imgW + cx) * 4;
        dst8.set(src.subarray(srcOff, srcOff + side * 4), row * side * 4);
      }
      const pm = image.createPixelMapSync(dst, {
        size: { width: side, height: side },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888,
      });
      const old = this.faceDetailAvatar;
      this.faceDetailAvatar = pm;
      if (old !== null) {
        setTimeout((): void => { try { old.release(); } catch (_) {} }, 200);
      }
    } catch (_) {}
  }

  // ★ 当前详情面板对应的人脸（按跨帧跟踪 tid 匹配；人脸离开画面返回 null）：
  //   tid 不随置信度排序/数组重排变化，面板锁定的是"这张脸"而不是"这个下标"
  private detailFace(): FaceHudItem | null {
    if (this.faceDetailTid <= 0) {
      return null;
    }
    for (let i = 0; i < this.faceHudFaces.length; ++i) {
      const f = this.faceHudFaces[i];
      const ftid = f.tid !== undefined && f.tid !== null ? f.tid : 0;
      if (ftid === this.faceDetailTid) {
        return f;
      }
    }
    return null;
  }

  private detailSummary(): string {
    const f = this.detailFace();
    if (f === null) {
      return '';
    }
    let s = '未知';
    if (f.gender === 0) {
      s = '女';
    } else if (f.gender === 1) {
      s = '男';
    }
    if (f.age >= 0) {
      s += ` · ${f.age}岁`;
    }
    return s;
  }

  private detailBeautyText(): string {
    const f = this.detailFace();
    if (f === null || f.beauty < 0) {
      return '';
    }
    return `颜值 ${f.beauty}`;
  }

  private detailCompList(): FaceCompBar[] {
    const names: string[] = ['对称', '眼形', '眉形', '鼻形', '嘴形', '三庭', '眼型'];
    const list: FaceCompBar[] = [];
    const f = this.detailFace();
    if (f !== null && f.comps !== undefined && f.comps !== null && f.comps.length === 7) {
      for (let i = 0; i < 7; ++i) {
        const v = f.comps[i];
        list.push({ name: names[i], v: v >= 0 ? Math.round(v) : 0 });
      }
    }
    return list;
  }

  // ★ 分项条配色：≥70 绿 / ≥40 黄 / 其余红
  private compBarColor(v: number): string {
    if (v >= 70) return '#34C759';
    if (v >= 40) return '#FFD60A';
    return '#FF6B5E';
  }

  // ★ 复制当前详情人脸信息（性别/年龄/颜值/7 分项）到剪贴板
  private copyFaceInfo(): void {
    const f = this.detailFace();
    if (f === null) {
      this.showToast('人脸已离开画面');
      return;
    }
    const names: string[] = ['对称', '眼形', '眉形', '鼻形', '嘴形', '三庭', '眼型'];
    let text = this.detailSummary();
    if (f.beauty >= 0) {
      text += ` 颜值${f.beauty}`;
    }
    if (f.comps !== undefined && f.comps !== null && f.comps.length === 7) {
      const parts: string[] = [];
      for (let i = 0; i < 7; ++i) {
        if (f.comps[i] >= 0) {
          parts.push(`${names[i]}${f.comps[i]}`);
        }
      }
      if (parts.length > 0) {
        text += ` 分项: ${parts.join(' ')}`;
      }
    }
    this.copyGeoText(text, '人脸信息');
  }

  // ★ 详情箭头：面板底部中心 → 人脸连线框顶中心，颜色与人脸框一致；
  //   每次 250ms 轮询重画，人脸移动箭头实时跟随
  private drawFaceArrow(): void {
    if (!this.faceDetailVisible || this.faceDetailTid <= 0) {
      return;
    }
    try {
      const d = display.getDefaultDisplaySync();
      const vpw = d.width / d.densityPixels;
      const vph = d.height / d.densityPixels;
      const ctx = this.faceArrowCtx;
      ctx.clearRect(0, 0, vpw, vph);
      const f = this.detailFace();
      if (f === null) {
        return;
      }
      // 人脸框顶中心（共享映射：faceScreenRect）
      const rect = this.faceScreenRect(f);
      if (rect === null) {
        return;
      }
      const ex = (rect.x1 + rect.x2) / 2;
      const ey = rect.y1;
      // 箭头起点：详情面板底部中心（面板 y 固定锚在 HUD 下方，高度实测回填）
      const sx = vpw / 2;
      const sy = this.faceDetailPanelY() +
        (this.faceDetailPanelH > 0 ? this.faceDetailPanelH : 220);
      const color = faceColorOfTid(this.faceDetailTid, 0);
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(sx, sy);
      ctx.lineTo(ex, ey);
      ctx.stroke();
      // 箭头头部（两撇）+ 框顶圆点
      const ang = Math.atan2(ey - sy, ex - sx);
      const head = 12;
      ctx.beginPath();
      ctx.moveTo(ex, ey);
      ctx.lineTo(ex - head * Math.cos(ang - 0.42), ey - head * Math.sin(ang - 0.42));
      ctx.moveTo(ex, ey);
      ctx.lineTo(ex - head * Math.cos(ang + 0.42), ey - head * Math.sin(ang + 0.42));
      ctx.stroke();
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(ex, ey, 4, 0, Math.PI * 2);
      ctx.fill();
    } catch (_) {}
  }

  // ★ 电池电量刷新（置信度旁显示）：batterySOC 静态快照需轮询读新值
  private refreshBattery(): void {
    try {
      const soc = batteryInfo.batterySOC;
      if (soc >= 0 && soc <= 100) this.batterySoc = soc;
      // chargingStatus：1=充电中 3=已充满（仍插电）均显示 ⚡
      const st = batteryInfo.chargingStatus;
      this.batteryCharging = st === 1 || st === 3;
    } catch (_) {}
  }

  private startBatteryPoll(): void {
    this.refreshBattery();
    if (this.batteryTimer >= 0) clearInterval(this.batteryTimer);
    this.batteryTimer = setInterval((): void => { this.refreshBattery(); }, 60000);
  }

  // ★ 触发音效开关（识别设置弹窗）
  private toggleRecordSound(): void {
    this.recordSoundOn = !this.recordSoundOn;
    this.alertService.enabled = this.recordSoundOn;
    this.showToast(this.recordSoundOn ? '已开启触发音效' : '已关闭触发音效');
    this.speak(this.recordSoundOn ? '已开启触发音效' : '已关闭触发音效');
    this.persistSettings();
  }

  // ★ 试听当前触发音效（自定义 MP3 或默认提示音）：静音模式明确提示
  //   试听同样按录像时长截断：短于录像全播，长于录像只播高潮段，避免音乐一直播
  private previewRecordSound(): void {
    if (SpeechService.isRingerSilent()) {
      this.showToast('手机静音/震动模式，不播放提示音');
      return;
    }
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.alertService.play(ctx, undefined, this.recordDurationMs);
  }

  // ★ 选择自定义 MP3：文件选择器挑 .mp3 → 拷入沙箱 → 立即试听
  //   未选自定义时点"选择 MP3"也可直接试听默认提示音
  private async pickRecordSound(): Promise<void> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    try {
      const pickerObj = new picker.DocumentViewPicker();
      const opts = new picker.DocumentSelectOptions();
      opts.fileSuffixFilters = ['.mp3'];
      opts.maxSelectNumber = 1;
      const uris: Array<string> = await pickerObj.select(opts);
      if (uris.length === 0) return;
      const dest = `${ctx.filesDir}/record_sound.mp3`;
      const src = fs.openSync(uris[0], fs.OpenMode.READ_ONLY);
      const dst = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.copyFileSync(src.fd, dst.fd);
      fs.closeSync(src);
      fs.closeSync(dst);
      // ★ 文件名展示（URI 末段）
      let name = '自定义音效';
      const seg = uris[0].split('/');
      if (seg.length > 0 && seg[seg.length - 1].length > 0) name = decodeURIComponent(seg[seg.length - 1]);
      this.recordSoundPath = dest;
      this.recordSoundName = name;
      this.alertService.customPath = dest;
      this.persistSettings();
      this.showToast(`已选择 ${name}`);
      this.speak(`已选择 ${name}`);
      // ★ 选完立即试听：同样按录像时长截断（短于录像全播，长于录像只播高潮段）
      this.alertService.play(ctx, undefined, this.recordDurationMs);
    } catch (e) {
      const err = e as BusinessError;
      if (err.code !== 13900042) {
        // 13900042 = 用户取消选择，不算错误
        this.showToast('选择音效失败');
        console.error(`[recordSound] pick failed: ${err.code} ${err.message}`);
      }
    }
  }

  // ★ 恢复默认触发音效（清除自定义 MP3）
  private resetRecordSound(): void {
    this.recordSoundPath = '';
    this.recordSoundName = '';
    this.alertService.customPath = '';
    this.persistSettings();
    this.showToast('已恢复默认提示音');
    this.speak('已恢复默认提示音');
  }

  // ★ 水深模型参数：水的浑浊度（0清澈/1微浑/2浑浊）
  private onDepthTurbidityChange(idx: number): void {
    this.depthTurbidity = idx;
    try { entry.setDepthParams(this.depthWeightG, this.depthTurbidity); } catch (_) {}
    this.persistSettings();
  }

  // ★ 水深模型参数：摄像头重量(g)
  private onDepthWeightChange(g: number): void {
    this.depthWeightG = g;
    try { entry.setDepthParams(this.depthWeightG, this.depthTurbidity); } catch (_) {}
    this.persistSettings();
  }

  // ★ 录像时长（5~30 秒滑动选择，自动录像按此时长录制）
  private onRecordDurationChange(sec: number): void {
    this.recordDurationSec = sec;
    this.recordDurationMs = sec * 1000;
    this.persistSettings();
  }

  // ★ 随时保存：未录像=开始录像；录像中=不等时长、立即停止并保存
  private onRecordSaveNow(): void {
    this.recordSheetVisible = false;
    this.toggleRecord();
  }

  private toggleAlertLabel(idx: number): void {
    const i = this.alertLabels.indexOf(idx);
    if (i >= 0) {
      const next: number[] = [];
      for (let k = 0; k < this.alertLabels.length; k++) {
        if (this.alertLabels[k] !== idx) next.push(this.alertLabels[k]);
      }
      this.alertLabels = next;
    } else {
      const next = this.alertLabels.slice();
      next.push(idx);
      this.alertLabels = next;
    }
    this.persistSettings();
    // ★ 自动录像触发类别按当前模型分别保存：切换模型回来还能找回
    SettingsStore.setModelAlerts(this.modelId, this.alertLabels);
  }

  private async selectModel(id: string): Promise<void> {
    if (id === this.modelId) return;
    this.loading = true;
    const prevModelId = this.modelId;
    const prevLabels = this.alertLabels;
    const ok = await this.loadModel(id);
    if (ok) {
      this.modelId = id;
      // ★ 自动录像触发类别按模型分别保存/加载：旧模型存档，新模型读档，切换不清空
      await SettingsStore.setModelAlerts(prevModelId, prevLabels);
      const saved = await SettingsStore.getModelAlerts(id);
      this.alertLabels = saved !== null ? saved : [];
      this.persistSettings();
      this.showToast('已切换模型');
      this.speak('已切换模型');
    } else {
      this.showToast('模型加载失败');
    }
    this.loading = false;
  }

  private checkAutoRecord(list: Det[]): void {
    if (!this.autoRecordOn) return;
    if (this.isRecording || this.autoRecording || this.autoRecordCooldown) return;
    if (this.switching || this.loading || this.releasingCamera || this.pageHidden) return;
    // ---- 照抄 Android applyDetections：YOLO 命中勾选类别触发 ----
    if (this.yoloEnabled && this.alertLabels.length > 0) {
      const hits: string[] = [];
      const counts: Record<string, number> = {};
      for (let i = 0; i < list.length; i++) {
        const label = list[i].label;
        if (this.alertLabels.indexOf(label) >= 0) {
          const name = list[i].name;
          if (hits.indexOf(name) < 0) hits.push(name);
          counts[name] = (counts[name] ?? 0) + 1;
        }
      }
      if (hits.length > 0) {
        // ★ 语音文案：发现+类名+量词+数量（"发现草鱼3条、人1个"），MP3 播完后播报
        let announce = '发现';
        for (let i = 0; i < hits.length; i++) {
          announce += `${hits[i]}${counts[hits[i]]}${measureFor(hits[i])}`;
          if (i < hits.length - 1) announce += '、';
        }
        this.startAutoRecord(`识别到 ${hits.join(',')}`, announce);
        return;
      }
    }
    // ---- 照抄 Android applyDetections：运动检测触发（阈值=灵敏度设置，默认 20%）----
    if (this.motionEnabled && this.motionLevel >= this.motionSens) {
      const pct = Math.round(this.motionLevel * 100);
      this.startAutoRecord(`运动检测 ${pct}%`, '');
    }
  }

  private toggleAutoRecord(): void {
    this.autoRecordOn = !this.autoRecordOn;
    this.persistSettings();
    // ★ 照抄 Android toggleAutoRecord 文案
    this.showToast(this.autoRecordOn ? '已开启自动记录' : '已关闭自动记录');
    this.speak(this.autoRecordOn ? '已开启自动记录' : '已关闭自动记录');
  }

  private beginAutoRecordCooldown(): void {
    this.autoRecordCooldown = true;
    setTimeout(() => { this.autoRecordCooldown = false; }, 15000);
  }

  // ═══════════════ 录像（手动/自动/随时保存）═══════════════
  // 手动：底部录像按钮 toggleRecord；自动：checkAutoRecord 由 YOLO 命中/运动触发；
  // 架构：AVRecorder.getInputSurface 交给 native 直供 NV12 帧——预览/识别全程不中断
  // ★ 录像不中断（对齐 Android 录像架构）：AVRecorder getInputSurface 交给
  //   native 直供 NV12 帧，相机/预览/识别/运动/水深全程不重配不中断
  private async beginRecording(durationMs: number): Promise<boolean> {
    if (this.isRecording) return false;
    if (!this.previewReady || this.switching || this.loading || this.pageHidden) {
      this.showToast(`${this.camLabel()}未就绪，无法录像`);
      return false;
    }
    // 编码尺寸跟随当前画面（手机竖屏 480x640 / USB 640x480）
    let vw = 640;
    let vh = 480;
    try {
      const sz = entry.getFrameSize();
      if (sz && sz.length >= 2 && sz[0] > 0 && sz[1] > 0) {
        vw = sz[0];
        vh = sz[1];
      }
    } catch (_) {}
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const withAudio = this.cameraService.getUsbState().isMicGranted();
    const videoSurface = await this.media.recorder.prepare(ctx, durationMs, withAudio, vw, vh);
    if (!videoSurface) {
      this.showToast('录像启动失败');
      return false;
    }
    try { entry.attachRecordSurface(videoSurface, vw, vh); } catch (_) {}
    const ok = await this.media.recorder.start();
    if (!ok) {
      try { entry.detachRecordSurface(); } catch (_) {}
      this.showToast('录像启动失败');
      return false;
    }
    this.isRecording = true;
    this.startRecordTimer();
    return true;
  }

  private async endRecording(): Promise<void> {
    if (!this.isRecording) return;
    this.isRecording = false;
    this.stopRecordTimer();
    try { entry.detachRecordSurface(); } catch (_) {}
    // ★ 编码器排空/封装实测 ~2s：先给"正在保存"反馈，别让用户干等
    this.showToast('正在保存录像…');
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const path = await this.media.recorder.stop();
    if (path) {
      const r = await this.media.saveToGallery(ctx, path, true);
      if (r.success) {
        this.showToast('录像已保存到相册');
        this.speak('录像已保存到相册');
        // ★ 录像入册后同步刷新相册缩略图（与截图行为一致）
        this.refreshAlbumThumb();
      }
    }
  }

  private async startAutoRecord(reason: string, announce: string): Promise<void> {
    if (this.isRecording || this.autoRecording || this.autoRecordCooldown) return;
    this.autoRecording = true;
    this.beginAutoRecordCooldown();
    // ★ 诊断日志：自动录像触发时的水深/运动上下文，hilog 里可对照 [depth] 状态行
    console.info(`[autoRecord] ${reason} | motion=${(this.motionLevel * 100).toFixed(0)}% sens=${(this.motionSens * 100).toFixed(0)}% depth=${this.depthEnabled ? `${this.depthCm.toFixed(0)}cm` : 'off'}`);
    // ★ 照抄 Android showTip("$reason，开始自动录像 ${sec}s")
    const sec = Math.round(this.recordDurationMs / 1000);
    try { promptAction.showToast({ message: `${reason}，开始自动录像 ${sec}s`, duration: 3000 }); } catch (_) {}
    // ★ 先播 MP3 告警音；MP3 播放结束且语音开关开启时，再播报"发现xx N条"
    //   （MP3 未配置/静音模式时回调立即触发，语音不延迟）
    //   MP3 时长上限=录像时长：比录像短全播，比录像长只播高潮段（35% 处起）
    try {
      const actx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      this.alertService.play(actx, (): void => {
        if (this.speechOn) {
          this.speak(announce.length > 0 ? announce : `${reason}，开始自动录像${sec}秒`);
        }
      }, this.recordDurationMs);
    } catch (_) {}
    // ★ 照抄 Android autoRecord：到时自动停录；先立即停表（编码器 flush ~2s 不计入显示时长），
    //   native 摘掉录像直供 surface 后再停编码器
    this.media.recorder.onBeforeAutoStop = (): void => {
      this.isRecording = false;
      this.stopRecordTimer();
      try { entry.detachRecordSurface(); } catch (_) {}
      // ★ 编码器排空/封装 ~2s：先给"正在保存"反馈
      this.showToast('正在保存录像…');
    };
    this.media.recorder.onAutoStop = (): void => {
      this.finishAutoRecord();
    };
    const ok = await this.beginRecording(this.recordDurationMs);
    if (!ok) this.autoRecording = false;
  }

  private async finishAutoRecord(): Promise<void> {
    this.isRecording = false;
    this.autoRecording = false;
    this.stopRecordTimer();
    const path = this.media.getRecorderPath();
    if (path) {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const r = await this.media.saveToGallery(ctx, path, true);
      // ★ 照抄 Android onComplete：提示「自动录像完成」并延迟刷新相册
      if (r.success) {
        this.showToast('自动录像完成');
        this.speak('自动录像完成');
        this.refreshAlbumThumb();
      } else if (r.reason !== 'canceled') {
        // ★ 保存失败不再静默：明确告知用户，避免"录像去哪了"的困惑
        this.showToast(`录像保存失败：${r.reason}`);
      }
    }
  }

  private showToast(msg: string): void {
    try { promptAction.showToast({ message: msg, duration: 1800 }); } catch (_) {}
  }

  // 相册按钮缩略图：优先取相册最新一张照片，无权限时回退到本应用最近的截图
  private async refreshAlbumThumb(): Promise<void> {
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const helper = photoAccessHelper.getPhotoAccessHelper(ctx);
      const predicates = new dataSharePredicates.DataSharePredicates();
      predicates.orderByDesc(photoAccessHelper.PhotoKeys.DATE_ADDED);
      predicates.limit(1, 0);
      const opts: photoAccessHelper.FetchOptions = { fetchColumns: [], predicates: predicates };
      const result = await helper.getAssets(opts);
      const asset = await result.getFirstObject();
      const thumb = await asset.getThumbnail({ width: 96, height: 96 });
      this.albumThumb = thumb;
      result.close();
      return;
    } catch (e) {
      console.warn('[album] gallery query failed, fallback to local cache');
    }
    await this.loadCachedAlbumThumb();
  }

  private async loadCachedAlbumThumb(): Promise<void> {
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const path = `${ctx.filesDir}/album_thumb.jpg`;
      if (!fs.accessSync(path)) return;
      const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const source = image.createImageSource(file.fd);
      const pm = await source.createPixelMap({ desiredSize: { width: 96, height: 96 } });
      await source.release();
      fs.closeSync(file);
      this.albumThumb = pm;
    } catch (e) {
      console.warn('[album] load cached thumb failed');
    }
  }

  // ★ 相册按钮：系统 PhotoViewPicker 多选照片（免 READ_IMAGEVIDEO 权限，默认打开
  //   手机相册图库），选中后预览首张并可对全部所选照片执行 AI 高清彩色修复。
  //   （旧实现 helper.getAssets 直查媒体库需 ACL 受限权限，永远失败导致
  //    "暂无可预览的相册图片"——改走系统选择器彻底绕开权限问题）
  private async openAlbumPicker(): Promise<void> {
    try {
      const photoPicker = new photoAccessHelper.PhotoViewPicker();
      const selectOpt = new photoAccessHelper.PhotoSelectOptions();
      selectOpt.MIMEType = photoAccessHelper.PhotoViewMIMETypes.IMAGE_TYPE;
      selectOpt.maxSelectNumber = 9;
      const res: photoAccessHelper.PhotoSelectResult = await photoPicker.select(selectOpt);
      if (!res.photoUris || res.photoUris.length === 0) return;
      this.pendingRestoreUris = res.photoUris;
      // 预览首张（长边 ≤720 解码，与修复流水线输入同规格）
      const pm = await this.decodeForRestore(res.photoUris[0]);
      if (!pm) {
        this.pendingRestoreUris = [];
        this.showToast('图片解码失败');
        return;
      }
      this.albumPreviewPm = pm;
      this.albumPreviewVisible = true;
    } catch (e) {
      this.showToast('打开相册选择器失败');
    }
  }

  // ★ 解码所选照片：等比缩放至长边 ≤720（SR 全帧 onnxruntime 推理的输入上限，
  //   同时控制 readPixelsToBuffer 内存），输出像素图供预览与修复共用。
  //   PhotoViewPicker 返回 file://media/ 相册 URI：须经 fs.openSync 换 fd 解码，
  //   直接 createImageSource(uri) 在部分系统版本会抛异常（"图片解码失败"根因）
  private async decodeForRestore(uri: string): Promise<image.PixelMap | undefined> {
    let file: fs.File | undefined = undefined;
    try {
      file = fs.openSync(uri, fs.OpenMode.READ_ONLY);
      const source = image.createImageSource(file.fd);
      const info = await source.getImageInfo();
      const ow = info.size.width;
      const oh = info.size.height;
      if (ow <= 0 || oh <= 0) {
        await source.release();
        return undefined;
      }
      // 等比缩放：目标 desiredSize 按原图宽高比计算，避免拉伸变形
      const ratio = Math.max(ow, oh) > 720 ? 720 / Math.max(ow, oh) : 1;
      const dw = Math.max(2, Math.round(ow * ratio));
      const dh = Math.max(2, Math.round(oh * ratio));
      const pm = await source.createPixelMap({
        desiredSize: { width: dw, height: dh }
      });
      await source.release();
      return pm;
    } catch (e) {
      return undefined;
    } finally {
      if (file) { try { fs.closeSync(file); } catch (_) {} }
    }
  }

  // ★ 单张照片 AI 修复全链路：解码 ≤720 → readPixelsToBuffer → native
  //   restorePhoto（去噪+超分×4+褪色修复）→ RGBA 像素图 → JPEG 95 → 入系统相册。
  //   isFirst：首张成功后同步刷新预览图与缩略图缓存（立即看到修复效果）
  private async restoreOnePhoto(ctx: common.UIAbilityContext, uri: string, isFirst: boolean): Promise<boolean> {
    const pm = await this.decodeForRestore(uri);
    if (!pm) return false;
    let outPath: string = '';
    try {
      // ① 读取像素：按实际 pixelFormat 判断字节序（部分设备解码为 BGRA_8888）
      const info = await pm.getImageInfo();
      const w = info.size.width;
      const h = info.size.height;
      const isBgra = info.pixelFormat === image.PixelMapFormat.BGRA_8888;
      const buf = new ArrayBuffer(w * h * 4);
      await pm.readPixelsToBuffer(buf);
      // ② native AI 修复（异步数秒，Promise 回调不阻塞 UI）
      const r = await entry.restorePhoto(buf, w, h, isBgra);
      if (!r || !r.buf || r.w <= 0 || r.h <= 0) return false;
      const outPm = image.createPixelMapSync(r.buf, {
        size: { width: r.w, height: r.h },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888
      });
      // ③ JPEG 编码到沙箱缓存（quality 95 兼顾老照片细节与体积）
      outPath = `${ctx.cacheDir}/restore_${Date.now()}.jpg`;
      const file = fs.openSync(outPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const packer = image.createImagePacker();
      await packer.packToFile(outPm, file.fd, { format: 'image/jpeg', quality: 95 });
      packer.release();
      fs.closeSync(file);
      // ④ 入系统相册（复用截图保存链路：直接入库 + 授权过期弹窗兜底）
      const saveRes = await this.media.saveToGallery(ctx, outPath, false);
      if (!saveRes.success) return false;
      if (isFirst) {
        // 首张：更新预览为修复结果 + 刷新缩略图（解码大图做缓存）
        this.albumPreviewPm = outPm;
        try { fs.copyFileSync(outPath, `${ctx.filesDir}/album_thumb.jpg`); } catch (_) {}
        this.loadCachedAlbumThumb();
      } else {
        outPm.release();
      }
      return true;
    } catch (e) {
      return false;
    } finally {
      if (outPath) { try { fs.unlinkSync(outPath); } catch (_) {} }
      pm.release();
    }
  }

  // ★ AI 高清彩色修复入口：SR/上色模型懒加载 → 逐张修复入相册 → 汇总提示。
  //   修复 = 边缘保持去噪 + 黑白/棕褐照片 AI 上色（DDColor-L 真黑白上色）
  //   + AI 超分×4 高清 + 褪色修复（白平衡/对比度/饱和度），全部端侧本地推理
  private async startPhotoRestore(): Promise<void> {
    if (this.restoring) return;
    const uris = this.pendingRestoreUris;
    if (uris.length === 0) return;
    this.restoring = true;
    this.loading = true;
    let okCount = 0;
    try {
      // ① SR 超分模型懒加载（首次数秒；后续复用）
      this.loadingMsg = 'AI 修复模型加载中…';
      const srOk = await this.ensureSrModel();
      if (!srOk) {
        // 模型加载失败：仍继续（native 端跳过超分，仅去噪+上色+褪色修复）
        this.showToast('超分模型加载失败，本次仅去噪与色彩修复');
      }
      // ② 上色模型懒加载（DDColor-L 241MB，首次加载数秒；黑白/棕褐照片上色）
      this.loadingMsg = 'AI 上色模型加载中…';
      const colorOk = await this.ensureColorModel();
      if (!colorOk) {
        // 上色模型加载失败：仍继续（黑白照片本次仅去噪+超分+褪色修复）
        this.showToast('上色模型加载失败，黑白照片本次仅高清修复');
      }
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      for (let i = 0; i < uris.length; i++) {
        this.loadingMsg = `AI 高清彩色修复中… (${i + 1}/${uris.length})`;
        const ok = await this.restoreOnePhoto(ctx, uris[i], i === 0);
        if (ok) okCount++;
      }
      if (okCount > 0) {
        this.showToast(`已完成 ${okCount}/${uris.length} 张修复并保存到相册`);
        this.speak(`修复完成${okCount}张`);
        this.refreshAlbumThumb();
      } else {
        this.showToast('修复失败，请重试');
      }
      this.albumPreviewVisible = false;
      this.pendingRestoreUris = [];
    } finally {
      this.restoring = false;
      this.loading = false;
    }
  }

  private pad2(n: number): string { return n < 10 ? `0${n}` : `${n}`; }

  // ★ 水深历史：读数变化 ≥5cm 或距上次记录 ≥30s 记一条（时间 HH:mm:ss），最多保留 3 条
  private pushDepthRecord(cm: number): void {
    if (!(cm > 0)) return;
    const now = Date.now();
    const changed = this.lastDepthRecCm < 0 || Math.abs(cm - this.lastDepthRecCm) >= 5;
    if (!changed && now - this.lastDepthRecMs < 30000) return;
    this.lastDepthRecMs = now;
    this.lastDepthRecCm = cm;
    const d = new Date(now);
    const rec: DepthRecord = {
      cm: cm,
      time: `${this.pad2(d.getHours())}:${this.pad2(d.getMinutes())}:${this.pad2(d.getSeconds())}`
    };
    this.depthHistory = [rec].concat(this.depthHistory).slice(0, 3);
  }

  private showOcrPopup(): void {
    if (!this.ocrEnabled) { this.showToast('OCR 已关闭'); return; }
    this.ocrPopupVisible = true;
  }

  // ═══════ 表格结构识别（PP-Structure SLANet）═══════
  // native JSON 原始模型（一次转换，字段可选容错）
  private doRecognizeTable(): void {
    if (this.tableBusy) return;
    this.tableBusy = true;
    const seq = ++this.tableSeq;
    // ★ 识别全链路（模型加载+SLANet 推理+全图 OCR）最长约 20s：
    //   点击立即弹出 loading 面板给反馈，完成后面板原地切换为结果
    this.tableLoading = true;
    this.tablePopupVisible = true;
    this.tablePreviewPath = '';
    this.startTableElapsedTimer();
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    this.doRecognizeTableAsync(ctx, seq);
  }

  // ★ loading 面板已用时计时：每秒刷新提示，让用户知道没有死机
  private startTableElapsedTimer(): void {
    this.stopTableElapsedTimer();
    this.tableElapsedSec = 0;
    this.tableStage = '正在准备模型 · ';
    this.tableLoadingHint = `${this.tableStage}已用时 0 秒`;
    this.tableTimerId = setInterval((): void => {
      this.tableElapsedSec++;
      this.tableLoadingHint = `${this.tableStage}已用时 ${this.tableElapsedSec} 秒`;
    }, 1000);
  }

  private stopTableElapsedTimer(): void {
    if (this.tableTimerId !== -1) {
      clearInterval(this.tableTimerId);
      this.tableTimerId = -1;
    }
  }

  // ★ 放弃识别：识别中关闭弹窗 → 在途回调按 seq 静默失效（native 推理照常跑完，仅丢弃结果）
  private abandonTableRecognition(): void {
    this.tableSeq++;
    this.closeTablePopup();
  }

  private closeTablePopup(): void {
    this.tablePopupVisible = false;
    this.tableLoading = false;
    this.tablePreviewPath = '';
    this.stopTableElapsedTimer();
  }

  private async doRecognizeTableAsync(ctx: common.UIAbilityContext, seq: number): Promise<void> {
    try {
      // 保证 OCR 引擎就绪（单元格文本匹配依赖全图 OCR 文本行）
      const ocrOk = await this.ocr.initSet(ctx, this.ocrModelId);
      if (seq !== this.tableSeq) return;
      if (!ocrOk) {
        this.showToast('OCR 引擎初始化失败');
        this.closeTablePopup();
        return;
      }
      // ★ 表格识别依赖 OCR：OCR 未开启时点击"表格"自动开启（短按 OCR 徽标可再关闭）
      if (!this.ocrEnabled) {
        this.ocrEnabled = true;
        this.applyOcrParams();
        try { entry.setOcrEnabled(true); } catch (_) {}
        this.persistSettings();
        this.showToast('文字识别已自动开启（表格识别需要）');
      }
      // SLANet 表格模型（幂等，首次从 rawfile 解包；预览就绪后已后台预载）
      const tableOk = await this.ocr.initTable(ctx);
      if (seq !== this.tableSeq) return;
      if (!tableOk) {
        this.showToast('表格模型加载失败');
        this.closeTablePopup();
        return;
      }
      this.tableStage = '正在识别表格 · ';
      this.tableLoadingHint = `${this.tableStage}已用时 ${this.tableElapsedSec} 秒`;
      const json: string = await entry.recognizeTable(ctx.filesDir,
        (stage: number, path: string): void => { this.onTableEvent(stage, path, seq); });
      if (seq !== this.tableSeq) return;
      this.parseTableJson(json);
    } catch (_) {
      if (seq !== this.tableSeq) return;
      this.showToast('表格识别失败');
      this.closeTablePopup();
    } finally {
      // ★ 无条件复位：无论正常完成还是被放弃，识别链结束后都允许再次触发
      //   （放弃场景下 native 推理仍在跑，保持 busy 到回调结束避免并发识别）
      this.tableBusy = false;
    }
  }

  // ★ SLANet 表格模型后台预载（懒加载）：预览就绪后延时静默加载，
  //   点击"▦ 表格"时免付首次模型加载等待（initTable 幂等，预载与点击并发安全）
  private preloadTableModel(): void {
    setTimeout((): void => {
      try {
        const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
        this.ocr.initTable(ctx);
      } catch (_) {}
    }, 3000);
  }

  // ★ native 阶段事件回调（识别线程 → tsfn → JS 线程）：
  //   stage 1=采集帧 JPEG（loading 态立即显示被识别画面）
  //   stage 2=结构框标注图 JPEG（切"正在识别文字"阶段，图上绿框=SLANet 结构）
  private onTableEvent(stage: number, path: string, seq: number): void {
    if (seq !== this.tableSeq) return;
    if (stage === 1) {
      this.tablePreviewPath = path;
    } else if (stage === 2) {
      this.tablePreviewPath = path;
      this.tableStage = '正在识别文字 · ';
      this.tableLoadingHint = `${this.tableStage}已用时 ${this.tableElapsedSec} 秒`;
    }
  }

  private parseTableJson(json: string): void {
    try {
      const parsed = JSON.parse(json) as TableParsed;
      if (parsed.error !== undefined && parsed.error.length > 0) {
        this.showToast(parsed.error);
        this.closeTablePopup();
        return;
      }
      if (parsed.rows === undefined || parsed.cols === undefined ||
        parsed.cells === undefined) {
        this.showToast('表格识别失败');
        this.closeTablePopup();
        return;
      }
      const cells: TableCellData[] = [];
      const list: CellParsed[] = parsed.cells;
      for (let i = 0; i < list.length; i++) {
        const c = list[i];
        cells.push(new TableCellData(
          c.r ?? 0, c.c ?? 0, c.rs ?? 1, c.cs ?? 1, c.h ?? 0, c.t ?? ''));
      }
      this.tableRows = parsed.rows;
      this.tableCols = parsed.cols;
      this.tableCells = cells;
      this.tableHtml = parsed.html ?? '';
      this.tableElapsedMs = parsed.ms ?? 0;
      // 结构框标注图（事件回调已设置；此处兜底，确保结果态一定显示带框画面）
      if (parsed.boxed !== undefined && parsed.boxed.length > 0) {
        this.tablePreviewPath = parsed.boxed;
      }
      this.tablePopupVisible = true;
      // ★ 识别完成：loading 态原地切换为结果（弹窗保持打开，不闪断）
      this.tableLoading = false;
      this.stopTableElapsedTimer();
    } catch (_) {
      this.showToast('表格识别失败');
      this.closeTablePopup();
    }
  }

  private pollPreviewReady(): void {
    const check = (): void => {
      try {
        const s = entry.getFrameSize();
        if (s && s.length >= 2 && s[0] > 0 && s[1] > 0) {
          this.previewReady = true;
          // ★ 按实际帧宽高比适配预览区（USB 16:9 竖屏显示为横条不变形；手机摄像头按旋转后比例）
          this.fitPreviewToAspect(s[0] / s[1]);
          this.preloadTableModel();
          return;
        }
      } catch (_) {}
      setTimeout(check, 150);
    };
    setTimeout(check, 150);
  }

  // ★ 预览区适配内容宽高比：等比缩放居中，超出屏幕按高度收敛，杜绝拉伸变形
  //   折叠屏展开/折叠、旋转时由 display 'change' 监听按最近一次内容比例重算
  private previewAspect: number = 3 / 4;
  private displayChangeListener: ((displayId: number) => void) | null = null;
  // ★ 折叠开合监听（内外屏切换时 display 'change' 不可靠）
  private foldStatusListener: ((fs: display.FoldStatus) => void) | null = null;
  // ★ 当前已绑定的预览 surfaceId（折叠/展开后变化即需重建链路）
  private lastAttachedPreviewId: string = '';
  // ★ 折叠恢复去重时间戳（半折叠态连续触发防抖）
  private lastFoldRecoverMs: number = 0;

  // ★ 变焦上限：手机按 getZoomRatioRange 支持范围，USB 摄像头无参数默认最大 4 倍；
  //   超限时收敛当前倍率，避免滑条与实际能力不符
  private updateZoomMax(): void {
    if (this.cameraPosition === 2) {
      this.zoomMax = 4.0;
      this.hwZoomSupported = false;
    } else {
      let max = 4.0;
      let hw = false;
      try {
        const r = this.cameraService.getZoomRange();
        // ★ 硬件变焦范围必须 >1：前摄多为 [1,1]，setZoomRatio 无效果 → 走管线数字变焦
        if (r && r[1] > r[0] && r[1] > 1.001) { max = r[1]; hw = true; }
      } catch (_) {}
      this.zoomMax = max;
      this.hwZoomSupported = hw;
    }
    if (this.zoomRatio > this.zoomMax) {
      this.zoomRatio = this.zoomMax;
      this.applyZoomToCamera();
    }
  }

  // ★ 变焦路由：硬件变焦走 setZoomRatio；前摄/USB 等无硬件变焦走管线数字变焦（中心裁剪）
  private applyZoomToCamera(): void {
    try {
      if (this.hwZoomSupported) {
        // ★ 硬件变焦生效时必须把管线数字变焦复位 1x：
        //   前摄残留的 zoomScale_ 会与后摄硬件变焦叠加成双重放大
        entry.setUvcZoom(1.0);
        this.cameraService.setZoomRatio(this.zoomRatio);
      } else {
        entry.setUvcZoom(this.zoomRatio);
      }
    } catch (_) {}
  }

  private fitPreviewToAspect(aspect: number): void {
    if (!(aspect > 0.05 && aspect < 20)) return;
    this.previewAspect = aspect;
    try {
      const d = display.getDefaultDisplaySync();
      const vpw = d.width / d.densityPixels;
      const vph = d.height / d.densityPixels;
      // ★ 宽度优先撑满：预览始终占满屏幕宽度，纵向等比居中；
      //   任何内容比例（4:3/16:9/3:4）都不超出屏幕宽度，杜绝左右裁切（画面不全）。
      //   旧版"极宽内容 50% 屏高兜底"会把宽度撑出屏幕导致左右各裁一刀，已移除
      const w = vpw;
      const h = w / aspect;
      this.previewW = Math.round(w * 10) / 10;
      this.previewH = Math.round(h * 10) / 10;
      // ★ 位置水印可见性：预览在屏幕上垂直居中，底部可能被 BottomPanel 顶边遮挡。
      //   算出预览可见高度占帧高比例，传给 native 把水印条画在面板顶边之上；
      //   完全可见时 ratio=1（水印在帧最底部，照片/录像原样保留）
      const previewTop = (vph - this.previewH) / 2;
      const panelTop = vph - PANEL_OCCUPIED_VP;
      let visibleH = this.previewH;
      if (previewTop + this.previewH > panelTop) {
        visibleH = Math.max(this.previewH * 0.4, panelTop - previewTop);
      }
      let ratio = visibleH / this.previewH;
      if (ratio < 0.4) ratio = 0.4;
      if (ratio > 1) ratio = 1;
      try { entry.setLocationBottomRatio(ratio); } catch (_) {}
    } catch (_) {
      this.previewW = 0;
      this.previewH = 0;
    }
  }

  // ★ 重建预览链路统一入口：USB 走 UVC 重连，手机摄像头走 restoreCamera
  private restartPreviewChain(): void {
    if (this.cameraPosition === 2) {
      this.resumeUsbCamera();
      return;
    }
    this.restoreCamera(this.cameraPosition);
  }

  // ★ 折叠开合后预览恢复：XComponent surface 可能随内外屏切换重建（surfaceId 变化），
  //   相机输出与 native 管线仍绑旧 surface → 画面卡死在最后一帧；
  //   3 秒去重防止半折叠态连续触发多次重建
  private recoverPreviewAfterFold(): void {
    if (!this.started || this.switching || this.loading) return;
    const now = Date.now();
    if (now - this.lastFoldRecoverMs < 3000) return;
    this.lastFoldRecoverMs = now;
    this.restartPreviewChain();
  }

  // ★ display 变化（旋转等）：仅 surfaceId 真的变化才重建，避免多余相机重启
  private recoverPreviewOnSurfaceChanged(): void {
    if (!this.started || this.switching || this.loading) return;
    try {
      const previewId = this.xcController.getXComponentSurfaceId();
      if (this.lastAttachedPreviewId !== '' && previewId !== ''
        && previewId !== this.lastAttachedPreviewId) {
        this.lastFoldRecoverMs = Date.now();
        this.restartPreviewChain();
      }
    } catch (_) {}
  }

  // ★ 根据实际选中的相机设备设置渲染旋转与镜像：
  //   旋转 = CameraDevice.cameraOrientation（后摄通常 90、前摄通常 270，
  //          此前写死 90 导致前置摄像头画面 180° 反转）
  //   镜像 = 前置摄像头水平镜像（对齐 Android 前摄自拍镜像预览行为）
  private applyCameraRenderMode(): void {
    let orient = SENSOR_ROTATION;
    try { orient = this.cameraService.getActiveOrientation(); } catch (_) {}
    if (orient !== 0 && orient !== 90 && orient !== 180 && orient !== 270) orient = SENSOR_ROTATION;
    try { entry.setRotation(orient); } catch (_) {}
    let mirror = false;
    try { mirror = this.cameraService.isFrontCamera(); } catch (_) {}
    try { entry.setMirror(mirror); } catch (_) {}
  }

  // ★ 摄像头名称统一出口：所有提示/日志必须明确是哪个摄像头
  private camLabel(): string {
    if (this.cameraPosition === 2) return 'USB 摄像头';
    return this.cameraPosition === 1 ? '手机前置摄像头' : '手机后置摄像头';
  }

  // ★ USB 摄像头型号：USB 层枚举的产品名（如 "USB 2.0 Camera"），无则回退通用名
  private usbProductName(): string {
    try {
      const cams = UsbDeviceScanner.scan();
      if (cams.length > 0 && cams[0].productName.length > 0) return cams[0].productName;
    } catch (_) {}
    return 'USB 摄像头';
  }

  // ★ native 管线处理尺寸对齐实际选中的预览档位：createYuvReceiver 的 640x480
  //   只是占位（档位在 camera init 内部才确定）；不对齐则按 640x480 处理
  //   1440x1440/1920x1080 帧，UV 平面偏移错位 → 颜色错乱、录像内容错乱
  private syncPipelineFrameSize(): void {
    try {
      const sz = this.cameraService.getActivePreviewSize();
      if (sz && sz.length >= 2 && sz[0] > 0 && sz[1] > 0) {
        entry.updateFrameSize(sz[0], sz[1]);
        // ★ 变焦气泡旁展示：摄像头型号 + 实际预览分辨率
        const label = this.cameraPosition === 2 ? this.usbProductName() : this.cameraService.getActiveLabel();
        this.cameraInfo = `${label} ${sz[0]}×${sz[1]}`;
      }
    } catch (_) {}
  }

  // ★ 双指捏合缩放：硬件变焦支持的走 setZoomRatio（后摄），
  //   前摄/USB 走管线数字变焦（中心裁剪），范围 [1, zoomMax]
  private applyPinchZoom(v: number): void {
    const clamped = Math.min(Math.max(v, 1.0), this.zoomMax);
    if (Math.abs(clamped - this.zoomRatio) < 0.001) return;
    this.zoomRatio = clamped;
    try {
      if (this.hwZoomSupported) this.cameraService.setZoomRatio(clamped);
      else entry.setUvcZoom(clamped);
    } catch (e) {
      console.error(`[pinchZoom] zoom fail: ${JSON.stringify(e)}`);
    }
  }

  // ★ 释放相机带 3s 超时兜底：相机服务偶发卡死时 release 永不返回，
  //   会把 switching/releasingCamera 锁死 → 镜头切换/恢复永远黑屏；
  //   超时后放弃等待继续走后续重建流程（相机管理器重初始化是唯一出路）
  private async releaseCameraSafe(): Promise<void> {
    try {
      await Promise.race([
        this.cameraService.release(),
        new Promise<void>((_, reject) => setTimeout(() => reject(new Error('release timeout')), 3000))
      ]);
    } catch (_) {}
  }

  // ═══════════════ 手机摄像头生命周期（启动/切换/恢复/释放）═══════════════
  // startCamera：首次启动；selectCamera：用户切换（含 USB 列表/授权流）；
  // restoreCamera：异常后无条件重建；releaseCameraSafe：3s 超时兜底释放
  private async startCamera(rawDisplayId: string, previewDisplayId: string): Promise<void> {
    if (this.switching) return;
    this.switching = true;
    this.loading = true;
    this.previewReady = false;
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const aiId = entry.createYuvReceiver(640, 480);
      if (!aiId) { this.showToast('创建 YUV 接收器失败'); return; }
      try { entry.setRotation(SENSOR_ROTATION); } catch (_) {}
      entry.attachPreviewSurface(previewDisplayId);
      this.lastAttachedPreviewId = previewDisplayId;
      await Promise.race([
        this.cameraService.init(ctx, rawDisplayId, aiId, this.cameraPosition, 'ai'),
        new Promise<void>((_, reject) => setTimeout(() => reject(new Error('timeout')), 8000))
      ]);
      // ★ 管线尺寸对齐实际选中档位（高分辨率下必须，否则 UV 偏移错位颜色错乱）
      this.syncPipelineFrameSize();
      // ★ 按实际选中设备校正旋转与镜像（前摄 270° + 镜像）
      this.applyCameraRenderMode();
      // 按实际设备存在与否刷新 USB 状态（相机服务注册 或 USB 层枚举到 UVC 设备）
      this.usbConnected = UsbDeviceScanner.scan().length > 0
        || this.cameraService.getUsbState().getUsbCamera() !== null;
      // ★ 变焦按相机支持范围：手机用 getZoomRatioRange 上限，超限收敛当前倍率
      this.updateZoomMax();
      this.applyZoomToCamera();
      entry.startPipeline();
      this.started = true;
      this.vision.start();
      this.pollPreviewReady();
    } catch (e) {
      this.showToast(`${this.camLabel()}启动失败`);
    } finally {
      this.loading = false;
      this.switching = false;
    }
  }

  private async openGallery(): Promise<void> {
    const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
    const tries: Want[] = [
      { bundleName: 'com.huawei.hmos.photos', abilityName: 'com.huawei.hmos.photos.MainAbility' },
      { bundleName: 'com.huawei.hmos.photos', abilityName: 'com.huawei.hmos.photos.EntryAbility' },
      { bundleName: 'com.ohos.photos', abilityName: 'com.ohos.photos.MainAbility' },
    ];
    for (const want of tries) {
      try { await ctx.startAbility(want); return; } catch (e) {}
    }
  }

  private async captureSnapshot(): Promise<void> {
    let outPath: string = '';
    try {
      const abilityCtx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      if (!abilityCtx) return;
      let rawBuf: ArrayBuffer | null = null;
      let imgW = 0, imgH = 0;
      for (let attempt = 1; attempt <= 3; attempt++) {
        try {
          const b = entry.captureLatestFrame() as ArrayBuffer;
          const s = entry.getFrameSize() as number[];
          if (b && b.byteLength > 0 && s && s.length >= 2 && s[0] > 0 && s[1] > 0) {
            rawBuf = b; imgW = s[0]; imgH = s[1]; break;
          }
        } catch (e) { }
        if (attempt < 3) await new Promise<void>(r => setTimeout(r, 120));
      }
      if (!rawBuf || imgW <= 0 || imgH <= 0) { this.showToast('画面尚未就绪'); return; }
      const pixelMap = image.createPixelMapSync(rawBuf, {
        size: { width: imgW, height: imgH },
        srcPixelFormat: image.PixelMapFormat.RGBA_8888,
      });
      outPath = `${abilityCtx.cacheDir}/snapshot_${Date.now()}.jpg`;
      const file = fs.openSync(outPath, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const packer = image.createImagePacker();
      await packer.packToFile(pixelMap, file.fd, { format: 'image/jpeg', quality: 98 });
      fs.closeSync(file);
      try {
        const helper = photoAccessHelper.getPhotoAccessHelper(abilityCtx);
        const srcUri = fileUri.getUriFromPath(outPath);
        const req = photoAccessHelper.MediaAssetChangeRequest.createImageAssetRequest(abilityCtx, srcUri);
        await helper.applyChanges(req);
        this.showToast('截图已保存');
        this.speak('截图已保存');
        try { fs.copyFileSync(outPath, `${abilityCtx.filesDir}/album_thumb.jpg`); } catch (_) {}
        this.refreshAlbumThumb();
      } catch (e) {
        // ★ 保存授权过期（安全组件授权窗口短）：引导用户点底部"保存图片"重新授权
        this.showToast('截图保存失败，请点击底部"保存图片"后重试');
      }
    } catch (e) {} finally {
      if (outPath) { try { fs.unlinkSync(outPath); } catch (_) {} }
    }
  }

  private async selectCamera(pos: number): Promise<void> {
    // ★ 切换/加载期间禁止再选其他摄像头：防止状态与实际相机不一致
    if (this.switching || this.loading) return;
    if (pos === this.cameraPosition) return;
    if (pos === 2) {
      // 优先：相机服务已注册 USB 相机（CAMERA_CONNECTION_USB_PLUGIN）→ 直接切换
      const usbCam = this.cameraService.getUsbState().getUsbCamera();
      if (!usbCam) {
        // ★ 照抄 Android UsbManager.getDeviceList()：相机服务未注册时走 USB 层枚举 UVC 设备
        const cams = UsbDeviceScanner.scan();
        if (cams.length === 0) {
          // ★ 照抄 Android strings.xml cameranotworked
          this.showToast('尚未插入 USB 摄像头');
          return;
        }
        // 有 UVC 设备 → 弹设备列表（点选后走系统授权弹窗），照抄 Android 设备选择 Dialog
        this.usbDevices = cams;
        this.usbSheetVisible = true;
        return;
      }
    }
    const prevPos = this.cameraPosition;
    // ★ 从 USB 相机切走：停掉 UVC 取流与帧看门狗（否则后台持续占 USB 带宽）
    if (prevPos === 2) {
      this.stopUvcWatchdog();
      try { entry.uvcStopStreaming(); } catch (_) {}
      try { entry.uvcDisconnect(); } catch (_) {}
      this.usbPipeHandle = null;
    }
    this.cameraPosition = pos;
    this.switching = true;
    this.loading = true;
    this.loadingMsg = '正在切换摄像头…';
    this.previewReady = false;
    this.vision.stop();
    const rawId = this.rawCtrl.getXComponentSurfaceId();
    const previewId = this.xcController.getXComponentSurfaceId();
    if (!rawId || !previewId) { this.switching = false; this.loading = false; return; }
    try {
      try { entry.stopPipeline(); } catch (_) {}
      await this.releaseCameraSafe();
      const aiId = entry.createYuvReceiver(640, 480);
      if (aiId) {
        try { entry.setRotation(SENSOR_ROTATION); } catch (_) {}
        entry.attachPreviewSurface(previewId);
        this.lastAttachedPreviewId = previewId;
        const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
        await Promise.race([
          this.cameraService.switchTo(ctx, rawId, aiId, pos, 'ai'),
          new Promise<void>((_, reject) => setTimeout(() => reject(new Error('timeout')), 8000))
        ]);
        // ★ 管线尺寸对齐实际选中档位（高分辨率下必须，否则 UV 偏移错位颜色错乱）
        this.syncPipelineFrameSize();
        // ★ 按实际选中设备校正旋转与镜像（前摄 270° + 镜像）
        this.applyCameraRenderMode();
        this.usbConnected = UsbDeviceScanner.scan().length > 0
          || this.cameraService.getUsbState().getUsbCamera() !== null;
        // ★ 照抄 Android strings.xml openedsuccess
        if (pos === 2) this.showToast('USB 摄像头连接成功！');
        this.speak(`已切换到${this.camLabel()}`);
        this.updateZoomMax();
        this.applyZoomToCamera();
        entry.startPipeline();
        this.vision.start();
        this.pollPreviewReady();
        this.persistSettings();
      }
    } catch (e) {
      this.showToast(`${this.camLabel()}切换失败`);
      // ★ 失败回退到上一个正常摄像头：HUD 与设置保持一致，不留黑屏
      this.cameraPosition = prevPos;
      // ★ 先释放切换锁再恢复，否则 restoreCamera 会被 switching 挡住
      this.switching = false;
      await this.restoreCamera(prevPos);
    } finally {
      this.switching = false;
      this.loading = false;
    }
  }

  // ★ 无条件恢复 pos 摄像头（不复用 selectCamera 的 same-position 早退）：
  //   USB 连接失败/切换失败后，管线已停、相机已释放，必须完整重建预览链路
  private async restoreCamera(pos: number): Promise<void> {
    if (this.switching) return;
    this.switching = true;
    this.loading = true;
    this.loadingMsg = '正在恢复摄像头…';
    this.previewReady = false;
    this.vision.stop();
    const rawId = this.rawCtrl.getXComponentSurfaceId();
    const previewId = this.xcController.getXComponentSurfaceId();
    if (!rawId || !previewId) { this.switching = false; this.loading = false; return; }
    try {
      try { entry.stopPipeline(); } catch (_) {}
      await this.releaseCameraSafe();
      const aiId = entry.createYuvReceiver(640, 480);
      if (aiId) {
        try { entry.setRotation(SENSOR_ROTATION); } catch (_) {}
        entry.attachPreviewSurface(previewId);
        this.lastAttachedPreviewId = previewId;
        const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
        await Promise.race([
          this.cameraService.switchTo(ctx, rawId, aiId, pos, 'ai'),
          new Promise<void>((_, reject) => setTimeout(() => reject(new Error('timeout')), 8000))
        ]);
        // ★ 管线尺寸对齐实际选中档位（高分辨率下必须，否则 UV 偏移错位颜色错乱）
        this.syncPipelineFrameSize();
        this.applyCameraRenderMode();
        this.updateZoomMax();
        this.applyZoomToCamera();
        entry.startPipeline();
        this.vision.start();
        this.pollPreviewReady();
        this.persistSettings();
      }
    } catch (e) {
      this.showToast(`${this.camLabel()}恢复失败，请重启应用`);
    } finally {
      this.switching = false;
      this.loading = false;
    }
  }

  private async toggleRecord(): Promise<void> {
    if (this.isRecording) {
      // 手动停掉进行中的自动录像：进入冷却，防止立刻又触发
      if (this.autoRecording) {
        this.autoRecording = false;
        this.beginAutoRecordCooldown();
      }
      await this.endRecording();
      return;
    }
    // ★ 与自动录像同款信息展示：开始提示 + 录像计时浮窗（isRecording 驱动）
    this.showToast('开始录像');
    this.speak('开始录像');
    await this.beginRecording(0);
  }

  // ═══════════════ USB/UVC 外接摄像头（授权→fd 注入→libuvc 取流→看门狗）═══════════════
  // 插拔双通道监听（相机服务 cameraStatus + USB 公共事件）→ notifyUsbAttached/Detached；
  // 连接：requestRight 授权 → openPipe 拿 fd → uvcConnect → uvcStartStreaming；
  // 保障：5s/10s 帧看门狗（无帧/全解码失败提示并回退）、UVC 诊断日志落盘
  // ★ 照抄 Android CameraFragment.onAttachDev：插入即申请权限并自动连接
  //   （相机服务 cameraStatus 与 USB 层公共事件可能都触发，2.5s 去重防止重复连接）
  private notifyUsbAttached(): void {
    const cams = UsbDeviceScanner.scan();
    this.usbConnected = cams.length > 0 || this.cameraService.getUsbState().getUsbCamera() !== null;
    if (cams.length > 0 && Date.now() - this.lastUsbNotifyMs > 2500) {
      this.lastUsbNotifyMs = Date.now();
      this.appendUvcLog('usb attached, auto connect');
      this.connectUsbDevice(cams[0]);
    }
  }

  private notifyUsbDetached(): void {
    const cams = UsbDeviceScanner.scan();
    const svcHas = this.cameraService.getUsbState().getUsbCamera() !== null;
    this.usbConnected = cams.length > 0 || svcHas;
    this.usbPipeHandle = null;
    // ★ USB 拔出后帧永远不会再来，先停看门狗再回退
    this.stopUvcWatchdog();
    // ★ 正在切换/加载时不并发重入；空闲时 USB 拔出才回退后置
    if (this.cameraPosition === 2 && !svcHas && cams.length === 0
      && !this.switching && !this.loading) {
      this.showToast('USB 摄像头已断开');
      this.selectCamera(0);
    }
  }

  // UVC 诊断日志：写入应用沙箱 files/uvc_debug.log。
  // 插拔 USB 会断 hdc，hilog 现场易丢；落盘后可在重连 PC 后取回分析
  private appendUvcLog(msg: string): void {
    // ★ 分步定位 EBADF(13900008)：每步带步骤号，失败信息含 code/message
    let step = 'getHostContext';
    try {
      const ctx = this.getUIContext().getHostContext() as common.UIAbilityContext;
      step = 'filesDir';
      const dir: string = ctx.filesDir;
      const line = `${new Date().toISOString()} ${msg}\n`;
      step = `openSync dir=${dir}`;
      // ★ 官方 FAQ 写法：必须带 READ_WRITE，否则 fd 只读，writeSync 报 EBADF(13900008)
      const file = fs.openSync(`${dir}/uvc_debug.log`,
        fs.OpenMode.READ_WRITE | fs.OpenMode.APPEND | fs.OpenMode.CREATE);
      step = `writeSync fd=${file.fd}`;
      fs.writeSync(file.fd, line);
      step = 'closeSync';
      fs.closeSync(file);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`appendUvcLog FAILED @step=${step}, msg=${msg}, ` +
        `code=${err?.code}, name=${err?.name}, message=${err?.message}`);
    }
  }

  // ★ USB 帧看门狗：连接成功 ≠ 有画面（接触不良/带宽不足时帧数为 0）。
  //   5s 无帧 → 明确提示检查接触；10s 仍无帧 → 断开并回退原摄像头，杜绝无信息的黑屏。
  //   有帧后转为断流监测（帧数停增 15s 提示）与解码失败监测（帧到达但全解码失败同样黑屏）
  private startUvcWatchdog(prevPos: number): void {
    this.stopUvcWatchdog();
    let noFrameChecks = 0;
    let lastCount = 0;
    let stallChecks = 0;
    let firstTick = true;
    this.uvcWatchdogTimer = setInterval((): void => {
      let frames = 0;
      let decodeFail = 0;
      try {
        frames = entry.uvcGetFrameCount();
        decodeFail = entry.uvcGetDecodeFailCount();
      } catch (_) { return; }
      // ★ 首个 tick 落盘首帧诊断（帧描述尺寸 vs JPEG 真实尺寸 vs actual_bytes + 解码返回码）：
      //   黑屏时一次定位是空帧/尺寸不符/数据损坏
      if (firstTick) {
        firstTick = false;
        try { this.appendUvcLog(`frame info: ${entry.uvcGetFrameInfo()}`); } catch (_) {}
      }
      if (frames === 0) {
        noFrameChecks++;
        if (noFrameChecks === 1) {
          this.appendUvcLog('watchdog: 5s 无帧，提示检查接触');
          this.showToast('USB 摄像头已连接但未收到画面，请检查接触是否良好');
        } else {
          this.appendUvcLog(`watchdog: ${noFrameChecks * 5}s 无帧，断开并回退原摄像头`);
          this.stopUvcWatchdog();
          this.showToast('USB 摄像头无画面输出，已切回原摄像头');
          try { entry.uvcStopStreaming(); } catch (_) {}
          try { entry.uvcDisconnect(); } catch (_) {}
          this.usbPipeHandle = null;
          this.cameraPosition = prevPos;
          this.restoreCamera(prevPos);
        }
        return;
      }
      // 帧在到达但全部解码失败：画面同样黑，明确提示而非黑屏无信息
      if (decodeFail >= frames) {
        this.appendUvcLog(`watchdog: ${frames} 帧全部解码失败，断开并回退原摄像头`);
        this.stopUvcWatchdog();
        this.showToast('USB 摄像头画面解码失败，已切回原摄像头');
        try { entry.uvcStopStreaming(); } catch (_) {}
        try { entry.uvcDisconnect(); } catch (_) {}
        this.usbPipeHandle = null;
        this.cameraPosition = prevPos;
        this.restoreCamera(prevPos);
        return;
      }
      if (frames === lastCount) {
        stallChecks++;
        if (stallChecks >= 3) {
          stallChecks = 0;
          this.appendUvcLog(`watchdog: 帧停滞在 ${frames}（15s 无新帧），提示检查接触`);
          this.showToast('USB 摄像头画面已中断，请检查接触');
        }
      } else {
        stallChecks = 0;
        lastCount = frames;
      }
    }, 5000);
  }

  private stopUvcWatchdog(): void {
    if (this.uvcWatchdogTimer >= 0) {
      clearInterval(this.uvcWatchdogTimer);
      this.uvcWatchdogTimer = -1;
    }
  }

  // ★ 照抄 Android：设备列表点选 → requestRight 系统授权弹窗 → libuvc fd 注入连接
  //   （等价 UVCCamera.connect：uvc_init2 → uvc_get_device_with_fd → uvc_open）
  private async connectUsbDevice(dev: UsbCamInfo): Promise<void> {
    this.usbSheetVisible = false;
    // ★ 回退目标：连接失败/无帧回退时恢复的原摄像头位置
    //   （息屏/回后台恢复场景 cameraPosition 已是 2：USB 连不上应回退后置而非再试 USB）
    const prevPos = this.cameraPosition === 2 ? 0 : this.cameraPosition;
    this.appendUvcLog(`connect start: name=${dev.name} vid=0x${dev.vendorId.toString(16)} ` +
      `pid=0x${dev.productId.toString(16)} bus=${dev.busNum} addr=${dev.devAddress} hasRight=${dev.hasRight}`);
    // 系统弹窗申请 USB 设备访问权限（等价 Android UsbManager.requestPermission 弹窗）
    if (!dev.hasRight) {
      const granted = await UsbDeviceScanner.requestRight(dev.name);
      this.appendUvcLog(`requestRight: ${granted}`);
      if (!granted) { this.showToast('USB 摄像头授权失败'); return; }
    }
    this.loading = true;
    this.loadingMsg = '正在连接 USB 摄像头…';
    this.previewReady = false;
    this.vision.stop();
    try { entry.stopPipeline(); } catch (_) {}
    // ★ 带 3s 超时释放：USB 连接路径相机服务卡死时不能无限挂起（连接失败回退也走得到这里）
    await this.releaseCameraSafe();

    // ★ 等价 Android UsbManager.openDevice + getFileDescriptor：
    //   connectDevice 拿 pipe → getFileDescriptor 把 usbfs fd dup 进本进程
    const handle = UsbDeviceScanner.openPipe(dev.name);
    if (!handle) {
      this.appendUvcLog('openPipe FAILED (connectDevice/getFileDescriptor 异常或 fd<=0)');
      this.showToast('USB 摄像头打开失败');
      // ★ 回退到上一个正常摄像头（selectCamera 同位置会早退，必须走 restoreCamera）
      await this.restoreCamera(this.cameraPosition);
      return;
    }
    this.usbPipeHandle = handle;
    this.appendUvcLog(`openPipe ok: fd=${handle.fd}`);

    // ★ 外部供帧管线（无 ImageReceiver，帧由 native uvc 回调注入）
    if (!entry.setupExternalPipeline(640, 480)) {
      this.appendUvcLog('setupExternalPipeline FAILED');
      this.showToast('预览管线创建失败');
      await this.restoreCamera(this.cameraPosition);
      return;
    }
    // ★ 变焦气泡旁先占位，取流成功后按实际协商分辨率刷新
    this.cameraInfo = '连接中…';
    // ★ UVC 相机：外部供帧已正立，关闭前置摄像头遗留的镜像
    try { entry.setMirror(false); } catch (_) {}
    const previewId = this.xcController.getXComponentSurfaceId();
    if (previewId) {
      entry.attachPreviewSurface(previewId);
      this.lastAttachedPreviewId = previewId;
    }

    // ★ fd 注入 libuvc：uvc_init2 → uvc_get_device_with_fd(vid,pid,fd,bus,addr) → uvc_open
    if (!entry.uvcConnect(handle.fd, dev.vendorId, dev.productId, dev.busNum, dev.devAddress)) {
      const detail = entry.uvcGetLastError();
      this.appendUvcLog(`uvcConnect FAILED: ${detail}`);
      this.showToast(`连接失败：${detail}`);
      await this.restoreCamera(this.cameraPosition);
      return;
    }
    this.appendUvcLog('uvcConnect ok');
    // ★ 落盘设备上报的格式/分辨率清单（诊断协商 -51 INVALID_MODE）
    try { this.appendUvcLog(`dev formats: ${entry.uvcDumpFormats()}`); } catch (_) {}
    // ★ native 端 NegotiateMax 直接上设备最高分辨率（MJPEG 优先、≥15fps 优先）；
    //   640×480 仅作为协商失败时的固定参数回退值
    if (!entry.uvcStartStreaming(640, 480, 30)) {
      const detail = entry.uvcGetLastError();
      entry.uvcDisconnect();
      this.appendUvcLog(`uvcStartStreaming FAILED: ${detail}`);
      this.showToast(`USB 摄像头取流失败：${detail}`);
      await this.restoreCamera(this.cameraPosition);
      return;
    }
    this.appendUvcLog('uvcStartStreaming ok');
    // ★ native 已直接上设备最高分辨率（NegotiateMax，MJPEG 优先、≥15fps 优先），
    //   读回实际帧尺寸刷新展示与预览比例
    let negW = 0;
    let negH = 0;
    try {
      const sz = entry.getFrameSize();
      if (sz && sz.length >= 2 && sz[0] > 0 && sz[1] > 0) {
        negW = sz[0];
        negH = sz[1];
        this.appendUvcLog(`negotiated size: ${negW}x${negH}`);
      }
    } catch (_) {}

    this.cameraPosition = 2;
    this.usbConnected = true;
    // ★ USB 摄像头展示型号+实际协商分辨率（negotiate 失败时回退 640×480 提示）
    this.cameraInfo = negW > 0
      ? `${this.usbProductName()} ${negW}×${negH}`
      : `${this.usbProductName()} 640×480`;
    // ★ USB 摄像头无变焦参数：默认最大 4 倍；重连/恢复后重新应用已保存的数字变焦
    //   （照抄 Android KEY_ZOOM_SCALE 恢复，updateZoomMax 内含超限收敛）
    this.updateZoomMax();
    this.persistSettings();
    // ★ 照抄 Android strings.xml openedsuccess
    this.showToast('摄像头连接成功！');
    this.started = true;
    this.vision.start();
    this.pollPreviewReady();
    this.loading = false;
    // ★ 帧看门狗：连接成功后 5s/10s 检查帧数，无帧/全解码失败时明确提示并回退 prevPos
    this.startUvcWatchdog(prevPos);
  }

  // ═══════════════ 生命周期：aboutToDisappear（页面销毁清理）═══════════════
  // 清全部定时器/监听/轮询 → TTS shutdown → 录像收尾 → 带超时释放相机与音效资源
  aboutToDisappear(): void {
    this.vision.stop();
    this.previewReady = false;
    this.stopRecordTimer();
    this.stopUvcWatchdog();
    this.stopTableElapsedTimer();
    if (this.batteryTimer !== -1) { clearInterval(this.batteryTimer); this.batteryTimer = -1; }
    if (this.geoTimer !== -1) { clearTimeout(this.geoTimer); this.geoTimer = -1; }
    this.stopFaceInfoPoll();
    try {
      if (this.displayChangeListener) display.off('change', this.displayChangeListener);
    } catch (_) {}
    this.displayChangeListener = null;
    // ★ 折叠开合监听注销
    try {
      if (this.foldStatusListener) display.off('foldStatusChange', this.foldStatusListener);
    } catch (_) {}
    this.foldStatusListener = null;
    this.usbScanner.unsubscribe();
    // ★ TTS 引擎释放：进程内引擎名额有限，退出页面必须 shutdown
    this.speech.shutdown();
    const self = this;
    (async (): Promise<void> => {
      try { if (self.isRecording) entry.detachRecordSurface(); } catch (_) {}
      try { if (self.isRecording) await self.media.recorder.stop(); } catch (_) {}
      // ★ 带 3s 超时释放：页面销毁时相机服务卡死会让相机一直被占用（下次启动黑屏）
      await self.releaseCameraSafe();
      try { self.cameraService.releaseAll(); } catch (_) {}
      try { await self.alertService.release(); } catch (_) {}
      try { entry.stopPipeline(); } catch (_) {}
    })();
  }

  // ═══════════════ build()：全屏 Stack 层级结构 ═══════════════
  // 自下而上叠放顺序（z 轴从低到高）：
  //   1. cam_raw XComponent：相机原始输出层（不可见，仅提供 raw surface，随旋转角转动）
  //   2. preview Stack：preview XComponent（native 推流的显示层，按内容比例适配尺寸）
  //      + 触摸覆盖层（点击命中人脸框 / 双指捏合变焦）+ 未就绪黑遮罩
  //   3. TopHudView：顶部 HUD 徽标条（识别/OCR/电量/人脸行…，回调全部转发本页方法）
  //   4. 人脸详情弹层（遮罩 + 全屏箭头 Canvas + 详情面板）
  //   5. BottomPanel：底部控制面板（录像/截图/识别/水深/运动/相册/变焦滑条）
  //   6. 录像计时浮窗 RecTimerPopup / loading 遮罩 LoadingOverlay
  //   7. 各功能弹窗（识别设置/画质/OCR/表格结果/运动/水深/记录/能耗/位置/USB 列表）
  build() {
    Stack({ alignContent: Alignment.Center }) {
      Stack() {
        XComponent({ id: 'cam_raw', type: XComponentType.SURFACE, controller: this.rawCtrl })
          .width('100%').height('100%')
      }
      .width('100%').height('100%').opacity(0).hitTestBehavior(HitTestMode.None).rotate({ angle: this.rotateAngle })

      Stack({ alignContent: Alignment.Center }) {
        XComponent({ id: 'preview', type: XComponentType.SURFACE, controller: this.xcController })
          // ★ 按实际内容宽高比适配（fitPreviewToAspect 计算）：
          //   USB 16:9 → 全宽横条；手机摄像头 3:4 → 竖向适配，均不拉伸变形
          .width(this.previewW > 0 ? this.previewW : '100%')
          .height(this.previewH > 0 ? this.previewH : '100%')
          .onLoad(() => {
            setTimeout(() => {
              const rawId = this.rawCtrl.getXComponentSurfaceId();
              const previewId = this.xcController.getXComponentSurfaceId();
              if (rawId && previewId) this.startOrRestoreCamera(rawId, previewId);
            }, 200);
          })
        // ★ 触摸覆盖层：承载点击（命中人脸识别框 → 弹详情面板）与双指捏合
        //   缩放。真正的触摸失效根因是 TopHudView 全屏容器 Default 命中阻塞
        //   了低 z 兄弟（已改 Transparent）；覆盖层与 XComponent 同在居中
        //   Stack 内同尺寸 → ClickEvent.x/y 即预览本地坐标，handleFaceFrameTap
        //   直接可用；埋点保留供回捞日志验证
        Column()
          .width(this.previewW > 0 ? this.previewW : '100%')
          .height(this.previewH > 0 ? this.previewH : '100%')
          .onClick((event: ClickEvent): void => {
            this.appendUvcLog(`[tap] click x=${event.x.toFixed(1)} y=${event.y.toFixed(1)}`);
            this.handleFaceFrameTap(event.x, event.y);
          })
          .gesture(PinchGesture({ fingers: 2 })
            .onActionStart((event: GestureEvent) => {
              this.pinchStartZoom = this.zoomRatio;
              this.appendUvcLog(`[pinch] start zoom=${this.zoomRatio.toFixed(2)} hw=${this.hwZoomSupported}`);
            })
            .onActionUpdate((event: GestureEvent) => {
              this.applyPinchZoom(this.pinchStartZoom * event.scale);
            })
            .onActionEnd(() => {
              this.persistSettings();
            }))
        if (!this.previewReady) {
          Column().width('100%').height('100%').backgroundColor('#000').justifyContent(FlexAlign.Center).hitTestBehavior(HitTestMode.Transparent)
        }
      }
      .width('100%').height('100%').rotate({ angle: this.rotateAngle })

      TopHudView({
        cameraPosition: this.cameraPosition,
        usbConnected: this.usbConnected,
        yoloEnabled: this.yoloEnabled,
        ocrEnabled: this.ocrEnabled,
        hudOpacity: this.hudOpacity,
        // ★ HUD 顶部偏移：状态栏安全区高度+间距（不遮挡系统信号/电量栏）
        hudTopOffset: this.hudTopOffset,
        mergedList: this.mergedList,
        motionLevel: this.motionLevel,
        motionEnabled: this.motionEnabled,
        motionSens: this.motionSens,
        depthEnabled: this.depthEnabled,
        depthCm: this.depthCm,
        depthHistory: this.depthHistory,
        modelName: this.modelId,
        confThresh: this.confThresh,
        // ★ 电池电量：置信度旁显示（🔋xx%，充电中 ⚡xx%）
        batterySoc: this.batterySoc,
        batteryCharging: this.batteryCharging,
        ocrText: this.ocrText,
        ocrDone: this.ocrDone,
        ocrTotal: this.ocrTotal,
        isRecording: this.isRecording,
        recordSec: this.recordSec,
        recBlink: this.recBlink,
        busy: this.switching || this.loading,
        powerMode: this.powerMode,
        ocrModelName: this.shortOcrSetName(),
        faceEnabled: this.faceEnabled,
        faceHudRows: this.faceHudRows,
        // ★ 画面徽标按增强模式变色区分（0关/1非AI/2 AI）
        usbEnhanceMode: this.usbEnhanceMode,
        onFaceDetail: (tid: number): void => { this.openFaceDetail(tid); },
        onCameraSelect: (pos: number): void => { this.selectCamera(pos); },
        // ★ 画面设置胶囊：USB 增强/识别框/人脸识别统一入口
        onShowPictureSheet: (): void => { this.pictureSheetVisible = true; },
        onYoloToggle: (): void => { this.toggleYolo(); },
        // ★ YOLO 长按：弹出与底部"识别"长按相同的识别设置菜单
        onYoloLongPress: (): void => { this.sheetVisible = true; },
        onOcrToggle: (): void => { this.toggleOcr(); },
        onOcrLongPress: (): void => { this.ocrSheetVisible = true; },
        onShowPowerSheet: (): void => { this.powerSheetVisible = true; },
        onOpacityChange: (v: number): void => { this.hudOpacity = v; this.persistSettings(); },
        onShowOcrPopup: (): void => { this.showOcrPopup(); },
        // ★ 表格识别：点击"▦ 表格"徽标对当前画面做一次 PP-Structure 表格结构识别
        tableBusy: this.tableBusy,
        onRecognizeTable: (): void => { this.doRecognizeTable(); }
      })

      // ★ 人脸详情弹层（点 HUD 人脸行"详细"胶囊 / 点预览识别框弹出）：
      //   ① 半透明遮罩点空白关闭（不挡 HUD/底部面板：本层位于两者之间）
      //   ② 全屏箭头 Canvas（面板 → 人脸框顶中心，颜色与人脸连线框一致，250ms 实时刷新）
      //   ③ 详情面板（头像 + 性别/年龄/颜值 + 7 分项条 + 复制/关闭胶囊）
      if (this.faceDetailVisible) {
        Column()
          .width('100%').height('100%')
          .backgroundColor('rgba(0,0,0,0.22)')
          .onClick((): void => { this.closeFaceDetail(); })

        Canvas(this.faceArrowCtx)
          .width('100%').height('100%')
          .hitTestBehavior(HitTestMode.None)
          .onReady((): void => { this.drawFaceArrow(); })

        Column({ space: 8 }) {
          // ★ 头部：人脸头像（识别框实裁小图，含连线框）+ 标题/摘要 + 关闭
          Row({ space: 10 }) {
            if (this.faceDetailAvatar !== null) {
              Image(this.faceDetailAvatar)
                .width(88).height(88)
                .borderRadius(12)
                .objectFit(ImageFit.Cover)
                .border({ width: 1.5, color: faceColorOfTid(this.faceDetailTid, 0) })
                .backgroundColor('#0B0E12')
            } else {
              Column() {
                Text('☺').fontSize(30).fontColor(faceColorOfTid(this.faceDetailTid, 0))
              }
              .width(88).height(88)
              .borderRadius(12)
              .justifyContent(FlexAlign.Center)
              .backgroundColor('#0B0E12')
              .border({ width: 1.5, color: faceColorOfTid(this.faceDetailTid, 0) })
            }
            Column({ space: 4 }) {
              Text(this.faceDetailTid > 0 ? `人脸详情 · ${this.faceDetailTid}` : '人脸详情')
                .fontSize(12).fontColor('#8E97A3').fontWeight(FontWeight.Bold)
              if (this.detailFace() === null) {
                Text('人脸已离开画面')
                  .fontSize(14).fontColor('#8E97A3').fontWeight(FontWeight.Bold)
              } else {
                Text(this.detailSummary())
                  .fontSize(17).fontColor(faceColorOfTid(this.faceDetailTid, 0))
                  .fontWeight(FontWeight.Bold)
                Row({ space: 8 }) {
                  if (this.detailBeautyText().length > 0) {
                    Text(this.detailBeautyText())
                      .fontSize(13).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
                  }
                }.width('100%')
              }
            }.layoutWeight(1)

            Text('✕').fontSize(13).fontColor('#8E97A3')
              .padding({ left: 6, right: 6, top: 4, bottom: 4 })
              .onClick((): void => { this.closeFaceDetail(); })
          }.width('100%').alignItems(VerticalAlign.Center)

          if (this.detailFace() !== null) {
            Divider().color('rgba(255,255,255,0.12)')

            Column({ space: 6 }) {
              ForEach(this.detailCompList(), (c: FaceCompBar) => {
                Row({ space: 6 }) {
                  Text(c.name).fontSize(10).fontColor('#C7CDD6').width(28)
                  Stack({ alignContent: Alignment.Start }) {
                    Row().width('100%').height(5).borderRadius(2.5)
                      .backgroundColor('rgba(255,255,255,0.12)')
                    Row().width(`${Math.max(2, Math.min(100, c.v))}%`).height(5).borderRadius(2.5)
                      .backgroundColor(this.compBarColor(c.v))
                  }.layoutWeight(1)
                  Text(`${c.v}`).fontSize(10).fontColor(this.compBarColor(c.v))
                    .fontWeight(FontWeight.Bold).width(24).textAlign(TextAlign.End)
                }.width('100%').alignItems(VerticalAlign.Center)
              }, (c: FaceCompBar) => `${c.name}_${c.v}`)
            }.width('100%')

            // ★ 胶囊按钮：加大加高（高 34、全圆角），复制=人脸主色底
            Row({ space: 10 }) {
              Button('复制信息')
                .fontSize(13).fontColor('#003B44')
                .backgroundColor(faceColorOfTid(this.faceDetailTid, 0))
                .borderRadius(17).height(34).layoutWeight(1)
                .onClick((): void => { this.copyFaceInfo(); })
              Button('关闭')
                .fontSize(13).fontColor('#FFFFFF').backgroundColor('rgba(255,255,255,0.14)')
                .borderRadius(17).height(34).layoutWeight(1)
                .onClick((): void => { this.closeFaceDetail(); })
            }.width('100%')
          }
        }
        .width(262)
        .padding({ left: 14, right: 14, top: 12, bottom: 12 })
        .backgroundColor('#16191F')
        .borderRadius(16)
        .border({ width: 1, color: faceColorOfTid(this.faceDetailTid, 0) })
        .position({ x: '50%', y: this.faceDetailPanelY() })
        .translate({ x: '-50%' })
        .onAreaChange((_old: Area, newArea: Area): void => {
          // Area.height 类型是 Length（number|string），只接受数值
          const h = newArea.height;
          if (typeof h === 'number') {
            this.faceDetailPanelH = h;
          }
          this.drawFaceArrow();
        })
      }

      BottomPanel({
        isRecording: $isRecording,
        zoomRatio: $zoomRatio,
        albumThumb: $albumThumb,
        rotateText: this.rotateText,
        zoomMax: this.zoomMax,
        cameraInfo: this.cameraInfo,
        yoloEnabled: $yoloEnabled,
        motionEnabled: $motionEnabled,
        depthEnabled: $depthEnabled,
        currentMode: this.currentMode,
        recordSec: this.recordSec,
        autoRecordOn: this.autoRecordOn,
        // ★ 位置行（移自顶部 HUD）：置于变焦气泡行上方
        geoName: this.geoPlaceName,
        onShowGeoPopup: (): void => { this.geoPopupVisible = true; },
        onToggleRecord: (): void => { this.toggleRecord(); },
        // ★ 记录按钮与识别/水深/运动一致：短按=开关自动记录，长按=弹记录设置菜单
        onAutoRecordToggle: (): void => { this.toggleAutoRecord(); },
        onAutoRecordLongPress: (): void => { this.recordSheetVisible = true; },
        onSnapshot: async (): Promise<void> => {
          // ★ 保存图片=截图模式：圆圈文字跟随显示"截图"
          this.currentMode = '截图';
          this.loading = true; this.loadingMsg = '正在保存截图…';
          await this.captureSnapshot();
          this.loading = false;
        },
        onRotate: (): void => {
          this.rotateAngle = (this.rotateAngle + 90) % 360;
          if (this.rotateAngle === 0) this.rotateText = '水平';
          else if (this.rotateAngle === 90) this.rotateText = '垂直';
          else if (this.rotateAngle === 180) this.rotateText = '反转';
          else this.rotateText = '反向';
          // ★ 水平按钮文字变化时圆圈文字同步跟随
          this.currentMode = this.rotateText;
          this.persistSettings();
        },
        // ★ 照抄 Android：识别/水深/运动 短按=切换开关，长按=弹出配置菜单
        onYoloToggle: (): void => { this.currentMode = '识别'; this.toggleYolo(); },
        onYoloLongPress: (): void => { this.sheetVisible = true; },
        onDepthToggle: (): void => { this.currentMode = '水深'; this.toggleDepth(); },
        onDepthLongPress: (): void => { this.depthSheetVisible = true; },
        onMotionToggle: (): void => { this.currentMode = '运动'; this.toggleMotion(); },
        onMotionLongPress: (): void => { this.motionSheetVisible = true; },
        onAlbumPreview: (): void => { this.openAlbumPicker(); },
        onShowToast: (msg: string): void => { this.showToast(msg); },
        onZoomChange: (v: number): void => {
          this.zoomRatio = v;
          this.applyZoomToCamera();
          this.persistSettings();
        }
      })

      // ★ 录像计时浮窗（照抄 Android recTimerLayout：红点+00:00 白字 20sp，
      //   浮于控制面板上方居中，录像时显示）
      if (this.isRecording) {
        Stack({ alignContent: Alignment.Bottom }) {
          RecTimerPopup({ recordSec: this.recordSec })
            .margin({ bottom: 200 })
        }
        .width('100%')
        .height('100%')
        .hitTestBehavior(HitTestMode.Transparent)
      }

      // ★ loading 期间与首帧未就绪的间隙统一用 loading.gif 遮罩（纯 gif，无文案）：
      //   此前 previewReady=false 且 loading=false 时（切换/连接刚完成、首帧未到）
      //   只剩纯黑遮罩，看起来像"没用 gif 的黑屏 loading"
      LoadingOverlay({
        visible: this.loading || (!this.previewReady && !this.pageHidden)
      })

      // ───────── 弹窗层：功能设置与结果展示（互不叠加，各自 visible 独立）─────────
      if (this.sheetVisible) {
        ModelSheet({
          models: this.models.getModels(),
          currentModelId: this.modelId,
          labels: this.modelLabels,
          checkedLabels: this.alertLabels,
          confThresh: this.confThresh,
          yoloEnabled: this.yoloEnabled,
          speechOn: this.speechOn,
          speechVolume: this.speechVolume,
          detectSpeakOn: this.detectSpeakOn,
          recordSoundOn: this.recordSoundOn,
          recordSoundName: this.recordSoundName,
          onClose: (): void => { this.sheetVisible = false; },
          onSelectModel: (id: string): void => { this.selectModel(id); },
          onToggleLabel: (idx: number): void => { this.toggleAlertLabel(idx); },
          onConfChange: (v: number): void => { this.applyConfThresh(v); },
          onYoloToggle: (): void => { this.toggleYolo(); },
          onSpeechToggle: (): void => { this.toggleSpeech(); },
          onSpeechVolumeChange: (v: number): void => { this.onSpeechVolumeChange(v); },
          onDetectSpeakToggle: (): void => { this.toggleDetectSpeak(); },
          onSoundToggle: (): void => { this.toggleRecordSound(); },
          onPickSound: (): void => { this.pickRecordSound(); },
          onPreviewSound: (): void => { this.previewRecordSound(); },
          onResetSound: (): void => { this.resetRecordSound(); }
        })
      }

      // ★ 画面设置弹窗：识别框显示 + USB 增强（亮度/锐度/对比度/饱和度/AI强度）+ 人脸识别
      if (this.pictureSheetVisible) {
        PictureSheet({
          drawBoxesOn: this.drawBoxesOn,
          usbBrightness: this.usbBrightness,
          usbSharpness: this.usbSharpness,
          usbContrast: this.usbContrast,
          usbSaturation: this.usbSaturation,
          usbEnhanceMode: this.usbEnhanceMode,
          usbSrStrength: this.usbSrStrength,
          aiDenoise: this.aiDenoise,
          aiClarity: this.aiClarity,
          aiColorRestore: this.aiColorRestore,
          faceEnabled: this.faceEnabled,
          onClose: (): void => { this.pictureSheetVisible = false; },
          onDrawBoxesToggle: (): void => { this.toggleDrawBoxes(); },
          onUsbModeChange: (m: number): void => { this.onUsbModeChange(m); },
          onUsbBrightnessChange: (v: number): void => { this.onUsbBrightnessChange(v); },
          onUsbSharpnessChange: (v: number): void => { this.onUsbSharpnessChange(v); },
          onUsbContrastChange: (v: number): void => { this.onUsbContrastChange(v); },
          onUsbSaturationChange: (v: number): void => { this.onUsbSaturationChange(v); },
          onUsbSrStrengthChange: (v: number): void => { this.onUsbSrStrengthChange(v); },
          onUsbResetDefaults: (): void => { this.onUsbResetDefaults(); },
          onAiDenoiseChange: (v: number): void => { this.onAiDenoiseChange(v); },
          onAiClarityChange: (v: number): void => { this.onAiClarityChange(v); },
          onAiColorRestoreChange: (v: number): void => { this.onAiColorRestoreChange(v); },
          onFaceToggle: (): void => { this.toggleFace(); }
        })
      }

      if (this.ocrPopupVisible) {
        Column() {
          OcrResultPopup({
            text: this.ocrText,
            modelName: this.findOcrSetName(this.ocrModelId),
            onClose: (): void => { this.ocrPopupVisible = false; }
          })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('rgba(0,0,0,0.55)')
        .onClick((): void => { this.ocrPopupVisible = false; })
      }

      // ★ 表格识别结果弹窗（Grid 渲染合并单元格，复制文本/HTML）；
      //   识别中为 loading 态（转圈+已用时），关闭=放弃识别
      if (this.tablePopupVisible) {
        Column() {
          TableResultPopup({
            rows: this.tableRows,
            cols: this.tableCols,
            cells: this.tableCells,
            html: this.tableHtml,
            elapsedMs: this.tableElapsedMs,
            loading: this.tableLoading,
            loadingHint: this.tableLoadingHint,
            previewPath: this.tablePreviewPath,
            onClose: (): void => {
              if (this.tableLoading) this.abandonTableRecognition();
              else this.tablePopupVisible = false;
            }
          })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('rgba(0,0,0,0.55)')
        .onClick((): void => {
          if (this.tableLoading) this.abandonTableRecognition();
          else this.tablePopupVisible = false;
        })
      }

      // ★ 相册照片预览与 AI 修复（PhotoViewPicker 多选后弹出）：预览首张 +
      //   "AI 高清彩色修复"批量处理全部所选（去噪+超分×4+褪色修复，端侧本地）
      if (this.albumPreviewVisible && this.albumPreviewPm) {
        Column() {
          Row() {
            Text(`已选 ${this.pendingRestoreUris.length} 张照片`)
              .fontSize(16).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).layoutWeight(1)
            Text('✕').fontSize(18).fontColor('#B0B6C0')
              .padding({ left: 12, right: 4, top: 4, bottom: 4 })
              .onClick((): void => { this.albumPreviewVisible = false; })
          }
          .width('92%').margin({ top: 18 })

          Image(this.albumPreviewPm)
            .objectFit(ImageFit.Contain)
            .borderRadius(12)
            .layoutWeight(1)
            .width('92%')
            .margin({ top: 8, bottom: 12 })

          Row({ space: 12 }) {
            Button('打开相册')
              .fontSize(14).fontColor('#E6F7FF')
              .backgroundColor('rgba(0,229,255,0.16)')
              .borderRadius(18).height(38).layoutWeight(1)
              .onClick((): void => {
                this.albumPreviewVisible = false;
                this.openGallery();
              })
            Button('AI 高清彩色修复')
              .fontSize(14).fontColor(this.restoring ? '#80A0AA' : '#003B44')
              .backgroundColor(this.restoring ? 'rgba(0,229,255,0.4)' : '#00E5FF')
              .borderRadius(18).height(38).layoutWeight(1.6)
              .enabled(!this.restoring)
              .onClick((): void => { this.startPhotoRestore(); })
          }
          .width('92%')
          .margin({ bottom: 26 })
        }
        .width('100%').height('100%')
        .backgroundColor('rgba(0,0,0,0.92)')
        .onClick((): void => { this.albumPreviewVisible = false; })
      }

      // ★ 运动设置弹窗（照抄识别设置：开关 + 灵敏度滑条 + 完成）
      if (this.motionSheetVisible) {
        MotionSheet({
          motionEnabled: this.motionEnabled,
          motionSens: this.motionSens,
          onClose: (): void => { this.motionSheetVisible = false; },
          onMotionToggle: (): void => { this.toggleMotion(); },
          onSensChange: (v: number): void => {
            this.motionSens = v;
            this.persistSettings();
          }
        })
      }

      // ★ 水深设置弹窗（开关 + 浑浊度 + 摄像头重量）
      if (this.depthSheetVisible) {
        DepthSheet({
          depthEnabled: this.depthEnabled,
          depthTurbidity: this.depthTurbidity,
          depthWeightG: this.depthWeightG,
          onClose: (): void => { this.depthSheetVisible = false; },
          onDepthToggle: (): void => { this.toggleDepth(); },
          onTurbidityChange: (idx: number): void => { this.onDepthTurbidityChange(idx); },
          onWeightChange: (g: number): void => { this.onDepthWeightChange(g); }
        })
      }

      // ★ 记录设置弹窗（随时保存 + 自动录像开关 + 录像时长 5~30s）
      if (this.recordSheetVisible) {
        RecordSheet({
          isRecording: this.isRecording,
          autoRecordOn: this.autoRecordOn,
          recordDurationSec: this.recordDurationSec,
          onClose: (): void => { this.recordSheetVisible = false; },
          onSaveNow: (): void => { this.onRecordSaveNow(); },
          onAutoRecordToggle: (): void => { this.toggleAutoRecord(); },
          onDurationChange: (sec: number): void => { this.onRecordDurationChange(sec); }
        })
      }

      // ★ 能耗设置弹窗（三档预设 + 六项节奏滑条，改完立即生效并保存）
      if (this.powerSheetVisible) {
        PowerSheet({
          powerMode: this.powerMode,
          powerRenderMs: this.powerRenderMs,
          powerYoloMs: this.powerYoloMs,
          powerYoloIdleMs: this.powerYoloIdleMs,
          powerSensorMs: this.powerSensorMs,
          powerOcrMs: this.powerOcrMs,
          powerThreads: this.powerThreads,
          onClose: (): void => { this.powerSheetVisible = false; },
          onPresetSelect: (mode: number): void => { this.applyPowerPreset(mode); },
          onCustomSelect: (): void => { this.applyPowerCustom(); },
          onRenderMsChange: (v: number): void => {
            this.powerRenderMs = v; this.powerCustom[0] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          },
          onYoloMsChange: (v: number): void => {
            this.powerYoloMs = v; this.powerCustom[1] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          },
          onYoloIdleMsChange: (v: number): void => {
            this.powerYoloIdleMs = v; this.powerCustom[2] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          },
          onSensorMsChange: (v: number): void => {
            this.powerSensorMs = v; this.powerCustom[4] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          },
          onOcrMsChange: (v: number): void => {
            this.powerOcrMs = v; this.powerCustom[3] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          },
          onThreadsChange: (v: number): void => {
            this.powerThreads = v; this.powerCustom[5] = v; this.markPowerCustom(); this.applyPowerParams(); this.persistSettings();
          }
        })
      }

      // ★ 文字识别设置弹窗（长按 OCR 弹出：模型集选择 + det 阈值三件套）
      if (this.ocrSheetVisible) {
        OcrSheet({
          ocrEnabled: this.ocrEnabled,
          ocrModelId: this.ocrModelId,
          modelSets: this.ocrModelSets,
          ocrDbScore: this.ocrDbScore,
          ocrDbBox: this.ocrDbBox,
          ocrUnclip: this.ocrUnclip,
          onClose: (): void => { this.ocrSheetVisible = false; },
          onOcrToggle: (): void => { this.toggleOcr(); },
          onSelectSet: (id: string): void => { this.selectOcrSet(id); },
          onDbScoreChange: (v: number): void => {
            this.ocrDbScore = v; this.applyOcrParams(); this.persistSettings();
          },
          onDbBoxChange: (v: number): void => {
            this.ocrDbBox = v; this.applyOcrParams(); this.persistSettings();
          },
          onUnclipChange: (v: number): void => {
            this.ocrUnclip = v; this.applyOcrParams(); this.persistSettings();
          }
        })
      }

      // ★ 位置信息弹窗（点 HUD 位置行弹出：复制地址/坐标）
      if (this.geoPopupVisible) {
        Column() {
          Column() {
            Text('当前位置').fontSize(15).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
            Text(this.geoPlaceName.length > 0 ? this.geoPlaceName : '暂未获取到位置')
              .fontSize(12).fontColor('#8E97A3').margin({ top: 6 })
              .constraintSize({ maxWidth: '80%' })
            Row({ space: 10 }) {
              Text(`${this.formatGeoLat()},${this.formatGeoLon()}`)
                .fontSize(12).fontColor('#00E5FF')
            }
            .margin({ top: 4 })

            Row({ space: 10 }) {
              Button('复制地址')
                .fontSize(13).fontColor('#003B44').backgroundColor('#00E5FF')
                .borderRadius(16).height(32).layoutWeight(1)
                .onClick((): void => {
                  this.copyGeoText(this.geoPlaceName, '地址');
                })
              Button('复制坐标')
                .fontSize(13).fontColor('#FFFFFF').backgroundColor('rgba(255,255,255,0.12)')
                .borderRadius(16).height(32).layoutWeight(1)
                .onClick((): void => {
                  this.copyGeoText(`${this.formatGeoLat()},${this.formatGeoLon()}`, '坐标');
                })
            }
            .width('100%').margin({ top: 14 })

            Button('关闭')
              .fontSize(13).fontColor('#8E97A3').backgroundColor(Color.Transparent)
              .height(30).margin({ top: 2 })
              .onClick((): void => { this.geoPopupVisible = false; })
          }
          .width('78%')
          .padding({ left: 18, right: 18, top: 18, bottom: 14 })
          .backgroundColor('#16191F')
          .borderRadius(18)
          .alignItems(HorizontalAlign.Center)
        }
        .width('100%').height('100%')
        .justifyContent(FlexAlign.Center)
        .backgroundColor('rgba(0,0,0,0.55)')
        .onClick((): void => { this.geoPopupVisible = false; })
      }

      // ★ USB 设备列表弹窗（照抄 Android 设备选择 Dialog）
      if (this.usbSheetVisible) {
        Column() {
          UsbDeviceSheet({
            devices: this.usbDevices,
            onConnect: (d: UsbCamInfo): void => { this.connectUsbDevice(d); },
            onRescan: (): void => { this.usbDevices = UsbDeviceScanner.scan(); },
            onClose: (): void => { this.usbSheetVisible = false; }
          })
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.End)
        .backgroundColor('rgba(0,0,0,0.55)')
        .onClick((): void => { this.usbSheetVisible = false; })
      }
    }
    .width('100%').height('100%').backgroundColor('#000')
  }
}