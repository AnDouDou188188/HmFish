/**
 * 记录设置弹窗：随时保存（开始/立即保存当前录像）、自动录像开关、
 * 录像时长 5~30 秒滑动选择
 * 样式照抄 MotionSheet：底部弹窗 + 操作按钮 + 开关行 + 滑条 + 完成按钮
 * 触发音效已挪到识别设置（ModelSheet）
 */
@Component
export struct RecordSheet {
  @Prop isRecording: boolean = false;
  @Prop autoRecordOn: boolean = false;
  @Prop recordDurationSec: number = 10;

  onClose?: () => void;
  onSaveNow?: () => void;
  onAutoRecordToggle?: () => void;
  onDurationChange?: (sec: number) => void;

  @State appear: boolean = false;
  // ★ 滑条本地缓冲：拖动期间只更新本地值，松手才提交父组件
  @State localDurationSec: number = 10;

  aboutToAppear(): void {
    this.localDurationSec = this.recordDurationSec;
    setTimeout(() => { this.appear = true; }, 30);
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
          Text('记录设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
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
          // ★ 随时保存：未录像=开始录像；录像中=不等时长、立即停止并保存
          Button(this.isRecording ? '立即保存当前录像' : '开始录像')
            .width('100%').height(44)
            .margin({ top: 14 })
            .fontSize(15).fontWeight(FontWeight.Bold)
            .fontColor(this.isRecording ? '#FFFFFF' : '#062A33')
            .backgroundColor(Color.Transparent)
            .linearGradient({
              angle: 90,
              colors: this.isRecording
                ? [['#FF5252', 0], ['#FF7853', 1]]
                : [['#00E5FF', 0], ['#4DF0FF', 1]]
            })
            .borderRadius(22)
            .onClick(() => this.onSaveNow?.())

          Row({ space: 8 }) {
            Text('自动录像').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Medium)
            Text(this.autoRecordOn ? '已开启' : '已关闭').fontSize(11)
              .fontColor(this.autoRecordOn ? '#00E5FF' : '#6E7681')
            Blank()
            Toggle({ type: ToggleType.Switch, isOn: this.autoRecordOn })
              .selectedColor('#00E5FF')
              .onChange((isOn: boolean) => {
                if (isOn !== this.autoRecordOn) this.onAutoRecordToggle?.();
              })
          }
          .width('100%')
          .margin({ top: 18 })

          Row({ space: 6 }) {
            Text('⏱').fontSize(13).fontColor('#00E5FF')
            Text('录像时长').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
            Blank()
            Text(`${this.localDurationSec.toFixed(0)}秒`)
              .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
          }
          .width('100%')
          .margin({ top: 16, bottom: 10 })

          Row({ space: 10 }) {
            Text('5秒').fontSize(10).fontColor('#6E7681')
            Slider({ value: this.localDurationSec, min: 5, max: 30, step: 5 })
              .layoutWeight(1)
              .selectedColor('#00E5FF')
              .trackColor('rgba(255,255,255,0.15)')
              .blockColor('#FFFFFF')
              .onChange((v: number, mode: SliderChangeMode) => {
                this.localDurationSec = v;
                if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
                  this.onDurationChange?.(v);
                }
              })
            Text('30秒').fontSize(10).fontColor('#6E7681')
          }
          .width('100%')

          Text('识别到勾选类别或运动超阈值时按设定时长自动录像；录像中可随时点"立即保存"不等时长直接保存')
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
