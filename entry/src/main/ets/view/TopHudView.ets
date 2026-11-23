// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ TopHudView（TopHudView.ets）—— 顶部 HUD 状态条                                ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  屏幕顶部半透明状态条，集中展示运行状态并聚合功能入口：                        ║
// ║   第 1 行（常显）：HUD 折叠徽标 + 模型名 + hud 透明度滑条 + 置信度 + 电池     ║
// ║   折叠区（collapsed 控制显隐）：摄像头切换胶囊 / 五功能徽标                   ║
// ║     （YOLO·OCR·能耗·画面·表格）/ 人脸行 / YOLO 合并结果 / 运动等级 /         ║
// ║     水深读数与历史 / OCR 摘要（"x行共x字·点击查看"或"识别中 done/total"）      ║
// ║   录像时另有红点"正在录像 m:ss"横幅（随 hudOpacity）                          ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  全部状态 @Prop 由 Index 下发（识别结果经 VisionController 节流合并）；        ║
// ║  开关/弹窗入口/透明度经 onXxx 回调上抛；localOpacity 本地缓冲切断 @Prop        ║
// ║  回写回路（否则拖动滑条会被父状态回写打断/回跳）；collapsed 为本地折叠态      ║
// ║                                                                              ║
// ║ 【关键契约】                                                                 ║
// ║  根容器 hitTestBehavior(Transparent)：本容器 100%×100% 全屏，               ║
// ║  Default 会响应全屏命中测试并阻塞所有低 z 兄弟节点（预览触摸层/捏合缩放）     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 顶部 HUD：合并结果/计数/相机信息等顶部状态展示
import { Merged } from '../common/Types';

// ★ 水深历史记录：最近 N 次读数+时间戳，读数不被下一次覆盖丢失
export interface DepthRecord {
  cm: number;
  time: string;
}

// ★ 人脸 HUD 行：每张人脸一行（彩色圆点 + 摘要 + "详细"胶囊，点详细弹详情面板）
//   tid：跨帧跟踪 id（libocr 稳定身份，同脸不变）——"详细"按它找人，
//   不随检测置信度排序导致的数组下标跳变而弹错人
export interface FaceHudRow {
  idx: number;
  tid: number;
  text: string;
  color: string;
}

// ★ 人脸主色 hex → 半透明底色（"详细"胶囊底，与彩色描边同色系）
function faceRowBg(hex: string): string {
  const r = parseInt(hex.substring(1, 3), 16);
  const g = parseInt(hex.substring(3, 5), 16);
  const b = parseInt(hex.substring(5, 7), 16);
  return `rgba(${r},${g},${b},0.18)`;
}

// ★ TopHudView：顶部 HUD 主体（第一行常显 + collapsed 控制折叠区显隐）
@Component
export struct TopHudView {
  // ★ 基础状态：当前摄像头位（0后置/1前置/2USB）/USB 是否在连/各功能开关/HUD 整体透明度
  @Prop cameraPosition: number;
  @Prop usbConnected: boolean;
  @Prop yoloEnabled: boolean;
  @Prop ocrEnabled: boolean;
  @Prop hudOpacity: number;
  // ★ HUD 顶部偏移：状态栏安全区高度+小间距（父组件按窗口安全区计算传入）
  @Prop hudTopOffset: number = 44;
  // ★ 识别与传感数据（Index 节流合并后下发）：YOLO 合并结果列表/
  //   运动等级(0~1)/水深当前读数(cm)
  @Prop mergedList: Merged[];
  @Prop motionLevel: number;
  @Prop motionEnabled: boolean;
  @Prop depthEnabled: boolean;
  @Prop depthCm: number;
  // ★ 最近 3 次水深记录（时间+读数）：避免读数被下一次刷新覆盖丢失
  @Prop depthHistory: DepthRecord[] = [];
  @Prop modelName: string = '';
  @Prop confThresh: number = 0.25;
  // ★ 电池电量：置信度旁显示（-1 表示未取到不显示）；充电中显示 ⚡
  @Prop batterySoc: number = -1;
  @Prop batteryCharging: boolean = false;
  @Prop ocrText: string = '';
  @Prop motionSens: number = 0.35;
  @Prop isRecording: boolean = false;
  @Prop recordSec: number = -1;
  @Prop recBlink: boolean = true;
  // ★ 切换/加载中：禁用相机选择，防止状态错乱（HUD 不选中其他摄像头）
  @Prop busy: boolean = false;
  // ★ OCR 识别进度：只在 HUD"识别中"徽标显示，不再画在画面上
  @Prop ocrDone: number = 0;
  @Prop ocrTotal: number = 0;
  // ★ 能耗模式：0省电/1标准/2性能/3自定义（胶囊显示当前档位，点击弹设置）
  @Prop powerMode: number = 1;
  // ★ OCR 模型集短名（如"票据"/"普通"）：徽标与文字识别行区分当前是哪个模型
  @Prop ocrModelName: string = '';
  // ★ 人脸识别开关 + 每人脸一行摘要（父组件轮询刷新；点"详细"弹详情面板）
  @Prop faceEnabled: boolean = false;
  @Prop faceHudRows: FaceHudRow[] = [];
  // ★ 画面增强模式（画面徽标按设置变色区分）：0关/1非AI/2 AI增强
  @Prop usbEnhanceMode: number = 0;

  // ── 与 Index 的交互回调：选摄像头/各功能开关（短按）与长按弹设置/
  //    透明度实时提交/各弹窗入口/人脸详情/表格识别触发 ──
  onCameraSelect?: (pos: number) => void;
  // ★ 画面设置胶囊：USB 增强/识别框/人脸识别统一入口
  onShowPictureSheet?: () => void;
  onYoloToggle?: () => void;
  // ★ YOLO 长按：弹出与底部"识别"长按相同的识别设置菜单
  onYoloLongPress?: () => void;
  onOcrToggle?: () => void;
  onOcrLongPress?: () => void;
  onOpacityChange?: (v: number) => void;
  onShowOcrPopup?: () => void;
  onShowPowerSheet?: () => void;
  // ★ 人脸详情：点某张人脸行的"详细"链接，弹出该脸详情面板（箭头指向人脸框）
  onFaceDetail?: (idx: number) => void;
  // ★ 表格识别：点击"▦ 表格"徽标触发单次表格结构识别
  onRecognizeTable?: () => void;
  // ★ 表格识别中：徽标显示转圈，防重复触发
  @Prop tableBusy: boolean = false;

  // ★ 透明度滑条本地缓冲：滑条绑本地值切断 @Prop 回写回路（否则拖动会被打断/回跳），
  //   拖动中实时提交父组件（HUD 背景即时变化）
  @State localOpacity: number = 0.45;
  // ★ HUD 折叠：点模型名前的 HUD 徽标切换；折叠时只显示第一行（模型名+透明度滑条）
  @State collapsed: boolean = false;

  // ★ 入场：透明度滑条本地缓冲对齐父状态（拖动期间以本地值为准）
  aboutToAppear(): void {
    this.localOpacity = this.hudOpacity;
  }

  // ★ 录像秒数格式化为 m:ss
  private fmtSec(): string {
    const s = Math.max(0, this.recordSec);
    const m = Math.floor(s / 60);
    const r = s % 60;
    return `${m < 10 ? '0' + m : m}:${r < 10 ? '0' + r : r}`;
  }

  // ★ 能耗档位文案：0省电/1标准/2性能/3自定义
  private powerLabel(): string {
    if (this.powerMode === 0) return '省电';
    if (this.powerMode === 2) return '性能';
    if (this.powerMode === 3) return '自定义';
    return '标准';
  }

  // ★ 能耗胶囊底色：档位配色（省电绿/标准蓝/性能橙/自定义紫）
  private powerBg(): string {
    if (this.powerMode === 0) return 'rgba(52,199,89,0.75)';
    if (this.powerMode === 2) return 'rgba(255,149,0,0.75)';
    if (this.powerMode === 3) return 'rgba(175,82,222,0.75)';
    return 'rgba(10,132,255,0.75)';
  }

  build() {
    Stack() {
      // ★ HUD 紧凑化：间距/内边距整体收紧，减少对画面的遮挡
        Column({ space: 7 }) {
        // ── 录像横幅：红点闪烁 + "正在录像 m:ss"（仅录像时显示） ──
        if (this.isRecording) {
          Row({ space: 5 }) {
            Row().width(7).height(7).borderRadius(3.5)
              .backgroundColor(this.recBlink ? '#FF3B30' : 'rgba(255,59,48,0.25)')
            Text(`正在录像 ${this.fmtSec()}`)
              .fontSize(11).fontColor('#FF8A80').fontWeight(FontWeight.Bold)
          }
          .padding({ left: 10, right: 10, top: 3, bottom: 3 })
          .borderRadius(9)
          .backgroundColor('rgba(255,59,48,0.14)')
          .border({ width: 1, color: 'rgba(255,59,48,0.45)' })
          .width('100%').justifyContent(FlexAlign.Center)
          .opacity(this.hudOpacity)
        }

        // ── 第一行（常显，折叠时也保留）：HUD 折叠徽标 / 模型名 /
        //    hud 透明度滑条 / 置信度 / 电池 ──
        Row({ space: 5 }) {
          // ★ HUD 徽标（模型名前）：点击折叠/展开整个 HUD；折叠时只留第一行
          Row({ space: 3 }) {
            Text('HUD').fontSize(9).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
            Text(this.collapsed ? '▸' : '▾')
              .fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          }
          .padding({ left: 5, right: 5, top: 2, bottom: 2 })
          .borderRadius(8)
          .backgroundColor('rgba(0,229,255,0.22)')
          .border({ width: 1, color: 'rgba(0,229,255,0.55)' })
          .opacity(this.hudOpacity)
          .onClick(() => { this.collapsed = !this.collapsed; })

          Row().width(6).height(6).borderRadius(3)
            .backgroundColor(this.yoloEnabled ? '#00E5FF' : '#6E7681')
            .opacity(this.hudOpacity)
          Text(this.modelName.length > 0 ? this.modelName : '未加载模型')
            .fontSize(11).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis }).layoutWeight(1)
            .opacity(this.hudOpacity)
          // ★ HUD 透明度挪到第一排：带"hud透明"文字，拖动实时生效
          //   "hud透明"字+滑条+% 常显（加深色底保证透明态可读），其余文字随透明度消失
          Text('hud透明').fontSize(9).fontColor('#FFFFFF')
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .borderRadius(7).backgroundColor('rgba(0,0,0,0.5)')
          Slider({ value: this.localOpacity, min: 0.0, max: 0.95, step: 0.05 })
            .width(46)
            .trackThickness(2)
            .blockSize({ width: 8, height: 8 })
            .selectedColor('#FFFFFF')
            .trackColor('rgba(0,0,0,0.5)')
            .blockColor('#FFFFFF')
            .onChange((v: number) => {
              this.localOpacity = v;
              this.onOpacityChange?.(v);
            })
          Text(`${Math.round(this.localOpacity * 100)}%`)
            .fontSize(9).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
            .width(30).textAlign(TextAlign.End)
          Text(`置信度 ${(this.confThresh * 100).toFixed(0)}%`)
            .fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
            .opacity(this.hudOpacity)
          // ★ 剩余电池电量（置信度旁）：充电中 ⚡；低电量(≤20%)红色提醒
          if (this.batterySoc >= 0) {
            Text(`${this.batteryCharging ? '⚡' : '🔋'}${this.batterySoc}%`)
              .fontSize(10)
              .fontColor(this.batteryCharging ? '#FFD60A' :
                (this.batterySoc <= 20 ? '#FF6B5E' : '#FFFFFF'))
              .fontWeight(FontWeight.Bold)
              .opacity(this.hudOpacity)
          }
        }.width('100%')

        // ★ 折叠态：以下整块隐藏，只留第一行（HUD 徽标切换）
        if (!this.collapsed) {
      Column({ space: 7 }) {
        Row({ space: 8 }) {
          // ★ 每类摄像头独立配色：USB=橙、后置=蓝、前置=紫；
          //   文案明确是哪个摄像头；选中样式对齐"运动"开启态（半透明彩色底+彩边+彩字）
          this.camPill('USB 摄像头', 2, '#FF9500', 'rgba(255,149,0,0.28)')
          this.camPill('手机后置', 0, '#0A84FF', 'rgba(10,132,255,0.28)')
          this.camPill('手机前置', 1, '#AF52DE', 'rgba(175,82,222,0.28)')
        }.justifyContent(FlexAlign.SpaceEvenly).width('100%')

        Row({ space: 6 }) {
          // ★ 五徽标统一规格：固定高 28、同圆角/边框、平分整行宽度，仅配色区分；
          //   开关态由背景色表达（绿=开/白=关），不再显示 ✓/✗ 前缀，降低密集感
          Row({ space: 2 }) {
            Text('YOLO').fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          }
          .height(28)
          .padding({ left: 2, right: 2 })
          .borderRadius(9)
          .border({ width: 1, color: 'rgba(255,255,255,0.15)' })
          .backgroundColor(this.yoloEnabled ? '#34C759' : 'rgba(255,255,255,0.1)')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          // ★ YOLO 与 OCR/识别一致：短按=开关，长按=弹出识别设置菜单
          .gesture(GestureGroup(GestureMode.Exclusive,
            TapGesture().onAction(() => this.onYoloToggle?.()),
            LongPressGesture({ repeat: false, duration: 500 })
              .onAction(() => this.onYoloLongPress?.())
          ))

          Row({ space: 2 }) {
            // ★ 徽标区分模型：显示"OCR·票据"/"OCR·普通"（当前模型集短名）
            Text(`OCR${this.ocrModelName.length > 0 ? '·' + this.ocrModelName : ''}`)
              .fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).maxLines(1)
          }
          .height(28)
          .padding({ left: 2, right: 2 })
          .borderRadius(9)
          .border({ width: 1, color: 'rgba(255,255,255,0.15)' })
          .backgroundColor(this.ocrEnabled ? '#34C759' : 'rgba(255,255,255,0.1)')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          // ★ OCR 与识别/水深/运动一致：短按=开关，长按=弹出模型/参数设置
          .gesture(GestureGroup(GestureMode.Exclusive,
            TapGesture().onAction(() => this.onOcrToggle?.()),
            LongPressGesture({ repeat: false, duration: 500 })
              .onAction(() => this.onOcrLongPress?.())
          ))

          // ★ 能耗模式徽标：显示当前档位（省电/标准/性能/自定义），点击弹设置
          Row({ space: 2 }) {
            Text('⚡').fontSize(10).fontColor('#FFFFFF')
            Text(this.powerLabel()).fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          }
          .height(28)
          .padding({ left: 2, right: 2 })
          .borderRadius(9)
          .border({ width: 1, color: 'rgba(255,255,255,0.15)' })
          .backgroundColor(this.powerBg())
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          .onClick(() => this.onShowPowerSheet?.())

          // ★ 画面设置徽标：按增强模式变色变字区分（灰=关/青=非AI增强/紫=AI增强），
          //   一眼看出当前画面增强档位，点开弹设置
          Row({ space: 2 }) {
            Text('⚙').fontSize(10).fontColor('#FFFFFF')
            Text(this.usbEnhanceMode === 2 ? '画面·AI' : (this.usbEnhanceMode === 1 ? '画面·增' : '画面'))
              .fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).maxLines(1)
          }
          .height(28)
          .padding({ left: 2, right: 2 })
          .borderRadius(9)
          .border({
            width: 1,
            color: this.usbEnhanceMode === 2 ? 'rgba(191,90,242,0.8)' :
              (this.usbEnhanceMode === 1 ? 'rgba(0,229,255,0.55)' : 'rgba(255,255,255,0.15)')
          })
          .backgroundColor(this.usbEnhanceMode === 2 ? 'rgba(191,90,242,0.6)' :
            (this.usbEnhanceMode === 1 ? 'rgba(0,229,255,0.5)' : 'rgba(255,255,255,0.1)'))
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          .onClick(() => this.onShowPictureSheet?.())

          // ★ 表格识别徽标：点击对当前画面做一次 PP-Structure 表格结构识别
          Row({ space: 2 }) {
            if (this.tableBusy) {
              LoadingProgress().width(10).height(10).color('#FFD60A')
            } else {
              Text('▦').fontSize(10).fontColor('#FFFFFF')
            }
            Text('表格').fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).maxLines(1)
          }
          .height(28)
          .padding({ left: 2, right: 2 })
          .borderRadius(9)
          .border({ width: 1, color: 'rgba(255,255,255,0.15)' })
          .backgroundColor(this.tableBusy ? 'rgba(255,214,10,0.45)' : 'rgba(255,159,10,0.75)')
          .layoutWeight(1)
          .justifyContent(FlexAlign.Center)
          .onClick(() => {
            if (this.tableBusy) return;
            this.onRecognizeTable?.();
          })
        }.width('100%').alignItems(VerticalAlign.Center)

        // ★ 人脸识别 HUD 行：移至 YOLO/OCR 按钮行下方，识别数据顺序统一为 人脸→YOLO→OCR；
        //   每张人脸一行（彩色圆点 + 摘要 + "详细"链接，父组件轮询刷新），与按钮行多留间隔
        if (this.faceEnabled && this.faceHudRows.length > 0) {
          Column({ space: 2 }) {
            ForEach(this.faceHudRows, (r: FaceHudRow) => {
              Row({ space: 5 }) {
                Row().width(7).height(7).borderRadius(3.5).backgroundColor(r.color)
                Text(r.text)
                  .fontSize(11).fontColor('#B8FFE3').fontWeight(FontWeight.Bold)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .layoutWeight(1)
                Text('详细')
                  .fontSize(11).fontColor(r.color).fontWeight(FontWeight.Bold)
                  .padding({ left: 12, right: 12, top: 4, bottom: 4 })
                  .borderRadius(12)
                  .backgroundColor(faceRowBg(r.color))
                  .border({ width: 1, color: r.color })
                  .onClick(() => this.onFaceDetail?.(r.tid > 0 ? r.tid : r.idx))
              }.width('100%').alignItems(VerticalAlign.Center)
            }, (r: FaceHudRow) => `${r.idx}_${r.tid}_${r.text}_${r.color}`)
          }.width('100%').margin({ top: 5 })
        }

        // ── YOLO 合并结果（最多显示 4 类）：彩点 + 类名 + 数量 + 平均置信度 ──
        if (this.yoloEnabled && this.mergedList.length > 0) {
          Divider().color('rgba(255,255,255,0.1)').margin({ top: 1, bottom: 1 })
          Column({ space: 2 }) {
            ForEach(this.mergedList.slice(0, 4), (item: Merged) => {
              Row({ space: 5 }) {
                Row().width(5).height(5).borderRadius(2.5)
                  .backgroundColor(['#FF3B30', '#34C759', '#0A84FF', '#FF9500'][item.label % 4])
                Text(item.name).fontSize(11).fontColor('#FFF').fontWeight(FontWeight.Bold).layoutWeight(1)
                Text(`x${item.count}`).fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
                Text(`${(item.avgScore * 100).toFixed(0)}%`).fontSize(10).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
              }.width('100%')
            },
              // ★ 核心修复：把动态变化的值（count 和 score）拼进 Key 里
              // 这样一旦数字变了，ArkUI 就会认为这是一个“新元素”，强制重绘！
              (item: Merged, idx: number) => `${idx}_${item.count}_${item.avgScore.toFixed(2)}`)
          }.width('100%')
        }

        Divider().color('rgba(255,255,255,0.1)').margin({ top: 1, bottom: 1 })

        // ── 运动等级：数值行（越阈值变红点）+ 阈值刻度进度条（越线即触发） ──
        if (this.motionEnabled) {
          Row({ space: 5 }) {
            Row().width(5).height(5).borderRadius(2.5)
              .backgroundColor(this.motionLevel >= this.motionSens ? '#FF3B30' : '#34C759')
            Text('运动').fontSize(10).fontColor('#FFF')
            // ★ 灵敏度跟在"运动"后面：方便查看当前触发阈值（不用进设置页）
            Text(`灵敏度 ${Math.round(this.motionSens * 100)}%`)
              .fontSize(9).fontColor('rgba(255,255,255,0.65)')
            Blank()
            Text(`${Math.round(this.motionLevel * 100)}%`)
              .fontSize(10).fontColor('#FFF').fontWeight(FontWeight.Bold)
          }.width('100%')

          // ★ 进度条上叠加灵敏度阈值刻度：当前运动值越过刻度即触发
          Stack({ alignContent: Alignment.Start }) {
            Row().width('100%').height(3).borderRadius(1.5).backgroundColor('rgba(255,255,255,0.2)')
            Row().width(`${this.motionLevel * 100}%`).height(3).borderRadius(1.5).backgroundColor('#FF3B30')
            Row().width(1.5).height(9)
              .backgroundColor('#FFFFFF')
              .position({ x: `${this.motionSens * 100}%`, y: -3 })
          }.width('100%')
        }

        // ── 水深：当前读数行 + 最近 3 次历史记录（最新在上，可回看） ──
        if (this.depthEnabled) {
          Column() {
            Row({ space: 5 }) {
              Row().width(5).height(5).borderRadius(2.5)
                .backgroundColor('#00BFFF')
              Text('水深').fontSize(10).fontColor('#FFF')
              Blank()
              Text(`${this.depthCm.toFixed(0)} cm`)
                .fontSize(10).fontColor('#FFF').fontWeight(FontWeight.Bold)
            }.width('100%')

            // ★ 最近 3 次水深记录+时间：最新在上，读数历史可回看
            ForEach(this.depthHistory, (r: DepthRecord) => {
              Row({ space: 5 }) {
                Text(r.time).fontSize(9).fontColor('rgba(255,255,255,0.5)')
                Blank()
                Text(`${r.cm.toFixed(0)} cm`).fontSize(9).fontColor('rgba(160,220,255,0.85)')
              }.width('100%')
            }, (r: DepthRecord) => `${r.time}_${r.cm}`)
          }.width('100%')
        }

        // ── 文字识别：摘要行（识别完成="x行共x字·点击查看" / 识别中="done/total"）
        //    + 前 5 行文本预览；整块点击弹出全文弹窗 ──
        if (this.ocrEnabled) {
          Divider().color('rgba(255,255,255,0.1)').margin({ top: 1, bottom: 1 })

          Column() {
            Row({ space: 5 }) {
              Row().width(5).height(5).borderRadius(2.5).backgroundColor('#00E5FF')
              Text('文字识别').fontSize(10).fontColor('#FFF')
              // ★ 当前模型集短名跟在"文字识别"后：一眼看出是哪个模型
              if (this.ocrModelName.length > 0) {
                Text(this.ocrModelName).fontSize(9).fontColor('rgba(255,255,255,0.6)')
              }
              Blank()
              if (this.ocrText.length > 0) {
                // ★ 识别完成摘要："x行共x字"（行数=非空行，字数=去掉换行符后的字符总数），
                //   "点击查看"作为尾巴提示可点开全文弹窗
                Text(`${this.ocrText.split('\n').length}行共${this.ocrText.replace(/\n/g, '').length}字 · 点击查看`)
                  .fontSize(9).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .constraintSize({ maxWidth: '55%' })
              } else {
                // ★ 识别中：旋转进度徽标；识别进度（done/total）只在这里显示，不画在画面上
                Row({ space: 4 }) {
                  LoadingProgress().width(10).height(10).color('#00E5FF')
                  Text(this.ocrTotal > 0 ? `识别中 ${this.ocrDone}/${this.ocrTotal}` : '识别中')
                    .fontSize(9).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
                }
                .padding({ left: 7, right: 7, top: 2, bottom: 2 })
                .borderRadius(8)
                .backgroundColor('rgba(0,229,255,0.12)')
                .border({ width: 1, color: 'rgba(0,229,255,0.35)' })
              }
            }.width('100%')

            if (this.ocrText.length > 0) {
              Column({ space: 1 }) {
                ForEach(this.ocrText.split('\n').slice(0, 5), (line: string) => {
                  Text(line.length > 0 ? line : ' ')
                    .fontSize(10)
                    .fontColor('#00E5FF')
                    .maxLines(1)
                    .textOverflow({ overflow: TextOverflow.Ellipsis })
                    .width('100%')
                }, (line: string, idx: number) => `${idx}_${line}`)
              }.width('100%')
            }
          }
          .width('100%')
          .onClick(() => this.onShowOcrPopup?.())
        }
        }
        .width('100%')
        .opacity(this.hudOpacity)
        }
      }
      // ★ 顶部菜单：100% 全宽、无边框；顶部偏移=状态栏安全区高度+小间距，
      //   不覆盖系统信号/电量等信息栏
      .width('100%')
      .padding({ left: 10, right: 10, top: 2, bottom: 8 })
      .backgroundColor(`rgba(17, 21, 25, ${this.hudOpacity})`)
      .position({ x: 0, y: this.hudTopOffset })

    }
    .width('100%').height('100%')
    // ★ 必须 Transparent：本容器 100%×100% 全屏，Default 会响应全屏命中测试
    //   并阻塞所有低 z 兄弟节点（预览触摸覆盖层/捏合缩放）——真机注入实证：
    //   HUD 内元素点击正常、覆盖层 onClick 从不触发，根因即在此
    .hitTestBehavior(HitTestMode.Transparent)
  }

  // ★ 摄像头切换胶囊：选中半透明彩色底+彩边+彩字（每类专属配色）；
  //   busy（切换/加载中）或 USB 未连接时半透明禁点，防止状态错乱
  @Builder
  camPill(text: string, pos: number, activeColor: string, activeBg: string) {
    Row({ space: 4 }) {
      if (this.cameraPosition !== pos) Row().width(5).height(5).borderRadius(2.5).backgroundColor('#8E8E93')
      Text(text).fontSize(11)
        .fontColor(this.cameraPosition === pos ? activeColor : '#8E8E93')
        .fontWeight(FontWeight.Bold)
    }
    // ★ 选中样式与"运动"开启的胶囊一致：半透明彩色底 + 彩色描边 + 彩色字（每类保留专属配色）
    .padding({ left: 10, right: 10, top: 4, bottom: 4 })
    .borderRadius(13)
    .backgroundColor(this.cameraPosition === pos ? activeBg : 'rgba(0,0,0,0.55)')
    .border({ width: 1, color: this.cameraPosition === pos ? activeColor : 'rgba(255,255,255,0.25)' })
    .opacity(this.busy ? 0.45 : (pos === 2 && !this.usbConnected ? 0.45 : 1.0))
    .onClick(() => {
      // ★ 切换/加载期间禁点：不响应、不改变选中态
      if (this.busy) return;
      this.onCameraSelect?.(pos);
    })
  }
}
