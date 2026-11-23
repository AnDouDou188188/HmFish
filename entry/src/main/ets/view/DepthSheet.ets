// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ DepthSheet（DepthSheet.ets）—— 水深设置半模态弹窗                             ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  长按底部"水深"按钮弹出。配置水深检测的三项参数：                             ║
// ║   1. 水深检测总开关（Toggle）                                                ║
// ║   2. 水的浑浊度三选一（清澈/微浑/浑浊）——影响下沉速度估算                    ║
// ║   3. 摄像头重量滑条 10~300g step5——与浑浊度共同决定下沉速度模型              ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  Index 通过 @Prop 下发当前配置；修改立即经 onXxx 回调上抛（Index 落盘并      ║
// ║  热更新 native 管线）；重量滑条用 localWeightG 本地缓冲，拖动期间不打扰       ║
// ║  @Prop，松手（End/Click）才提交，避免拖动被父状态回写打断                    ║
// ║                                                                              ║
// ║ 【视觉结构】                                                                 ║
// ║  全屏遮罩（点击关闭，220ms 淡入）+ 底部上滑面板（拖拽条/标题行/确认按钮      ║
// ║  置顶常显/内容区）；appear 状态驱动遮罩淡入 + 面板 translate 上滑入场         ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
/**
 * 水深设置弹窗：水深检测开关、水的浑浊度、摄像头重量(g)
 * 深度按"入水→触底"过程积分估算：重量与浑浊度决定下沉速度，
 * 数据线随深度增长阻力增大而持续减速，画面静止后锁定深度
 * 样式照抄 MotionSheet：底部弹窗 + 开关行 + 选择行 + 滑条 + 完成按钮
 */
// ★ DepthSheet：水深设置弹窗主体（开关/浑浊度/重量三段配置）
@Component
export struct DepthSheet {
  // ★ 当前配置（Index 持有真值下发）：检测开关/浑浊度档位(0~2)/摄像头重量(g)
  @Prop depthEnabled: boolean = false;
  @Prop depthTurbidity: number = 1;
  @Prop depthWeightG: number = 30;

  // ── 交互回调：关闭/开关切换/浑浊度档位选择/重量滑条提交（松手才触发） ──
  onClose?: () => void;
  onDepthToggle?: () => void;
  onTurbidityChange?: (idx: number) => void;
  onWeightChange?: (g: number) => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localWeightG: number = 30;

  // ★ 入场：本地缓冲对齐父状态；延时 30ms 置 appear，让面板先完成布局再播上滑动画
  aboutToAppear(): void {
    this.localWeightG = this.depthWeightG;
    setTimeout(() => { this.appear = true; }, 30);
  }

  // ★ 浑浊度选项胶囊：选中青底深字、未选中灰底浅字，layoutWeight(1) 三等分整行
  @Builder
  TurbOption(idx: number, label: string) {
    Text(label)
      .fontSize(13)
      .fontColor(this.depthTurbidity === idx ? '#062A33' : '#EDEFF2')
      .fontWeight(FontWeight.Bold)
      .textAlign(TextAlign.Center)
      .layoutWeight(1)
      .height(34)
      .borderRadius(17)
      .backgroundColor(this.depthTurbidity === idx ? '#00E5FF' : 'rgba(255,255,255,0.08)')
      .onClick(() => this.onTurbidityChange?.(idx))
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      // ── 全屏半透明遮罩：appear 驱动 220ms 淡入，点击任意处关闭 ──
      Column()
        .width('100%').height('100%')
        .backgroundColor(this.appear ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0)')
        .animation({ duration: 220, curve: Curve.EaseOut })
        .onClick(() => this.onClose?.())

      // ── 底部面板容器：translate 620→0 上滑入场，限高 82% 防满屏 ──
      Column() {
        Row()
          .width(42).height(4).borderRadius(2)
          .backgroundColor('rgba(255,255,255,0.25)')
          .margin({ top: 10 })

        Row() {
          Text('水深设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
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

        // ── 水深检测总开关行 ──
        Column() {
          Row({ space: 8 }) {
            Text('水深检测').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Medium)
            Text(this.depthEnabled ? '已开启' : '已关闭').fontSize(11)
              .fontColor(this.depthEnabled ? '#00E5FF' : '#6E7681')
            Blank()
            Toggle({ type: ToggleType.Switch, isOn: this.depthEnabled })
              .selectedColor('#00E5FF')
              .onChange((isOn: boolean) => {
                if (isOn !== this.depthEnabled) this.onDepthToggle?.();
              })
          }
          .width('100%')
          .margin({ top: 12 })

          // ── 浑浊度三选一：点击整颗胶囊切换档位（回调上抛档位下标） ──
          Row({ space: 6 }) {
            Text('≈').fontSize(13).fontColor('#00E5FF')
            Text('水的浑浊度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
          }
          .width('100%')
          .margin({ top: 18 })

          Row({ space: 8 }) {
            this.TurbOption(0, '清澈')
            this.TurbOption(1, '微浑')
            this.TurbOption(2, '浑浊')
          }
          .width('100%')
          .margin({ top: 10 })

          // ── 摄像头重量滑条：10~300g step5，松手才提交父组件 ──
          Row({ space: 6 }) {
            Text('⚖').fontSize(13).fontColor('#00E5FF')
            Text('摄像头重量').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
            Blank()
            Text(`${this.localWeightG.toFixed(0)}g`)
              .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
          }
          .width('100%')
          .margin({ top: 18, bottom: 10 })

          Row({ space: 10 }) {
            Text('10g').fontSize(10).fontColor('#6E7681')
            Slider({ value: this.localWeightG, min: 10, max: 300, step: 5 })
              .layoutWeight(1)
              .selectedColor('#00E5FF')
              .trackColor('rgba(255,255,255,0.15)')
              .blockColor('#FFFFFF')
              .onChange((v: number, mode: SliderChangeMode) => {
                this.localWeightG = v;
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onWeightChange?.(v);
                }
              })
            Text('300g').fontSize(10).fontColor('#6E7681')
          }
          .width('100%')

          Text('按入水→触底过程估算水深：画面呈水体特征（蓝色主导）持续确认后才开始测量，重量与浑浊度决定下沉速度，数据线随深度增长阻力增大而减速，画面静止后锁定深度；离开水面自动复位重新测量')
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
