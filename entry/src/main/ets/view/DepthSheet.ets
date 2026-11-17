/**
 * 水深设置弹窗：水深检测开关、水的浑浊度、摄像头重量(g)
 * 深度按"入水→触底"过程积分估算：重量与浑浊度决定下沉速度，
 * 数据线随深度增长阻力增大而持续减速，画面静止后锁定深度
 * 样式照抄 MotionSheet：底部弹窗 + 开关行 + 选择行 + 滑条 + 完成按钮
 */
@Component
export struct DepthSheet {
  @Prop depthEnabled: boolean = false;
  @Prop depthTurbidity: number = 1;
  @Prop depthWeightG: number = 30;

  onClose?: () => void;
  onDepthToggle?: () => void;
  onTurbidityChange?: (idx: number) => void;
  onWeightChange?: (g: number) => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localWeightG: number = 30;

  aboutToAppear(): void {
    this.localWeightG = this.depthWeightG;
    setTimeout(() => { this.appear = true; }, 30);
  }

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
      Column()
        .width('100%').height('100%')
        .backgroundColor(this.appear ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0)')
        .animation({ duration: 220, curve: Curve.EaseOut })
        .onClick(() => this.onClose?.())

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
