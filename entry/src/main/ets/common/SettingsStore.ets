// ★ 设置持久化层：preferences（文件 yolo_settings_v3）承载全部用户设置项。
//   数据流：启动时 Index.ets 调 init→load 取"出厂默认值 + 已存值覆盖"的
//   全量快照；之后任意设置变更调用 save(Partial) 增量写入并 flush 落盘。
//   内含多代一次性迁移（画质增强布尔→对比度滑条、增强 mode 枚举 v3 收敛），
//   保证老用户升级后设置不丢、语义正确
import { preferences } from '@kit.ArkData';
import { common } from '@kit.AbilityKit';

// ★ AppSettings：全量设置快照（load 返回完整对象 / save 接收其 Partial）。
//   新增设置项必须同时补 load 默认值与 save 写入分支，
//   否则只在当次运行生效、不会落盘
export interface AppSettings {
  hudOpacity: number;
  cameraPosition: number;
  zoomRatio: number;
  confThresh: number;
  motionSens: number;
  enabledLabels: number[];
  alertLabels: number[];
  autoRecord: boolean;
  recordDurationMs: number;
  alertSound: boolean;
  modelId: string;
  // ★ 所有设置立即持久化：功能开关与旋转状态也纳入（此前重启即丢）
  yoloEnabled: boolean;
  ocrEnabled: boolean;
  motionEnabled: boolean;
  depthEnabled: boolean;
  rotateAngle: number;
  // ★ 水深模型参数：浑浊度(0清澈/1微浑/2浑浊) + 摄像头重量(g)
  depthTurbidity: number;
  depthWeightG: number;
  // ★ 能耗模式：0省电/1标准/2性能/3自定义 + 六项节奏参数（滑条微调立即生效）
  powerMode: number;
  powerRenderMs: number;
  powerYoloMs: number;
  powerYoloIdleMs: number;
  powerSensorMs: number;
  powerOcrMs: number;
  powerThreads: number;
  // ★ 自定义档参数（最近一次手动微调的六项值）：点"自定义"按钮恢复这套参数
  powerCustom: number[];
  // ★ OCR 模型集 + det 阈值三件套（长按 OCR 弹窗设置）
  ocrModelId: string;
  ocrDbScore: number;
  ocrDbBox: number;
  ocrUnclip: number;
  ocrThreads: number;
  // ★ 语音播报（识别设置弹窗开关+音量）+ 自动录像触发音效（开关+自定义 MP3）
  speechOn: boolean;
  speechVolume: number;
  // ★ YOLO 识别结果朗读（识别设置内独立开关，默认关闭）
  detectSpeak: boolean;
  recordSoundOn: boolean;
  recordSoundName: string;
  recordSoundPath: string;
  // ★ 识别框/标签画面显示开关（默认开：关闭后不画框/文字，识别与自动录像不受影响）
  drawBoxesOn: boolean;
  // ★ 画质增强（USB/手机前后置全部相机生效）：
  //   mode 0 关 / 1 非AI(亮度+锐度+对比度+饱和度) / 2 AI增强
  //   （超分细节 + AI降噪 + 水下去浊 + 色彩还原，修复层异步合成）
  usbBrightness: number;
  usbSharpness: number;
  usbEnhanceMode: number;
  usbSrStrength: number;
  // ★ 对比度/饱和度滑条（-50~+50，替代旧版"画质增强"布尔开关）
  usbContrast: number;
  usbSaturation: number;
  // ★ AI 增强附加模块（mode 2 内独立滑条，0~100）
  aiDenoise: number;
  aiClarity: number;
  aiColorRestore: number;
  // ★ 人脸识别开关（SCRFD 检测 + 106 关键点连线框 + 性别年龄颜值标注 + HUD）
  faceEnabled: boolean;
  // ★ mode 枚举 v3 迁移标记：v1/v2 的 2/3/4/5（旧多模型）→ v3 的 2（AI超分）
  usbEnhanceV2: boolean;
  usbEnhanceV3: boolean;
  // ★ "参数非零但模式为关→升为非AI"迁移的一次性标记：此前该迁移每次启动都跑，
  //   用户明明选了"关闭"重启却被强行改回非AI——加标记保证只迁移一次
  usbParamModeMigrated: boolean;
  // ★ 画中画（后台悬浮小窗）开关：退后台时用 PiP 小窗继续看画面（默认开）
  pipEnabled: boolean;
  // ★ 语音命令开关：点悬浮麦克风说话，AI 模糊识别纠正后执行（默认关）
  voiceCmdEnabled: boolean;
  // ★ 应用内自动更新：自动检查开关 / 跳过的版本 tag / 稍后提醒截止时间戳 / 上次检查时间戳
  updateAutoCheck: boolean;
  updateSkipTag: string;
  updateSnoozeUntil: number;
  updateLastCheck: number;
}

// ★ preferences 文件名：带版本号——跨大版本结构不兼容时换文件名即可整体重置
const STORE = 'yolo_settings_v3';

// ★ SettingsStore：静态单例风格（pref 全局一份，EntryAbility 启动后先 init）
export class SettingsStore {
  private static pref: preferences.Preferences | null = null;

  // ★ init：打开（或创建）preferences 实例；必须先于 load/save 调用
  static async init(ctx: common.UIAbilityContext): Promise<void> {
    if (SettingsStore.pref) return;
    SettingsStore.pref = await preferences.getPreferences(ctx, STORE);
  }

  // ★ load：组装全量设置——先填出厂默认值，再用已存值逐项覆盖，
  //   保证新增设置项在老数据下也拿到合理默认（不会是 undefined）
  static async load(): Promise<AppSettings> {
    const r: AppSettings = {
      hudOpacity: 0.45,
      cameraPosition: 0,
      zoomRatio: 1.0,
      confThresh: 0.45,
      motionSens: 0.35,
      enabledLabels: [],
      alertLabels: [],
      autoRecord: false,
      recordDurationMs: 10000,
      alertSound: true,
      modelId: '',
      yoloEnabled: true,
      ocrEnabled: false,
      motionEnabled: false,
      depthEnabled: false,
      rotateAngle: 0,
      depthTurbidity: 1,
      depthWeightG: 30,
      powerMode: 1,
      powerRenderMs: 40,
      powerYoloMs: 300,
      powerYoloIdleMs: 800,
      powerSensorMs: 250,
      powerOcrMs: 4000,
      powerThreads: 8,
      powerCustom: [40, 300, 800, 4000, 250, 8],
      ocrModelId: 'paddle',
      ocrDbScore: 0.5,
      ocrDbBox: 0.3,
      ocrUnclip: 1.6,
      ocrThreads: 4,
      speechOn: true,
      speechVolume: 30,
      detectSpeak: false,
      recordSoundOn: true,
      recordSoundName: '',
      recordSoundPath: '',
      drawBoxesOn: true,
      usbBrightness: 0,
      usbSharpness: 0,
      usbEnhanceMode: 0,
      usbSrStrength: 100,
      usbContrast: 0,
      usbSaturation: 0,
      aiDenoise: 50,
      aiClarity: 50,
      aiColorRestore: 60,
      faceEnabled: false,
      usbEnhanceV2: false,
      usbEnhanceV3: false,
      usbParamModeMigrated: false,
      pipEnabled: true,
      voiceCmdEnabled: false,
      updateAutoCheck: true,
      updateSkipTag: '',
      updateSnoozeUntil: 0,
      updateLastCheck: 0
    };
    if (!SettingsStore.pref) return r;
    const p = SettingsStore.pref;
    r.hudOpacity = await p.get('hudOpacity', 0.45) as number;
    r.cameraPosition = await p.get('cameraPosition', 0) as number;
    r.zoomRatio = await p.get('zoomRatio', 1.0) as number;
    r.confThresh = await p.get('confThresh', 0.25) as number;
      r.motionSens = await p.get('motionSens', 0.35) as number;
    r.autoRecord = await p.get('autoRecord', false) as boolean;
    r.recordDurationMs = await p.get('recordDurationMs', 10000) as number;
    r.alertSound = await p.get('alertSound', true) as boolean;
    r.modelId = await p.get('modelId', '') as string;
    r.yoloEnabled = await p.get('yoloEnabled', true) as boolean;
    r.ocrEnabled = await p.get('ocrEnabled', false) as boolean;
    r.motionEnabled = await p.get('motionEnabled', false) as boolean;
    r.depthEnabled = await p.get('depthEnabled', false) as boolean;
    r.rotateAngle = await p.get('rotateAngle', 0) as number;
    // ★ 水深模型参数：浑浊度 + 摄像头重量
    r.depthTurbidity = await p.get('depthTurbidity', 1) as number;
    r.depthWeightG = await p.get('depthWeightG', 30) as number;
    // ★ 能耗模式 + OCR 模型/参数
    r.powerMode = await p.get('powerMode', 1) as number;
    r.powerRenderMs = await p.get('powerRenderMs', 40) as number;
    r.powerYoloMs = await p.get('powerYoloMs', 300) as number;
    r.powerYoloIdleMs = await p.get('powerYoloIdleMs', 800) as number;
    r.powerSensorMs = await p.get('powerSensorMs', 250) as number;
    r.powerOcrMs = await p.get('powerOcrMs', 4000) as number;
    r.powerThreads = await p.get('powerThreads', 8) as number;
    // ★ 自定义档参数：非法/缺失回退标准预设
    r.powerCustom = parseNumList(await p.get('powerCustom', '40,300,800,4000,250,8') as string);
    if (r.powerCustom.length !== 6) r.powerCustom = [40, 300, 800, 4000, 250, 8];
    r.ocrModelId = await p.get('ocrModelId', 'paddle') as string;
    r.ocrDbScore = await p.get('ocrDbScore', 0.5) as number;
    r.ocrDbBox = await p.get('ocrDbBox', 0.3) as number;
    r.ocrUnclip = await p.get('ocrUnclip', 1.6) as number;
    r.ocrThreads = await p.get('ocrThreads', 4) as number;
    r.speechOn = await p.get('speechOn', true) as boolean;
    r.speechVolume = await p.get('speechVolume', 30) as number;
    r.detectSpeak = await p.get('detectSpeak', false) as boolean;
    r.recordSoundOn = await p.get('recordSoundOn', true) as boolean;
    r.recordSoundName = await p.get('recordSoundName', '') as string;
    r.recordSoundPath = await p.get('recordSoundPath', '') as string;
    r.drawBoxesOn = await p.get('drawBoxesOn', true) as boolean;
    r.usbBrightness = await p.get('usbBrightness', 0) as number;
    r.usbSharpness = await p.get('usbSharpness', 0) as number;
    r.usbEnhanceMode = await p.get('usbEnhanceMode', 0) as number;
    r.usbSrStrength = await p.get('usbSrStrength', 100) as number;
    // ★ v4 一次性迁移：旧版"画质增强"布尔开关 → 对比度/饱和度滑条（15/15）
    const oldContrast = await p.get('usbContrast', -999) as number;
    if (oldContrast === -999) {
      const oldEnhance = await p.get('usbEnhance', false) as boolean;
      r.usbContrast = oldEnhance ? 15 : 0;
      await p.put('usbContrast', r.usbContrast);
    } else {
      r.usbContrast = oldContrast;
    }
    const oldSaturation = await p.get('usbSaturation', -999) as number;
    if (oldSaturation === -999) {
      const oldEnhance = await p.get('usbEnhance', false) as boolean;
      r.usbSaturation = oldEnhance ? 15 : 0;
      await p.put('usbSaturation', r.usbSaturation);
    } else {
      r.usbSaturation = oldSaturation;
    }
    r.faceEnabled = await p.get('faceEnabled', false) as boolean;
    // ★ AI 增强附加模块（0~100，未存储时用推荐默认）
    r.aiDenoise = await p.get('aiDenoise', 50) as number;
    r.aiClarity = await p.get('aiClarity', 50) as number;
    r.aiColorRestore = await p.get('aiColorRestore', 60) as number;
    // ★ mode 枚举 v3 一次性迁移：v1/v2 的 2/3/4/5（旧多模型枚举）→ v3 的 2（AI超分）
    r.usbEnhanceV2 = await p.get('usbEnhanceV2', false) as boolean;
    r.usbEnhanceV3 = await p.get('usbEnhanceV3', false) as boolean;
    r.usbParamModeMigrated = await p.get('usbParamModeMigrated', false) as boolean;
    // ★ 画中画 + 语音命令 + 应用内更新设置
    r.pipEnabled = await p.get('pipEnabled', true) as boolean;
    r.voiceCmdEnabled = await p.get('voiceCmdEnabled', false) as boolean;
    r.updateAutoCheck = await p.get('updateAutoCheck', true) as boolean;
    r.updateSkipTag = await p.get('updateSkipTag', '') as string;
    r.updateSnoozeUntil = await p.get('updateSnoozeUntil', 0) as number;
    r.updateLastCheck = await p.get('updateLastCheck', 0) as number;
    if (!r.usbEnhanceV3) {
      if (r.usbEnhanceMode >= 2 && r.usbEnhanceMode <= 5) {
        r.usbEnhanceMode = 2;
      }
      r.usbEnhanceV3 = true;
      await p.put('usbEnhanceMode', r.usbEnhanceMode);
      await p.put('usbEnhanceV3', true);
      await p.flush();
    }
    r.enabledLabels = parseNumList(await p.get('enabledLabels', '') as string);
    r.alertLabels = parseNumList(await p.get('alertLabels', '') as string);
    return r;
  }

  // ───────── 增量保存 ─────────
  // ★ save：只落传入的字段（Partial 增量写），列表序列化为逗号串，
  //   最后统一 flush 确保立刻持久化到磁盘
  static async save(s: Partial<AppSettings>): Promise<void> {
    if (!SettingsStore.pref) return;
    const p = SettingsStore.pref;
    if (s.hudOpacity !== undefined) await p.put('hudOpacity', s.hudOpacity);
    if (s.cameraPosition !== undefined) await p.put('cameraPosition', s.cameraPosition);
    if (s.zoomRatio !== undefined) await p.put('zoomRatio', s.zoomRatio);
    if (s.confThresh !== undefined) await p.put('confThresh', s.confThresh);
    if (s.motionSens !== undefined) await p.put('motionSens', s.motionSens);
    if (s.autoRecord !== undefined) await p.put('autoRecord', s.autoRecord);
    if (s.recordDurationMs !== undefined) await p.put('recordDurationMs', s.recordDurationMs);
    if (s.alertSound !== undefined) await p.put('alertSound', s.alertSound);
    if (s.modelId !== undefined) await p.put('modelId', s.modelId);
    if (s.yoloEnabled !== undefined) await p.put('yoloEnabled', s.yoloEnabled);
    if (s.ocrEnabled !== undefined) await p.put('ocrEnabled', s.ocrEnabled);
    if (s.motionEnabled !== undefined) await p.put('motionEnabled', s.motionEnabled);
    if (s.depthEnabled !== undefined) await p.put('depthEnabled', s.depthEnabled);
    if (s.rotateAngle !== undefined) await p.put('rotateAngle', s.rotateAngle);
    if (s.depthTurbidity !== undefined) await p.put('depthTurbidity', s.depthTurbidity);
    if (s.depthWeightG !== undefined) await p.put('depthWeightG', s.depthWeightG);
    if (s.powerMode !== undefined) await p.put('powerMode', s.powerMode);
    if (s.powerRenderMs !== undefined) await p.put('powerRenderMs', s.powerRenderMs);
    if (s.powerYoloMs !== undefined) await p.put('powerYoloMs', s.powerYoloMs);
    if (s.powerYoloIdleMs !== undefined) await p.put('powerYoloIdleMs', s.powerYoloIdleMs);
    if (s.powerSensorMs !== undefined) await p.put('powerSensorMs', s.powerSensorMs);
    if (s.powerOcrMs !== undefined) await p.put('powerOcrMs', s.powerOcrMs);
    if (s.powerThreads !== undefined) await p.put('powerThreads', s.powerThreads);
    if (s.powerCustom !== undefined) await p.put('powerCustom', s.powerCustom.join(','));
    if (s.ocrModelId !== undefined) await p.put('ocrModelId', s.ocrModelId);
    if (s.ocrDbScore !== undefined) await p.put('ocrDbScore', s.ocrDbScore);
    if (s.ocrDbBox !== undefined) await p.put('ocrDbBox', s.ocrDbBox);
    if (s.ocrUnclip !== undefined) await p.put('ocrUnclip', s.ocrUnclip);
    if (s.ocrThreads !== undefined) await p.put('ocrThreads', s.ocrThreads);
    if (s.speechOn !== undefined) await p.put('speechOn', s.speechOn);
    if (s.speechVolume !== undefined) await p.put('speechVolume', s.speechVolume);
    if (s.detectSpeak !== undefined) await p.put('detectSpeak', s.detectSpeak);
    if (s.recordSoundOn !== undefined) await p.put('recordSoundOn', s.recordSoundOn);
    if (s.recordSoundName !== undefined) await p.put('recordSoundName', s.recordSoundName);
    if (s.recordSoundPath !== undefined) await p.put('recordSoundPath', s.recordSoundPath);
    if (s.drawBoxesOn !== undefined) await p.put('drawBoxesOn', s.drawBoxesOn);
    if (s.usbBrightness !== undefined) await p.put('usbBrightness', s.usbBrightness);
    if (s.usbSharpness !== undefined) await p.put('usbSharpness', s.usbSharpness);
    if (s.usbEnhanceMode !== undefined) await p.put('usbEnhanceMode', s.usbEnhanceMode);
    if (s.usbSrStrength !== undefined) await p.put('usbSrStrength', s.usbSrStrength);
    if (s.usbContrast !== undefined) await p.put('usbContrast', s.usbContrast);
    if (s.usbSaturation !== undefined) await p.put('usbSaturation', s.usbSaturation);
    if (s.aiDenoise !== undefined) await p.put('aiDenoise', s.aiDenoise);
    if (s.aiClarity !== undefined) await p.put('aiClarity', s.aiClarity);
    if (s.aiColorRestore !== undefined) await p.put('aiColorRestore', s.aiColorRestore);
    if (s.faceEnabled !== undefined) await p.put('faceEnabled', s.faceEnabled);
    if (s.usbEnhanceV2 !== undefined) await p.put('usbEnhanceV2', s.usbEnhanceV2);
    if (s.usbEnhanceV3 !== undefined) await p.put('usbEnhanceV3', s.usbEnhanceV3);
    if (s.usbParamModeMigrated !== undefined) await p.put('usbParamModeMigrated', s.usbParamModeMigrated);
    if (s.pipEnabled !== undefined) await p.put('pipEnabled', s.pipEnabled);
    if (s.voiceCmdEnabled !== undefined) await p.put('voiceCmdEnabled', s.voiceCmdEnabled);
    if (s.updateAutoCheck !== undefined) await p.put('updateAutoCheck', s.updateAutoCheck);
    if (s.updateSkipTag !== undefined) await p.put('updateSkipTag', s.updateSkipTag);
    if (s.updateSnoozeUntil !== undefined) await p.put('updateSnoozeUntil', s.updateSnoozeUntil);
    if (s.updateLastCheck !== undefined) await p.put('updateLastCheck', s.updateLastCheck);
    if (s.enabledLabels !== undefined) await p.put('enabledLabels', s.enabledLabels.join(','));
    if (s.alertLabels !== undefined) await p.put('alertLabels', s.alertLabels.join(','));
    await p.flush();
  }

  // ★ 自动录像触发类别按模型分别保存/加载：切换模型不再清空，
  //   每个模型维护自己的勾选类别集合（key: mlabels_<modelId>）
  static async setModelAlerts(modelId: string, labels: number[]): Promise<void> {
    if (!SettingsStore.pref || !modelId) return;
    await SettingsStore.pref.put(`mlabels_${modelId}`, labels.join(','));
    await SettingsStore.pref.flush();
  }

  // ★ 读取某模型的触发类别：null 表示从未设置过（调用方回退全局 alertLabels）
  static async getModelAlerts(modelId: string): Promise<number[] | null> {
    if (!SettingsStore.pref || !modelId) return null;
    // has() 区分"从未保存过"（回退全局 alertLabels）与"保存过但为空"（合法的空集合）
    const has = await SettingsStore.pref.has(`mlabels_${modelId}`);
    if (!has) return null;
    const raw = await SettingsStore.pref.get(`mlabels_${modelId}`, '') as string;
    return parseNumList(raw);
  }
}

// ★ parseNumList："1,2,3" → [1,2,3]；非法片段丢弃，空串返回空数组
function parseNumList(s: string): number[] {
  if (!s) return [];
  return s.split(',').map((x: string) => parseInt(x)).filter((n: number) => !isNaN(n));
}
