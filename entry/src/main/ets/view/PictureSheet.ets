// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ PictureSheet（PictureSheet.ets）—— 画面设置半模态弹窗                         ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  点顶部"画面"徽标弹出。集中管理"画面长什么样"与"画不画框"：                    ║
// ║   1. 显示识别框和文字开关（只管画不画，识别/朗读/自动录像不受影响）            ║
// ║   2. 人脸识别开关（SCRFD + 106 关键点 + 性别年龄颜值标注）                     ║
// ║   3. 画面增强三模式（USB + 手机前后置全部相机生效）：                          ║
// ║      非 AI：亮度/锐度/对比度/饱和度四滑条 + 水下推荐值一键恢复（不占算力）      ║
// ║      AI：超分(QuickSRNet)/降噪(联合双边滤波)/水下去浊(暗通道先验)/             ║
// ║           色彩还原(灰世界白平衡) 四模块独立滑条，修复层异步合成不卡预览        ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  全部 @Prop 下发 + onXxx 上抛（Index 落盘并热更新 native 管线）；             ║
// ║  八个滑条各自本地缓冲、松手提交；"完成"按钮 commitAndClose 兜底提交当前        ║
// ║  模式全部滑条值（防拖动中断丢值）再关闭弹窗                                   ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 画面设置弹窗：识别框/文字开关、画面增强（三模式 + 亮度/锐度/对比度/
//   饱和度滑条 + 恢复水下默认值 + AI 四模块：超分/降噪/去浊/色彩）、人脸识别开关
// ★ PictureSheet：画面设置弹窗主体（显示开关/人脸/增强三模式条件分段渲染）
@Component
export struct PictureSheet {
  // ★ 识别框/标签画面显示开关：关闭后画面干净不画框/文字（识别与自动录像不受影响）
  @Prop drawBoxesOn: boolean = true;
  // ★ 画面增强（USB + 手机前后置全部相机生效）：
  //   模式 0关/1非AI(亮度+锐度+对比度+饱和度)/2 AI增强(超分+降噪+去浊+色彩还原)
  @Prop usbBrightness: number = 0;
  @Prop usbSharpness: number = 0;
  @Prop usbContrast: number = 0;
  @Prop usbSaturation: number = 0;
  @Prop usbEnhanceMode: number = 0;
  @Prop usbSrStrength: number = 100;
  // ★ AI 增强附加模块（mode 2 内独立滑条 0~100，修复层异步合成）
  @Prop aiDenoise: number = 50;
  @Prop aiClarity: number = 50;
  @Prop aiColorRestore: number = 60;
  // ★ 人脸识别开关：SCRFD 检测 + 106 关键点连线框 + 性别年龄颜值标注
  @Prop faceEnabled: boolean = false;

  // ── 交互回调：关闭/画框开关/模式切换/八滑条提交/水下默认值/人脸开关 ──
  onClose?: () => void;
  onDrawBoxesToggle?: () => void;
  onUsbModeChange?: (m: number) => void;
  onUsbBrightnessChange?: (v: number) => void;
  onUsbSharpnessChange?: (v: number) => void;
  onUsbContrastChange?: (v: number) => void;
  onUsbSaturationChange?: (v: number) => void;
  onUsbSrStrengthChange?: (v: number) => void;
  onUsbResetDefaults?: () => void;
  onAiDenoiseChange?: (v: number) => void;
  onAiClarityChange?: (v: number) => void;
  onAiColorRestoreChange?: (v: number) => void;
  onFaceToggle?: () => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localUsbBright: number = 0;
  @State localUsbSharp: number = 0;
  @State localUsbContrast: number = 0;
  @State localUsbSaturation: number = 0;
  @State localSrStrength: number = 100;
  @State localAiDenoise: number = 50;
  @State localAiClarity: number = 50;
  @State localAiColorRestore: number = 60;

  // ★ 入场：八个滑条本地缓冲对齐父状态；延时 30ms 置 appear 触发上滑动画
  aboutToAppear(): void {
    this.localUsbBright = this.usbBrightness;
    this.localUsbSharp = this.usbSharpness;
    this.localUsbContrast = this.usbContrast;
    this.localUsbSaturation = this.usbSaturation;
    this.localSrStrength = this.usbSrStrength;
    this.localAiDenoise = this.aiDenoise;
    this.localAiClarity = this.aiClarity;
    this.localAiColorRestore = this.aiColorRestore;
    setTimeout(() => { this.appear = true; }, 30);
  }

  // ★ 完成：提交当前模式全部本地滑条值（兜底防拖动中断丢值）并关闭弹窗
  commitAndClose(): void {
    if (this.usbEnhanceMode === 1) {
      if (this.localUsbBright !== this.usbBrightness) this.onUsbBrightnessChange?.(this.localUsbBright);
      if (this.localUsbSharp !== this.usbSharpness) this.onUsbSharpnessChange?.(this.localUsbSharp);
      if (this.localUsbContrast !== this.usbContrast) this.onUsbContrastChange?.(this.localUsbContrast);
      if (this.localUsbSaturation !== this.usbSaturation) this.onUsbSaturationChange?.(this.localUsbSaturation);
    } else if (this.usbEnhanceMode === 2) {
      if (this.localSrStrength !== this.usbSrStrength) this.onUsbSrStrengthChange?.(this.localSrStrength);
      if (this.localAiDenoise !== this.aiDenoise) this.onAiDenoiseChange?.(this.localAiDenoise);
      if (this.localAiClarity !== this.aiClarity) this.onAiClarityChange?.(this.localAiClarity);
      if (this.localAiColorRestore !== this.aiColorRestore) this.onAiColorRestoreChange?.(this.localAiColorRestore);
    }
    this.onClose?.();
  }

  // ★ 分区标题行：图标 + 标题 + 右侧补充说明（extra 为空不渲染占位）
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
      // ── 全屏半透明遮罩：appear 驱动 220ms 淡入，点击任意处关闭 ──
      Column()
        .width('100%').height('100%')
        .backgroundColor(this.appear ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0)')
        .animation({ duration: 220, curve: Curve.EaseOut })
        .onClick(() => this.onClose?.())

      // ── 底部面板容器：translate 620→0 上滑入场，限高 82%，底部"完成"常显 ──
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

        // ── 滚动配置区：按 usbEnhanceMode 条件渲染 非 AI 四滑条 / AI 四模块 ──
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

            // ★ 画面增强：USB + 手机前后置全部相机生效（3 模式：关/非AI/AI增强）
            this.SectionTitle('▣', '画面增强', '全部相机生效')

            Flex({ wrap: FlexWrap.Wrap, alignItems: ItemAlign.Center }) {
              this.UsbModePill('关闭', 0)
              this.UsbModePill('非AI增强', 1)
              this.UsbModePill('AI增强', 2)
            }
            .width('100%')
            .margin({ top: 12, bottom: 2 })

            if (this.usbEnhanceMode === 0) {
              Text('关闭所有画面增强处理，保持原始画面')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })
            } else if (this.usbEnhanceMode === 1) {
              Text('亮度/锐度/对比度/饱和度调节，不占用 AI 算力')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ★ 恢复默认值（水下场景优化）：一键回到推荐参数（清晰优先，饱和度适度）
              Row({ space: 8 }) {
                Text('💧').fontSize(13).fontColor('#00E5FF')
                Text('水下推荐：亮度+15 · 锐度60 · 对比度+20 · 饱和度+10')
                  .fontSize(10).fontColor('#8E97A3').layoutWeight(1)
                Text('恢复默认').fontSize(12).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
                  .padding({ left: 12, right: 12, top: 6, bottom: 6 })
                  .backgroundColor('rgba(0,229,255,0.15)')
                  .borderRadius(12)
                  .border({ width: 1, color: 'rgba(0,229,255,0.5)' })
                  .onClick(() => {
                    // 本地滑条立即跳到默认值 + 提交父组件
                    this.localUsbBright = 15;
                    this.localUsbSharp = 60;
                    this.localUsbContrast = 20;
                    this.localUsbSaturation = 10;
                    this.onUsbResetDefaults?.();
                  })
              }
              .width('100%')
              .margin({ top: 10, bottom: 2 })

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
              Text('AI 增强（修复层异步架构）：四个模块独立调节、互不干扰，全部后台异步合成，画面丝滑不卡顿（USB + 手机前后置均生效）')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ── 模块 1：AI 超分细节 ──
              Row({ space: 6 }) {
                Text('🎛').fontSize(13).fontColor('#00E5FF')
                Text('AI 超分细节').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localSrStrength > 0 ? `${this.localSrStrength.toFixed(0)}%` : '关闭')
                  .fontSize(13).fontColor(this.localSrStrength > 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
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

              Text('QuickSRNet 超分：原始细节零损失，AI 细节按强度 0~220% 增益叠加 + 双重锐化加强档')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ── 模块 2：AI 降噪 ──
              Row({ space: 6 }) {
                Text('🔇').fontSize(13).fontColor('#00E5FF')
                Text('AI 降噪').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localAiDenoise > 0 ? `${this.localAiDenoise.toFixed(0)}%` : '关闭')
                  .fontSize(13).fontColor(this.localAiDenoise > 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('关闭').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localAiDenoise, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localAiDenoise = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onAiDenoiseChange?.(v);
                    }
                  })
                Text('最强').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Text('以超分输出（无噪）为引导做联合双边滤波：抹平暗光噪点，保留真实边缘纹理')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ── 模块 3：AI 水下去浊 ──
              Row({ space: 6 }) {
                Text('🌊').fontSize(13).fontColor('#00E5FF')
                Text('AI 水下去浊').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localAiClarity > 0 ? `${this.localAiClarity.toFixed(0)}%` : '关闭')
                  .fontSize(13).fontColor(this.localAiClarity > 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('关闭').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localAiClarity, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localAiClarity = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onAiClarityChange?.(v);
                    }
                  })
                Text('最强').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Text('暗通道先验去雾：对抗水下浑浊散射，提升透视感与层次（水质越浑效果越明显）')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ── 模块 4：AI 色彩还原 ──
              Row({ space: 6 }) {
                Text('🎨').fontSize(13).fontColor('#00E5FF')
                Text('AI 色彩还原').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(this.localAiColorRestore > 0 ? `${this.localAiColorRestore.toFixed(0)}%` : '关闭')
                  .fontSize(13).fontColor(this.localAiColorRestore > 0 ? '#00E5FF' : '#6E7681')
                  .fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 16, bottom: 8 })

              Row({ space: 10 }) {
                Text('关闭').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localAiColorRestore, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localAiColorRestore = v;
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onAiColorRestoreChange?.(v);
                    }
                  })
                Text('最强').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Text('灰世界白平衡：拉回水下偏绿/偏蓝色偏，补偿红通道衰减，色彩自然不艳俗')
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

        // ★ 底部固定"完成"按钮：提交当前模式全部滑条值并关闭
        Column() {
          Row() {
            Text('完成')
              .fontSize(15)
              .fontColor('#06131A')
              .fontWeight(FontWeight.Bold)
          }
          .justifyContent(FlexAlign.Center)
          .width('100%')
          .height(46)
          .backgroundColor('#00E5FF')
          .borderRadius(23)
          .onClick(() => this.commitAndClose())
        }
        .width('100%')
        .padding({ left: 20, right: 20, top: 8, bottom: 22 })
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
