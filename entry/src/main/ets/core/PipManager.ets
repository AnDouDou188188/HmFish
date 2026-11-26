// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ PipManager（PipManager.ets）—— 画中画（后台悬浮小窗）管理器                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】                                                                     ║
// ║  封装 PiPWindow 全部交互，主页面只面对本类的少量方法/回调：                   ║
// ║   1. create：PiPWindow.create（VIDEO_LIVE 模板 + 播放/暂停控件组 +            ║
// ║      自定义 HUD NodeController），注册 stateChange/controlEvent              ║
// ║   2. setAutoStart：应用退后台时系统自动拉起小窗（微信式悬浮看画面）           ║
// ║   3. HUD 芯片：REC 红点 + 录像秒数 + 识别计数，1s 轮询刷 @Observed 数据       ║
// ║      （@ObjectLink 子组件自动重绘，无需重建节点）                            ║
// ║   4. 播放/暂停控件：映射识别推理的暂停/恢复（entry.setYoloEnabled 由          ║
// ║      页面回调里做，本类只上报 paused 事件并同步控件图标状态）                 ║
// ║                                                                              ║
// ║ 【生命周期回调契约（onState 回调页面）】                                      ║
// ║   started=true  → 小窗已启动：解除 3s 看门狗、按需重绑预览 surface、         ║
// ║                    启动 HUD 轮询                                             ║
// ║   started=false → 小窗被关闭（用户点 ✕ / 拖垃圾桶 / 还原）：页面据此在        ║
// ║                    退后台期间执行兜底释放（stopForBackground）               ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { PiPWindow, BuilderNode, NodeController, FrameNode } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ 小窗 HUD 数据（@Observed + @ObjectLink 子组件：字段变更自动重绘）
@Observed
class PipHudData {
  isRec: boolean = false;
  recSec: number = -1;
  detCount: number = 0;
}

// ★ 录像秒数格式化 m:ss
function pipFmtSec(s: number): string {
  const t = Math.max(0, s);
  const m = Math.floor(t / 60);
  const r = t % 60;
  return `${m < 10 ? '0' + m : m}:${r < 10 ? '0' + r : r}`;
}

// ★ 小窗 HUD 芯片子组件：REC 红点（闪烁静态近似：常亮）+ m:ss + 识别计数
@Component
struct PipHudPanel {
  @ObjectLink data: PipHudData;

  build() {
    Row({ space: 6 }) {
      if (this.data.isRec) {
        Row().width(7).height(7).borderRadius(3.5).backgroundColor('#FF3B30')
        Text(`REC ${pipFmtSec(this.data.recSec)}`)
          .fontSize(10).fontColor('#FF8A80').fontWeight(FontWeight.Bold)
      }
      Text(`鱼 ${this.data.detCount}`)
        .fontSize(10).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
    }
    .padding({ left: 9, right: 9, top: 4, bottom: 4 })
    .borderRadius(10)
    .backgroundColor('rgba(0,0,0,0.5)')
    .border({ width: 1, color: 'rgba(255,255,255,0.18)' })
  }
}

// ★ 小窗 HUD @Builder：BuilderNode 挂载入口（wrapBuilder 包装后 build）
@Builder
function pipHudBuilder(data: PipHudData) {
  PipHudPanel({ data: data })
}

// ★ HUD NodeController：PiPConfiguration.customUIController 需要 NodeController；
//   makeNode 时用 BuilderNode 把上面的 @Builder 挂成 FrameNode 返回
class PipHudNodeController extends NodeController {
  private hudData: PipHudData;
  private node: BuilderNode<[PipHudData]> | null = null;

  constructor(data: PipHudData) {
    super();
    this.hudData = data;
  }

  makeNode(uiContext: UIContext): FrameNode | null {
    this.node = new BuilderNode<[PipHudData]>(uiContext);
    this.node.build(wrapBuilder<[PipHudData]>(pipHudBuilder), this.hudData);
    return this.node.getFrameNode();
  }
}

// ★ PipManager：画中画控制器包装（一 App 一实例、一 PiPController）
export class PipManager {
  private controller: PiPWindow.PiPController | null = null;
  private hudData: PipHudData = new PipHudData();
  private hudNode: PipHudNodeController = new PipHudNodeController(this.hudData);
  // 当前小窗是否处于启动态（stateChange 维护，页面看门狗用）
  private started: boolean = false;
  // 页面回调：小窗启动/停止；播放/暂停被点
  private onState: ((started: boolean) => void) | null = null;
  private onPauseToggle: ((paused: boolean) => void) | null = null;

  // ★ 小窗是否已启动（供退后台 3s 看门狗判断自动拉起是否成功）
  isStarted(): boolean {
    return this.started;
  }

  // ★ 是否已创建控制器（页面决定走 PiP 保活路径还是普通释放路径）
  isReady(): boolean {
    return this.controller !== null;
  }

  // ★ create：创建 PiP 控制器（VIDEO_LIVE 模板 + 播放/暂停组 + 自定义 HUD）。
  //   幂等：已创建直接返回 true；设备不支持 PiP 返回 false
  async create(ctx: common.UIAbilityContext, xc: XComponentController,
    onState: (started: boolean) => void,
    onPauseToggle: (paused: boolean) => void): Promise<boolean> {
    if (this.controller !== null) return true;
    if (!PiPWindow.isPiPEnabled()) return false;
    this.onState = onState;
    this.onPauseToggle = onPauseToggle;
    const config: PiPWindow.PiPConfiguration = {
      context: ctx,
      componentController: xc,
      templateType: PiPWindow.PiPTemplateType.VIDEO_LIVE,
      controlGroups: [PiPWindow.VideoLiveControlGroup.VIDEO_PLAY_PAUSE],
      customUIController: this.hudNode
    };
    try {
      this.controller = await PiPWindow.create(config);
    } catch (_) {
      this.controller = null;
      return false;
    }
    const ctrl = this.controller;
    // ★ 生命周期：STARTED 解除页面看门狗；STOPPED/ERROR 上报（页面决定兜底释放）
    ctrl.on('stateChange', (state: PiPWindow.PiPState): void => {
      if (state === PiPWindow.PiPState.STARTED) {
        this.started = true;
        this.onState?.(true);
      } else if (state === PiPWindow.PiPState.STOPPED || state === PiPWindow.PiPState.ERROR) {
        this.started = false;
        this.onState?.(false);
      }
    });
    // ★ 控件事件：播放/暂停 → 上报页面（页面暂停/恢复识别推理并回写图标状态）
    ctrl.on('controlEvent', (param: PiPWindow.ControlEventParam): void => {
      if (param.controlType === PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE) {
        // status=PAUSE(0) 用户点了"暂停"；status=PLAY(1) 用户点了"播放"
        const paused = param.status !== PiPWindow.PiPControlStatus.PLAY;
        this.onPauseToggle?.(paused);
      }
    });
    // 初始控件状态：正在识别（播放中）
    this.setPlayPausePlaying(true);
    return true;
  }

  // ★ 退后台自动拉起小窗开关（主窗切后台时系统触发）
  setAutoStart(enable: boolean): void {
    try {
      this.controller?.setAutoStartEnabled(enable);
    } catch (_) {}
  }

  // ★ 前台主动拉起小窗（可选能力；本应用主要走退后台自动拉起）
  async start(): Promise<boolean> {
    if (this.controller === null) return false;
    try {
      await this.controller.startPiP();
      return true;
    } catch (_) {
      return false;
    }
  }

  // ★ 关闭小窗（回前台恢复主窗时调用；幂等）
  async stop(): Promise<void> {
    try {
      await this.controller?.stopPiP();
    } catch (_) {}
  }

  // ★ 更新内容宽高（预览比例变化时同步小窗比例；px，正整数）
  updateContentSize(w: number, h: number): void {
    if (w <= 0 || h <= 0) return;
    try {
      this.controller?.updateContentSize(w, h);
    } catch (_) {}
  }

  // ★ 回写播放/暂停控件图标状态（页面 yolo 实际状态变化后同步小窗按钮）
  setPlayPausePlaying(playing: boolean): void {
    try {
      this.controller?.updatePiPControlStatus(
        PiPWindow.PiPControlType.VIDEO_PLAY_PAUSE,
        playing ? PiPWindow.PiPControlStatus.PLAY : PiPWindow.PiPControlStatus.PAUSE);
    } catch (_) {}
  }

  // ★ 刷新 HUD 芯片（@Observed 字段变更 → @ObjectLink 子组件自动重绘）
  updateHud(isRec: boolean, recSec: number, detCount: number): void {
    this.hudData.isRec = isRec;
    this.hudData.recSec = recSec;
    this.hudData.detCount = detCount;
  }

  // ★ 销毁：注销监听、关小窗、弃控制器（aboutToDisappear 调用）
  destroy(): void {
    const ctrl = this.controller;
    this.controller = null;
    if (ctrl === null) return;
    try { ctrl.off('stateChange'); } catch (_) {}
    try { ctrl.off('controlEvent'); } catch (_) {}
    ctrl.stopPiP().catch((_e: BusinessError): void => {});
    this.started = false;
  }
}
