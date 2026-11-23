// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ MotionSheet（MotionSheet.ets）—— 运动检测设置半模态弹窗                       ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  长按底部"运动"按钮弹出。配置运动检测两项参数：                               ║
// ║   1. 运动检测总开关（Toggle）                                                ║
// ║   2. 触发灵敏度滑条 5%~95% step5（默认 35%，持久化）：画面运动幅度越过        ║
// ║      阈值时 HUD 标红并触发自动录像                                           ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  Index 通过 @Prop 下发当前值；修改立即经回调上抛（Index 落盘 + native 热更新）；║
// ║  灵敏度用 localSensPct 本地缓冲（百分比单位），松手才按 0~1 提交，             ║
// ║  避免拖动过程中 @Prop 实时回写打断滑条                                       ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
/**
 * 运动设置弹窗：运动检测开关、触发灵敏度（默认 20%，持久化）
 * 样式照抄 ModelSheet（识别设置）：底部弹窗 + 开关行 + 滑条 + 完成按钮
 */
// ★ MotionSheet：运动设置弹窗主体（总开关 + 灵敏度滑条两段）
@Component
export struct MotionSheet {
  // ★ 当前配置（Index 持有真值下发）：运动检测开关/触发灵敏度（0~1，滑条按百分比展示）
  @Prop motionEnabled: boolean = false;
  @Prop motionSens: number = 0.35;

  // ── 交互回调：关闭/开关切换/灵敏度提交（松手才触发，按 0~1 上抛） ──
  onClose?: () => void;
  onMotionToggle?: () => void;
  onSensChange?: (v: number) => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值（避免 @Prop 实时回写打断拖动），松手才提交父组件
  @State localSensPct: number = 35;

  // ★ 入场：百分比本地缓冲对齐父状态；延时 30ms 置 appear 触发上滑动画
  aboutToAppear(): void {
    this.localSensPct = this.motionSens * 100;
    setTimeout(() => { this.appear = true; }, 30);
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      // ── 全屏半透明遮罩：appear 驱动 220ms 淡入，点击任意处关闭 ──
      Column()
        .width('100%').height('100%')
        .backgroundColor(this.appear ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0)')
        .animation({ duration: 220, curve: Curve.EaseOut })
        .onClick(() => this.onClose?.())

      // ── 底部面板容器：translate 620→0 上滑入场，限高 82% ──
      Column() {
        Row()
          .width(42).height(4).borderRadius(2)
          .backgroundColor('rgba(255,255,255,0.25)')
          .margin({ top: 10 })

        Row() {
          Text('运动设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          Blank()
          Text('✕').fontSize(15).fontColor('#8E97A3')
            .padding({ left: 14, right: 14, top: 6, bottom: 6 })
            .onClick(() => this.onClose?.())
        }
        .width('100%')
        .padding({ left: 20, right: 10, top: 12 })

        // ★ 确认配置置顶：不用滑到底部就能关弹窗
        Button('确认配置')
          .width('92%').height(40)
          .margin({ top: 10 })
          .fontSize(14).fontWeight(FontWeight.Bold)
          .fontColor('#062A33')
          .backgroundColor(Color.Transparent)
          .linearGradient({
            angle: 90,
            colors: [['#00E5FF', 0], ['#4DF0FF', 1]]
          })
          .borderRadius(20)
          .onClick(() => this.onClose?.())

        // ── 运动检测总开关行 ──
        Column() {
          Row({ space: 8 }) {
            Text('运动检测').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Medium)
            Text(this.motionEnabled ? '已开启' : '已关闭').fontSize(11)
              .fontColor(this.motionEnabled ? '#00E5FF' : '#6E7681')
            Blank()
            Toggle({ type: ToggleType.Switch, isOn: this.motionEnabled })
              .selectedColor('#00E5FF')
              .onChange((isOn: boolean) => {
                if (isOn !== this.motionEnabled) this.onMotionToggle?.();
              })
          }
          .width('100%')
          .margin({ top: 12 })

          // ── 触发灵敏度滑条：5%~95% step5，松手才按 0~1 提交父组件 ──
          Row({ space: 6 }) {
            Text('◐').fontSize(13).fontColor('#00E5FF')
            Text('触发灵敏度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
            Blank()
            Text(`${this.localSensPct.toFixed(0)}%`)
              .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
          }
          .width('100%')
          .margin({ top: 18, bottom: 10 })

          Row({ space: 10 }) {
            Text('5%').fontSize(10).fontColor('#6E7681')
            Slider({ value: this.localSensPct, min: 5, max: 95, step: 5 })
              .layoutWeight(1)
              .selectedColor('#00E5FF')
              .trackColor('rgba(255,255,255,0.15)')
              .blockColor('#FFFFFF')
              .onChange((v: number, mode: SliderChangeMode) => {
                this.localSensPct = v;
                // ★ 松手（End/Click）才提交：拖动中不回写父状态，保存一次到位
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onSensChange?.(v / 100);
                }
              })
            Text('95%').fontSize(10).fontColor('#6E7681')
          }
          .width('100%')

          Text('画面运动幅度达到灵敏度阈值时，HUD 标红并触发自动录像')
            .fontSize(10).fontColor('#8E97A3')
            .width('100%').margin({ top: 8 })
        }
        .width('100%')
        .padding({ left: 20, right: 20, bottom: 30 })
      }
      .width('100%')
      .constraintSize({ maxHeight: '82%' })
      .backgroundColor('#16191F')
      .borderRadius({ topLeft: 24, topRight: 24 })
      .translate({ y: this.appear ? 0 : 620 })
      .animation({ duration: 280, curve: Curve.EaseOut })
    }
    .width('100%').height('100%')
  }
}
