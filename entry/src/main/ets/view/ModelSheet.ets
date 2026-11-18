// ★ 识别设置弹窗：YOLO 模型选择、自动录像类别、置信度阈值、
//   语音播报/识别朗读/触发音效（画面增强/识别框/人脸已迁至画面设置弹窗）
import { ModelInfo } from '../common/ModelManager';

/**
 * 识别设置弹窗：模型切换、自动录像类别勾选、置信度阈值
 */
@Component
export struct ModelSheet {
  @Prop models: ModelInfo[] = [];
  @Prop currentModelId: string = '';
  @Prop labels: string[] = [];
  @Prop checkedLabels: number[] = [];
  @Prop confThresh: number = 0.25;
  @Prop yoloEnabled: boolean = true;
  // ★ 语音播报：开关 + 音量（0~100，静音模式下自动不播）
  @Prop speechOn: boolean = true;
  @Prop speechVolume: number = 30;
  // ★ YOLO 识别结果朗读（独立开关，默认关闭）：只控制"识别到xx N条"的实时朗读
  @Prop detectSpeakOn: boolean = false;
  // ★ 触发音效：自动录像触发时的提示音（默认内置报警音，可选自定义 MP3，可试听）
  @Prop recordSoundOn: boolean = true;
  @Prop recordSoundName: string = '';

  onClose?: () => void;
  onSelectModel?: (id: string) => void;
  onToggleLabel?: (idx: number) => void;
  onConfChange?: (v: number) => void;
  onYoloToggle?: () => void;
  onSpeechToggle?: () => void;
  onSpeechVolumeChange?: (v: number) => void;
  onDetectSpeakToggle?: () => void;
  onSoundToggle?: () => void;
  onPickSound?: () => void;
  onPreviewSound?: () => void;
  onResetSound?: () => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localConfPct: number = 25;
  @State localVolume: number = 30;

  aboutToAppear(): void {
    this.localConfPct = this.confThresh * 100;
    this.localVolume = this.speechVolume;
    setTimeout(() => { this.appear = true; }, 30);
  }

  private isChecked(idx: number): boolean {
    return this.checkedLabels.indexOf(idx) >= 0;
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

  @Builder
  ModelCard(m: ModelInfo) {
    Column({ space: 5 }) {
      Row({ space: 4 }) {
        if (m.id === this.currentModelId) {
          Text('✓').fontSize(12).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
        }
        Text(m.name).fontSize(13)
          .fontColor(m.id === this.currentModelId ? '#FFFFFF' : '#B7BDC7')
          .fontWeight(m.id === this.currentModelId ? FontWeight.Bold : FontWeight.Normal)
      }
      Text(`${m.id} · ${m.inputSize}px`).fontSize(10)
        .fontColor(m.id === this.currentModelId ? '#00E5FF' : '#6E7681')
    }
    .alignItems(HorizontalAlign.Start)
    .padding({ left: 14, right: 14, top: 11, bottom: 11 })
    .borderRadius(12)
    .backgroundColor(m.id === this.currentModelId ? 'rgba(0,229,255,0.12)' : 'rgba(255,255,255,0.05)')
    .border({
      width: 1.2,
      color: m.id === this.currentModelId ? '#00E5FF' : 'rgba(255,255,255,0.08)'
    })
    .onClick(() => this.onSelectModel?.(m.id))
  }

  @Builder
  LabelChip(name: string, idx: number) {
    Row({ space: 4 }) {
      if (this.isChecked(idx)) {
        Text('✓').fontSize(11).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
      }
      Text(name).fontSize(12)
        .fontColor(this.isChecked(idx) ? '#FFFFFF' : '#B7BDC7')
    }
    .padding({ left: 10, right: 10, top: 6, bottom: 6 })
    .borderRadius(14)
    .backgroundColor(this.isChecked(idx) ? '#0090AC' : 'rgba(255,255,255,0.06)')
    .border({
      width: 1,
      color: this.isChecked(idx) ? '#00E5FF' : 'rgba(255,255,255,0.08)'
    })
    .margin(3)
    .onClick(() => this.onToggleLabel?.(idx))
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
          Text('识别设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          Blank()
          Text('✕').fontSize(15).fontColor('#8E97A3')
            .padding({ left: 14, right: 14, top: 6, bottom: 6 })
            .onClick(() => this.onClose?.())
        }
        .width('100%')
        .padding({ left: 20, right: 10, top: 12 })

        // ★ 确认配置置顶（滚动区外常显）：不用滑到底部就能关弹窗
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

        Scroll() {
          Column() {
            Row({ space: 8 }) {
              Text('YOLO 识别').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Medium)
              Text(this.yoloEnabled ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.yoloEnabled ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.yoloEnabled })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.yoloEnabled) this.onYoloToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 12 })

            Row({ space: 6 }) {
              Text('◐').fontSize(13).fontColor('#00E5FF')
              Text('置信度阈值').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Blank()
              Text(`${this.localConfPct.toFixed(0)}%`)
                .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
            }
            .width('100%')
            .margin({ top: 16, bottom: 10 })
            Row({ space: 10 }) {
              Text('5%').fontSize(10).fontColor('#6E7681')
              Slider({ value: this.localConfPct, min: 5, max: 95, step: 5 })
                .layoutWeight(1)
                .selectedColor('#00E5FF')
                .trackColor('rgba(255,255,255,0.15)')
                .blockColor('#FFFFFF')
                .onChange((v: number, mode: SliderChangeMode) => {
                  this.localConfPct = v;
                  if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                    this.onConfChange?.(v / 100);
                  }
                })
              Text('95%').fontSize(10).fontColor('#6E7681')
            }
            .width('100%')

            // ★ 语音播报：点击按钮/设置切换/识别结果语音反馈；静音模式自动禁声
            Row({ space: 8 }) {
              Text('🔊').fontSize(14).fontColor('#00E5FF')
              Text('语音播报').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Text(this.speechOn ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.speechOn ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.speechOn })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.speechOn) this.onSpeechToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 20 })

            if (this.speechOn) {
              Row({ space: 6 }) {
                Text('🔉').fontSize(13).fontColor('#00E5FF')
                Text('播报音量').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Blank()
                Text(`${this.localVolume.toFixed(0)}%`)
                  .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
              }
              .width('100%')
              .margin({ top: 14, bottom: 8 })

              Row({ space: 10 }) {
                Text('0%').fontSize(10).fontColor('#6E7681')
                Slider({ value: this.localVolume, min: 0, max: 100, step: 5 })
                  .layoutWeight(1)
                  .selectedColor('#00E5FF')
                  .trackColor('rgba(255,255,255,0.15)')
                  .blockColor('#FFFFFF')
                  .onChange((v: number, mode: SliderChangeMode) => {
                    this.localVolume = v;
                    // ★ 松手提交：父组件保存并播报一句示例，让用户听到实际音量
                    if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                      this.onSpeechVolumeChange?.(v);
                    }
                  })
                Text('100%').fontSize(10).fontColor('#6E7681')
              }
              .width('100%')

              Text('播报按钮操作与设置变更；手机静音/震动时不播报任何语音')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })

              // ★ YOLO 识别结果朗读（独立开关，默认关闭）：
              //   开启后实时朗读"识别到草鱼3条、鲫鱼1条"
              Row({ space: 8 }) {
                Text('📢').fontSize(14).fontColor('#00E5FF')
                Text('识别朗读').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
                Text(this.detectSpeakOn ? '已开启' : '已关闭').fontSize(11)
                  .fontColor(this.detectSpeakOn ? '#00E5FF' : '#6E7681')
                Blank()
                Toggle({ type: ToggleType.Switch, isOn: this.detectSpeakOn })
                  .selectedColor('#00E5FF')
                  .onChange((isOn: boolean) => {
                    if (isOn !== this.detectSpeakOn) this.onDetectSpeakToggle?.();
                  })
              }
              .width('100%')
              .margin({ top: 14 })

              Text('开启后实时朗读识别结果，如"识别到草鱼3条、鲫鱼1条"')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })
            }

            // ★ 触发音效：自动录像触发时播放提示音（可自定义 MP3、可试听）
            Row({ space: 8 }) {
              Text('🔔').fontSize(14).fontColor('#00E5FF')
              Text('触发音效').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Text(this.recordSoundOn ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.recordSoundOn ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.recordSoundOn })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.recordSoundOn) this.onSoundToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 20 })

            if (this.recordSoundOn) {
              Row({ space: 8 }) {
                Text('♪').fontSize(13).fontColor('#00E5FF')
                Text('当前音效').fontSize(12).fontColor('#B7BDC7')
                Text(this.recordSoundName.length > 0 ? this.recordSoundName : '默认提示音')
                  .fontSize(12).fontColor(this.recordSoundName.length > 0 ? '#00E5FF' : '#8E97A3')
                  .constraintSize({ maxWidth: 92 })
                  .maxLines(1)
                  .textOverflow({ overflow: TextOverflow.Ellipsis })
                  .layoutWeight(1)
                // ★ 试听：播放当前音效（自定义 MP3 或默认提示音）
                Button('试听')
                  .fontSize(12).fontColor('#062A33').backgroundColor('#00E5FF')
                  .borderRadius(12).height(28)
                  .onClick((): void => { this.onPreviewSound?.(); })
                Button('选择 MP3')
                  .fontSize(12).fontColor('#D7DAE1').backgroundColor('rgba(255,255,255,0.08)')
                  .borderRadius(12).height(28)
                  .onClick((): void => { this.onPickSound?.(); })
                if (this.recordSoundName.length > 0) {
                  Button('恢复默认')
                    .fontSize(12).fontColor('#8E97A3').backgroundColor('rgba(255,255,255,0.08)')
                    .borderRadius(12).height(28)
                    .onClick((): void => { this.onResetSound?.(); })
                }
              }
              .width('100%')
              .margin({ top: 12 })

              Text('自动录像触发时播放；试听立即播放当前音效；手机静音时不播放提示音')
                .fontSize(10).fontColor('#8E97A3')
                .width('100%').margin({ top: 6 })
            }

            this.SectionTitle('◆', '识别模型', '点击切换')

            Scroll() {
              Row({ space: 10 }) {
                ForEach(this.models, (m: ModelInfo) => {
                  this.ModelCard(m)
                }, (m: ModelInfo) => `${m.id}_${m.id === this.currentModelId ? 1 : 0}`)
              }
              .padding({ right: 10 })
            }
            .scrollable(ScrollDirection.Horizontal)
            .scrollBar(BarState.Off)
            .width('100%')

            Row({ space: 6 }) {
              Text('◉').fontSize(13).fontColor('#00E5FF')
              Text('自动录像类别').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Blank()
              Text(this.checkedLabels.length > 0 ? `已选 ${this.checkedLabels.length} 项` : '未选择')
                .fontSize(11).fontColor('#8E97A3')
            }
            .width('100%')
            .margin({ top: 20, bottom: 10 })
            if (this.labels.length === 0) {
              Text('当前模型暂无类别信息').fontSize(11).fontColor('#6E7681')
                .width('100%').textAlign(TextAlign.Center).padding(10)
            } else {
              Column({ space: 0 }) {
                Text('勾选后，画面中出现所选类别将自动录像并保存到相册')
                  .fontSize(10).fontColor('#8E97A3').margin({ bottom: 6 })
                Flex({ wrap: FlexWrap.Wrap }) {
                  ForEach(this.labels, (name: string, idx: number) => {
                    this.LabelChip(name, idx)
                  }, (name: string, idx: number) => `${idx}_${name}_${this.isChecked(idx) ? 1 : 0}`)
                }
                .width('100%')
              }
              .width('100%')
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
