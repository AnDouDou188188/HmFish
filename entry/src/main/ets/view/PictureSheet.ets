// ★ 画面设置弹窗：识别框/文字开关、USB 画面增强（三模式 + 亮度/锐度/对比度/
//   饱和度滑条 + AI 细节强度）、人脸识别开关（SCRFD + 106 关键点 + 性别年龄颜值）
@Component
export struct PictureSheet {
  // ★ 识别框/标签画面显示开关：关闭后画面干净不画框/文字（识别与自动录像不受影响）
  @Prop drawBoxesOn: boolean = true;
  // ★ USB 画面增强（仅 USB 摄像头画面生效）：
  //   模式 0关/1非AI(亮度+锐度+对比度+饱和度)/2 AI超分(QuickSRNetMedium x4 + 精修锐化)
  @Prop usbBrightness: number = 0;
  @Prop usbSharpness: number = 0;
  @Prop usbContrast: number = 0;
  @Prop usbSaturation: number = 0;
  @Prop usbEnhanceMode: number = 0;
  @Prop usbSrStrength: number = 100;
  // ★ 人脸识别开关：SCRFD 检测 + 106 关键点连线框 + 性别年龄颜值标注
  @Prop faceEnabled: boolean = false;

  onClose?: () => void;
  onDrawBoxesToggle?: () => void;
  onUsbModeChange?: (m: number) => void;
  onUsbBrightnessChange?: (v: number) => void;
  onUsbSharpnessChange?: (v: number) => void;
  onUsbContrastChange?: (v: number) => void;
  onUsbSaturationChange?: (v: number) => void;
  onUsbSrStrengthChange?: (v: number) => void;
  onFaceToggle?: () => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localUsbBright: number = 0;
  @State localUsbSharp: number = 0;
  @State localUsbContrast: number = 0;
  @State localUsbSaturation: number = 0;
  @State localSrStrength: number = 100;

  aboutToAppear(): void {
    this.localUsbBright = this.usbBrightness;
    this.localUsbSharp = this.usbSharpness;
    this.localUsbContrast = this.usbContrast;
    this.localUsbSaturation = this.usbSaturation;
    this.localSrStrength = this.usbSrStrength;
    setTimeout(() => { this.appear = true; }, 30);
  }

  @Builder
  SectionTitle(icon: string, title: string, extra: string) {
    Row({ space: 6 }) {
      Text(icon).fontSize(13).fontColor('#00E5FF')
      Text(title).fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
      Blank()
      if (extra.length > 0) {
        Text(extra).fontSize(11).fontColor('#8E97A3')
      }
    }
    .width('100%')
    .margin({ top: 20, bottom: 10 })
  }

  // ★ USB 增强模式 pill：选中高亮，点击切换（0关/1非AI/2 AI超分）
  @Builder
  UsbModePill(label: string, mode: number) {
    Text(label)
      .fontSize(12)
      .fontColor(this.usbEnhanceMode === mode ? '#FFFFFF' : '#B7BDC7')
      .fontWeight(this.usbEnhanceMode === mode ? FontWeight.Bold : FontWeight.Normal)
      .padding({ left: 12, right: 12, top: 7, bottom: 7 })
      .backgroundColor(this.usbEnhanceMode === mode ? 'rgba(0,229,255,0.22)' : 'rgba(255,255,255,0.07)')
      .borderRadius(14)
      .border({
        width: this.usbEnhanceMode === mode ? 1 : 0.5,
        color: this.usbEnhanceMode === mode ? '#00E5FF' : 'rgba(255,255,255,0.15)'
      })
      .margin({ right: 8, bottom: 8 })
      .onClick(() => {
        if (mode !== this.usbEnhanceMode) this.onUsbModeChange?.(mode);
      })
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
          Text('画面设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          Blank()
          Text('✕').fontSize(15).fontColor('#8E97A3')
            .padding({ left: 14, right: 14, top: 6, bottom: 6 })
            .onClick(() => this.onClose?.())
        }
        .width('100%')
        .padding({ left: 20, right: 10, top: 12 })

        Scroll() {
          Column() {
            // ★ 画面显示：识别框和文字开关（关闭后画面干净不画框/文字）
            Row({ space: 8 }) {
              Text('🎛').fontSize(14).fontColor('#00E5FF')
              Text('显示识别框和文字').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Text(this.drawBoxesOn ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.drawBoxesOn ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.drawBoxesOn })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.drawBoxesOn) this.onDrawBoxesToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 12 })

            Text('关闭后画面不绘制识别框和标签文字，画面更干净；识别、朗读、自动录像不受影响')
              .fontSize(10).fontColor('#8E97A3')
              .width('100%').margin({ top: 6 })

            // ★ 人脸识别开关：106 关键点连线框 + 性别年龄颜值标注
            Row({ space: 8 }) {
              Text('☺').fontSize(14).fontColor('#00E5FF')
              Text('人脸识别').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Text(this.faceEnabled ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.faceEnabled ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.faceEnabled })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.faceEnabled) this.onFaceToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 20 })

            Text('识别画面中的人脸：绘制 106 关键点连线框，标注约年龄、性别与颜值（0~100），HUD 同步显示')
              .fontSize(10).fontColor('#8E97A3')
              .width('100%').margin({ top: 6 })

            // ★ USB 画面增强：仅 USB 摄像头画面生效（3 模式：关/非AI/AI超分）
            this.SectionTitle('▣', 'USB 画面增强', '仅 USB 摄像头')

            Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
              this.UsbModePill('关闭', 0)
              this.UsbModePill('非AI增强', 1)
              this.UsbModePill('AI超分', 2)
            }
            .width('100%')
            .margin({ top: 12, bottom: 2 })

            if (this.usbEnhanceMode === 0) {
              Text('关闭所有 USB 画面增强处理，保持原始画面')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })
            } else if (this.usbEnhanceMode === 1) {
              Text('亮度/锐度/对比度/饱和度调节，不占用 AI 算力')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              Row({ space: 6 }) {
                Text('☀').fontSize(13).fontColor('#00E5FF')
                Text('画面亮度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localUsbBright > 0 ? `+${this.localUsbBright.toFixed(0)}` :
                  `${this.localUsbBright.toFixed(0)}`)
                  .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 14, bottom: 8 })

              Row({ space: 10 }) {
                Text('调暗').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localUsbBright, min: -50, max: 50, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localUsbBright = v;
                    // ★ 松手提交：立即生效并保存
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onUsbBrightnessChange?.(v);
                    }
                  })
                Text('调亮').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Row({ space: 6 }) {
                Text('🔺').fontSize(13).fontColor('#00E5FF')
                Text('画面锐度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localUsbSharp > 0 ? `+${this.localUsbSharp.toFixed(0)}` : '关闭')
                  .fontSize(13).fontColor(this.localUsbSharp > 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('柔和').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localUsbSharp, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localUsbSharp = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onUsbSharpnessChange?.(v);
                    }
                  })
                Text('锐利').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Row({ space: 6 }) {
                Text('◐').fontSize(13).fontColor('#00E5FF')
                Text('画面对比度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localUsbContrast > 0 ? `+${this.localUsbContrast.toFixed(0)}` :
                  (this.localUsbContrast < 0 ? `${this.localUsbContrast.toFixed(0)}` : '关闭'))
                  .fontSize(13).fontColor(this.localUsbContrast !== 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('低').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localUsbContrast, min: -50, max: 50, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localUsbContrast = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onUsbContrastChange?.(v);
                    }
                  })
                Text('高').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Row({ space: 6 }) {
                Text('🎨').fontSize(13).fontColor('#00E5FF')
                Text('画面饱和度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localUsbSaturation > 0 ? `+${this.localUsbSaturation.toFixed(0)}` :
                  (this.localUsbSaturation < 0 ? `${this.localUsbSaturation.toFixed(0)}` : '关闭'))
                  .fontSize(13).fontColor(this.localUsbSaturation !== 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('素').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localUsbSaturation, min: -50, max: 50, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localUsbSaturation = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onUsbSaturationChange?.(v);
                    }
                  })
                Text('艳').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')
            } else {
              Text('AI画质增强（v2 显示分辨率超分）：AI 细节 0~220% 增益注入原图 + 双重锐化加强档，效果直观可辨')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              Row({ space: 6 }) {
                Text('🎛').fontSize(13).fontColor('#00E5FF')
                Text('AI 细节增强强度').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(`${this.localSrStrength.toFixed(0)}%`)
                  .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 14, bottom: 8 })

              Row({ space: 10 }) {
                Text('关闭').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localSrStrength, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localSrStrength = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onUsbSrStrengthChange?.(v);
                    }
                  })
                Text('最强').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Text('原始画面细节零损失；AI 细节按强度 0~220% 增益叠加（软限幅防爆点），叠加双重锐化加强档；强度 0=关闭 AI；过慢时自动降档保持流畅')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })
            }
          }
          .width('100%')
          .padding({ left: 20, right: 20, bottom: 30 })
        }
        .scrollBar(BarState.Off)
        .width('100%')
        .layoutWeight(1)
        .align(Alignment.Center)
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
