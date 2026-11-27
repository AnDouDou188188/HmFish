// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ BottomPanel（BottomPanel.ets）—— 底部控制面板                                ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  相机全屏预览最下方的半透明操作面板，对齐 Android 原生相机底栏布局：           ║
// ║   1. 位置行：地名展示 + 复制入口（点击弹复制地址/坐标弹窗）                    ║
// ║   2. 变焦区：当前倍数气泡（>1x 青色高亮）+ 摄像头参数 + 变焦滑杆              ║
// ║   3. 功能行：保存图片(SaveButton 安全组件)/旋转方向/识别/水深/运动 胶囊按钮    ║
// ║   4. 底排：相册缩略图入口 + 中央快门(录像) + 自动记录开关                     ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  纯"哑组件"：所有状态由 Index 持有并经 @Link/@Prop 下发，用户操作一律经       ║
// ║  onXxx 回调上抛，由 Index 决定开关/弹窗/启停；短按=切换开关、                 ║
// ║  长按=弹出对应设置弹窗（对齐 Android 交互习惯）                               ║
// ║                                                                              ║
// ║ 【关键契约】                                                                 ║
// ║  导出 PANEL_OCCUPIED_VP（面板屏幕内占用高度），Index 用它计算预览可见区域，   ║
// ║  位置水印必须画在面板顶边之上；快门按钮不得自行翻转 isRecording——             ║
// ║  开关状态由 Index.toggleRecord 内部翻转，按钮先行翻转会导致其读到已翻转        ║
// ║  状态走错分支（启动即"停止"）                                                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 底部控制面板：拍照/录像等主操作区（对齐 Android 底栏布局）
import { CaptureMediaView } from './CaptureMediaView';
import { image } from '@kit.ImageKit';

// ══════════════════ 底排调参区（改数值即可，单位 px） ══════════════════
const ALBUM_BTN_SIZE = 34;      // 相册 / 记录 外框边长（两侧统一）
const SHUTTER_SIZE = 46;        // 中间快门圆直径
const ROW_GAP_GEO = 4;          // 地址行 距面板顶
const ROW_GAP_ZOOM = 6;         // 变焦气泡行 距地址行
const ROW_GAP_SLIDER_TOP = 2;   // 滑杆行 距上
const ROW_GAP_SLIDER_BOTTOM = 2;// 滑杆行 距下
const ROW_GAP_BUTTONS = 10;      // 功能按钮行 距底排
const SLIDER_HEIGHT = 30;       // 变焦滑杆高度
const PANEL_HEIGHT = 222;       // 面板总高（含地址行 26）
const PANEL_PAD_BOTTOM = 50;    // 底 padding（内容距屏幕底 = 50-16 = 34，避开手势条）
const PANEL_TUCK = 16;          // 面板背景下探屏幕外像素（视觉全宽贴底）
// ★ 供 Index 计算预览可见区域（位置水印需画在面板顶边之上）
export const PANEL_OCCUPIED_VP = PANEL_HEIGHT - PANEL_TUCK; // 面板在屏幕内占用的总高
// ═════════════════════════════════════════════════════════════════════

// ★ 照抄 Android DemoFragment updateRecognizeButtonUI / updateMotionButtonUI /
//   updateDepthButtonUI：文本 "识别 ●/○"，开启 #00E676（水深 #00BFFF）、关闭 #D7DAE1
// ★ 胶囊按钮效果：整排（保存图片→运动）统一 半透明底+圆角+描边，开启时换高亮底色
@Component
struct ActionButton {
  @Prop text: string = '';
  @Prop isToggle: boolean = false;
  @Prop isActive: boolean = false;
  @Prop activeColor: string = '#00E676';
  // ★ 开启时的胶囊底色（activeColor 同系半透明）
  @Prop activeBg: string = 'rgba(0,230,118,0.28)';
  onTap?: () => void;
  // ★ 照抄 Android：短按=切换开关（toggleRecognize/toggleDepth/toggleMotion），
  //   长按=弹出配置菜单（showRecognizeSettingsDialog/showDepthSettingsDialog/showMotionSettingsDialog）
  onLongPress?: () => void;

  build() {
    Text(this.isToggle ? `${this.text} ${this.isActive ? '●' : '○'}` : this.text)
      .fontSize(13)
      // ★ 默认态用较深灰色（纯白太刺眼），开启态才用高亮色
      .fontColor(this.isActive ? this.activeColor : (this.isToggle ? '#8E8E93' : '#C7CBD1'))
      .fontWeight(this.isActive ? FontWeight.Bold : FontWeight.Normal)
      .padding({ left: 10, right: 10, top: 5, bottom: 5 })
      .borderRadius(14)
      .backgroundColor(this.isActive ? this.activeBg : 'rgba(0,0,0,0.55)')
      .border({ width: 1, color: this.isActive ? this.activeColor : 'rgba(255,255,255,0.25)' })
      .gesture(GestureGroup(GestureMode.Exclusive,
        TapGesture().onAction(() => this.onTap?.()),
        LongPressGesture({ repeat: false, duration: 500 }).onAction(() => this.onLongPress?.())))
  }
}

// ★ 照抄 Android lensFacingBtn1 + bg_rec_toggle_on/off：
//   开 = "●\n记录" #00E676 / 底 #CC004400 / 边 #00E676；
//   关 = "○\n记录" #D7DAE1 / 底 #80000000 / 边 #FFFFFF；40dp、圆角 5、边 1.2、9sp Bold
// ★ 与识别/水深/运动一致：单击=开关自动记录，长按=弹记录设置菜单
@Component
struct AutoRecordToggle {
  @Prop armed: boolean = false;
  onToggle?: () => void;
  onLongPress?: () => void;
  @State pressScale: number = 1;
  @State pressAlpha: number = 1;

  build() {
    Text(this.armed ? '●\n记录' : '○\n记录')
      .fontSize(9)
      // ★ 默认态深灰（与识别/水深/运动一致），开启态绿色高亮
      .fontColor(this.armed ? '#00E676' : '#8E8E93')
      .fontWeight(FontWeight.Bold)
      .textAlign(TextAlign.Center)
      .lineHeight(11)
      .width(ALBUM_BTN_SIZE)
      .height(ALBUM_BTN_SIZE)
      .backgroundColor(this.armed ? '#CC004400' : 'rgba(0,0,0,0.55)')
      .borderRadius(5)
      .border({ width: 1.2, color: this.armed ? '#00E676' : 'rgba(255,255,255,0.25)' })
      .scale({ x: this.pressScale, y: this.pressScale })
      .opacity(this.pressAlpha)
      .gesture(GestureGroup(GestureMode.Exclusive,
        TapGesture().onAction(() => {
          // ★ 照抄 Android clickAnimation：scale+alpha 1.0→0.4→1.0，共 150ms，动画结束才触发
          this.getUIContext().animateTo({ duration: 75, curve: Curve.EaseOut }, () => {
            this.pressScale = 0.4;
            this.pressAlpha = 0.4;
          });
          setTimeout(() => {
            this.getUIContext().animateTo({ duration: 75, curve: Curve.EaseIn }, () => {
              this.pressScale = 1;
              this.pressAlpha = 1;
            });
          }, 75);
          setTimeout(() => { this.onToggle?.(); }, 150);
        }),
        LongPressGesture({ repeat: false, duration: 500 }).onAction(() => {
          this.onLongPress?.();
        })))
  }
}

// ★ BottomPanel：底部面板主体。自上而下组装 位置行/变焦区/功能行/底排 四层；
//   @Link 双向同步录像与变焦（Index 持有真值），@Prop 单向下发展示数据，
//   全部操作经 onXxx 回调交由 Index 处理
@Component
export struct BottomPanel {
  // ★ 双向绑定：录像开关/变焦倍数/相册最新缩略图（滑杆拖动直接回写 zoomRatio）
  @Link isRecording: boolean;
  @Link zoomRatio: number;
  @Link albumThumb: image.PixelMap | undefined;

  // ★ 单向下发展示：旋转方向文案（水平/垂直/反转/反向）、当前拍摄模式（截图等）、
  //   录像计时秒数（-1=未录像）、自动记录开关、位置地名、变焦上限、摄像头参数
  @Prop rotateText: string;
  @Link yoloEnabled: boolean;
  @Link motionEnabled: boolean;
  @Link depthEnabled: boolean;
  @Prop currentMode: string;
  @Prop recordSec: number = -1;
  @Prop autoRecordOn: boolean = false;
  // ★ 位置行（移自顶部 HUD）：地名展示，点击弹复制地址/坐标
  @Prop geoName: string = '';
  // ★ 语音命令开关（地址栏麦克风按钮高亮态；点击由 Index 处理开关切换）
  @Prop voiceMicOn: boolean = false;
  // ★ 唤醒词：显示在麦克风芯片上（"🎤钓鱼"），一眼可知说什么唤醒
  @Prop voiceWakeWord: string = '';
  // ★ 变焦上限按相机支持范围动态设定：手机用 getZoomRatioRange 上限，USB 默认 4x
  @Prop zoomMax: number = 4.0;
  // ★ 摄像头主要参数（预览分辨率）：变焦气泡旁展示
  @Prop cameraInfo: string = '';

  // ── 与 Index 的交互回调：录像启停/自动记录/截图/旋转/相册/各功能开关与长按
  //    弹设置/变焦提交/位置弹窗，全部由 Index 实现业务逻辑 ──
  onToggleRecord?: () => void;
  onAutoRecordToggle?: () => void;
  onAutoRecordLongPress?: () => void;
  onSnapshot?: () => void;
  onRotate?: () => void;
  onShowToast?: (msg: string) => void;
  onAlbumPreview?: () => void;
  onYoloToggle?: () => void;
  onYoloLongPress?: () => void;
  onMotionToggle?: () => void;
  onMotionLongPress?: () => void;
  onDepthToggle?: () => void;
  onDepthLongPress?: () => void;
  onZoomChange?: (v: number) => void;
  onShowGeoPopup?: () => void;
  onVoiceMicTap?: () => void;

  // ★ SideBtn：36×36 侧栏小按钮（图标+文字，深色底白边小方块），
  //   当前布局未使用，保留为扩展入口
  @Builder
  SideBtn(icon: string, label: string, onTap: () => void) {
    Column({ space: 0 }) {
      Text(icon).fontSize(11).fontColor('#D7DAE1').fontWeight(FontWeight.Bold).lineHeight(10)
      Text(label).fontSize(8).fontColor('#D7DAE1').fontWeight(FontWeight.Bold).lineHeight(10)
    }
    .width(36).height(36)
    .justifyContent(FlexAlign.Center)
    .backgroundColor('rgba(0,0,0,0.5)')
    .borderRadius(5)
    .border({ width: 1.2, color: '#FFFFFF' })
    .onClick(onTap)
  }

  build() {
    Column() {
      // ★ 位置行（移自顶部 HUD）：地名 + 语音麦克风开关 + 复制胶囊，
      //   置于变焦气泡行上一行；麦克风为语音命令手动开关（识别设置内亦有总开关）
      if (this.geoName.length > 0 || this.voiceMicOn) {
        Row({ space: 5 }) {
          if (this.geoName.length > 0) {
            Text('📍').fontSize(12)
          }
          if (this.geoName.length > 0) {
            Text(this.geoName)
              .fontSize(12).fontColor('#D8DCE2')
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
              .layoutWeight(1)
          } else {
            Blank()
          }
          Row({ space: 3 }) {
            Text('🎤')
              .fontSize(12)
            if (this.voiceMicOn && this.voiceWakeWord.length > 0) {
              Text(this.voiceWakeWord)
                .fontSize(10).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
            }
          }
            .padding({ left: 8, right: 8, top: 3, bottom: 3 })
            .borderRadius(11)
            .backgroundColor(this.voiceMicOn ? 'rgba(0,229,255,0.30)' : 'rgba(0,0,0,0.5)')
            .border({ width: 1, color: this.voiceMicOn ? '#00E5FF' : 'rgba(255,255,255,0.35)' })
            .opacity(this.voiceMicOn ? 1 : 0.75)
            .onClick(() => this.onVoiceMicTap?.())
          if (this.geoName.length > 0) {
            Text('复制')
              .fontSize(11).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
              .padding({ left: 12, right: 12, top: 4, bottom: 4 })
              .borderRadius(12)
              .backgroundColor('rgba(0,229,255,0.18)')
              .border({ width: 1, color: '#00E5FF' })
          }
        }
        .width('90%')
        .padding({ top: 3, bottom: 3 })
        .margin({ top: ROW_GAP_GEO })
        .onClick(() => this.onShowGeoPopup?.())
      }

      // ★ 变焦当前倍数 + 摄像头主要参数：变焦条上方居中展示（>1x 青色高亮）
      Row({ space: 8 }) {
        Text(`${this.zoomRatio.toFixed(1)}x`)
          .fontSize(11).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          .padding({ left: 9, right: 9, top: 2, bottom: 2 })
          .borderRadius(9)
          .backgroundColor(this.zoomRatio > 1.001 ? 'rgba(0,229,255,0.30)' : 'rgba(0,0,0,0.5)')
          .border({ width: 1, color: this.zoomRatio > 1.001 ? '#00E5FF' : 'rgba(255,255,255,0.35)' })
        if (this.cameraInfo.length > 0) {
          Text(this.cameraInfo)
            .fontSize(9).fontColor('rgba(255,255,255,0.55)').fontWeight(FontWeight.Bold)
        }
      }
      .width('90%').justifyContent(FlexAlign.Center).margin({ top: ROW_GAP_ZOOM })

      // ── 变焦滑杆行：1x~zoomMax，拖动实时回写 @Link zoomRatio 并回调 Index 生效 ──
      Row({ space: 10 }) {
        Text('1x').fontSize(10).fontColor('#8E8E93')
        Slider({ value: this.zoomRatio, min: 1.0, max: this.zoomMax, step: 0.1 })
          .selectedColor('#00E5FF').trackColor('rgba(255,255,255,0.2)').blockColor('#FFFFFF')
          .height(SLIDER_HEIGHT)
          .layoutWeight(1)
          .onChange((v: number) => {
            const clamped = Math.min(v, this.zoomMax);
            this.zoomRatio = clamped;
            this.onZoomChange?.(clamped);
          })
        Text(`${this.zoomMax.toFixed(0)}x`).fontSize(10).fontColor('#8E8E93')
      }
      .width('90%').margin({ top: ROW_GAP_SLIDER_TOP, bottom: ROW_GAP_SLIDER_BOTTOM })

      // ── 功能按钮行：保存图片 / 旋转方向 / 识别 / 水深 / 运动（短按开关、长按设置）──
      Row({ space: 8 }) {
        SaveButton({
          text: SaveDescription.SAVE_IMAGE,
          buttonType: ButtonType.Capsule
        })
          .width(56)
          .height(28)
          .fontSize(12).fontColor('#FFFFFF')
          // ★ 胶囊按钮效果：整排统一半透明底；选中态（截图模式）换绿色高亮底
          //   SaveButton 为安全组件不支持 border，用底色区分即可
          .backgroundColor(this.currentMode === '截图' ? 'rgba(0,230,118,0.35)' : 'rgba(0,0,0,0.5)')
          .borderRadius(14)
          .onClick((event: ClickEvent, result: SaveButtonOnClickResult) => {
            if (result === SaveButtonOnClickResult.SUCCESS) {
              setTimeout(() => {
                this.onSnapshot?.();
              }, 0);
            } else {
              setTimeout(() => { this.onShowToast?.('授权失败，请重试'); }, 0);
            }
          })

        // ★ "水平"（0°）= ● 选中高亮（同运动开启态）；垂直/反转/反向 = ○ 关闭灰
        ActionButton({ text: this.rotateText, isToggle: true,
          isActive: this.rotateText === '水平',
          activeColor: '#00E676', activeBg: 'rgba(0,230,118,0.28)',
          onTap: () => this.onRotate?.() })
        // ★ 短按=开关（Android toggleRecognize），长按=识别设置（showRecognizeSettingsDialog）
        ActionButton({ text: '识别', isToggle: true, isActive: this.yoloEnabled,
          onTap: () => this.onYoloToggle?.(), onLongPress: () => this.onYoloLongPress?.() })
        // ★ 短按=开关（Android toggleDepth），长按=水深设置（浑浊度/重量）
        ActionButton({ text: '水深', isToggle: true, isActive: this.depthEnabled, activeColor: '#00BFFF',
          activeBg: 'rgba(0,191,255,0.28)',
          onTap: () => this.onDepthToggle?.(), onLongPress: () => this.onDepthLongPress?.() })
        // ★ 短按=开关（Android toggleMotion），长按=运动设置（灵敏度）
        ActionButton({ text: '运动', isToggle: true, isActive: this.motionEnabled,
          onTap: () => this.onMotionToggle?.(), onLongPress: () => this.onMotionLongPress?.() })
      }
      .width('100%').justifyContent(FlexAlign.SpaceAround).margin({ bottom: ROW_GAP_BUTTONS })

      // ── 底排：相册缩略图入口 | 中央快门（只负责录像启停）| 自动记录开关 ──
      Row() {
        Column() {
          Stack() {
            if (this.albumThumb) {
              Image(this.albumThumb)
                .width(ALBUM_BTN_SIZE).height(ALBUM_BTN_SIZE)
                .borderRadius(5)
                .objectFit(ImageFit.Cover)
            } else {
              Column() {
                Text('⊞').fontSize(10).fontColor('#8E8E93').fontWeight(FontWeight.Bold).lineHeight(9)
                Text('相册').fontSize(7).fontColor('#8E8E93').fontWeight(FontWeight.Bold).lineHeight(9)
              }
              .justifyContent(FlexAlign.Center)
            }
          }
          .width(ALBUM_BTN_SIZE).height(ALBUM_BTN_SIZE)
          .borderRadius(5)
          .backgroundColor('rgba(0,0,0,0.55)')
          .border({ width: 1.2, color: 'rgba(255,255,255,0.25)' })
          .clip(true)
          .onClick(() => this.onAlbumPreview?.())
        }.layoutWeight(1).alignItems(HorizontalAlign.Center).justifyContent(FlexAlign.Center)

        Stack() {
          CaptureMediaView({ currentMode: this.currentMode, isRecording: this.isRecording, recordSec: this.recordSec })
        }
        .width(SHUTTER_SIZE).height(SHUTTER_SIZE)
        .onClick(() => {
          // ★ 照抄 Android captureBtn：中间圆形按钮只负责录像——点击开始/录像中再点停止。
          //   开关状态由 Index.toggleRecord 内部翻转（begin/endRecording 设置 isRecording），
          //   按钮不得先行翻转，否则 toggleRecord 读到已翻转状态走错分支（启动即"停止"）
          this.onToggleRecord?.();
        })

        Column() {
          AutoRecordToggle({
            armed: this.autoRecordOn,
            onToggle: (): void => { this.onAutoRecordToggle?.(); },
            onLongPress: (): void => { this.onAutoRecordLongPress?.(); }
          })
        }.layoutWeight(1).alignItems(HorizontalAlign.Center).justifyContent(FlexAlign.Center)
      }
      .width('100%').alignItems(VerticalAlign.Center)
    }
    // ★ 底排避开系统导航条：内容距屏幕底 50-16=34px（手势条区域），
    //   面板背景保留 16px 下探到屏幕外，视觉仍全宽贴底；
    //   行距/滑杆高度整体收紧，少占 22px 画面
    .width('100%').padding({ top: 0, bottom: PANEL_PAD_BOTTOM }).height(PANEL_HEIGHT)
    // ★ 半透明面板：摄像头画面完整透出（按钮自带深色底保证可读），HUD 透明度不变
    .backgroundColor('rgba(0,0,0,0.45)')
    .position({ x: 0, y: '100%' }).translate({ y: -(PANEL_HEIGHT - PANEL_TUCK) })
  }
}