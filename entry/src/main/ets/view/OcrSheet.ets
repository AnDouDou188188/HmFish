// ★ OCR 设置弹窗：模型集选择与识别参数配置
import { OcrModelSet } from '../common/OcrManager';

/**
 * 文字识别设置弹窗（长按 OCR 弹出）：
 *   1) 模型集选择：PaddleOCR 普通文本 / Xiaomi-OCR 票据格式（目录存在即列出）
 *   2) det 阈值三件套滑条（立即保存、立即生效）
 * 样式照抄 MotionSheet（运动设置）：底部弹窗 + 选项行 + 滑条 + 完成按钮
 */
@Component
export struct OcrSheet {
  @Prop ocrEnabled: boolean = false;
  @Prop ocrModelId: string = 'paddle';
  @Prop modelSets: OcrModelSet[] = [];
  @Prop @Watch('syncLocal') ocrDbScore: number = 0.5;
  @Prop @Watch('syncLocal') ocrDbBox: number = 0.3;
  @Prop @Watch('syncLocal') ocrUnclip: number = 1.6;

  onClose?: () => void;
  onOcrToggle?: () => void;
  onSelectSet?: (id: string) => void;
  onDbScoreChange?: (v: number) => void;
  onDbBoxChange?: (v: number) => void;
  onUnclipChange?: (v: number) => void;

  @State appear: boolean = false;
  // ★ 三滑条本地缓冲（与滑条同单位：%、%、×10）：拖动期间只更新本地值，松手才提交
  @State localVals: number[] = [50, 30, 16];
  // ★ 数值文案（@Builder 体内直读才刷新，不能按值传入）
  @State localLabels: string[] = ['50%', '30%', '1.6'];

  aboutToAppear(): void {
    this.syncLocal();
    setTimeout(() => { this.appear = true; }, 30);
  }

  private syncLocal(): void {
    this.localVals = [this.ocrDbScore * 100, this.ocrDbBox * 100, this.ocrUnclip * 10];
    this.localLabels = [this.fmtVal(0, this.localVals[0]), this.fmtVal(1, this.localVals[1]),
      this.fmtVal(2, this.localVals[2])];
  }

  // ★ 各滑条数值文案（2=文本框扩展一位小数，其余百分比）
  private fmtVal(idx: number, v: number): string {
    if (idx === 2) return v.toFixed(1);
    return `${v.toFixed(0)}%`;
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
          Text('文字识别设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
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
            // ───── 开关 ─────
            Row({ space: 8 }) {
              Text('文字识别').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Medium)
              Text(this.ocrEnabled ? '已开启' : '已关闭').fontSize(11)
                .fontColor(this.ocrEnabled ? '#00E5FF' : '#6E7681')
              Blank()
              Toggle({ type: ToggleType.Switch, isOn: this.ocrEnabled })
                .selectedColor('#00E5FF')
                .onChange((isOn: boolean) => {
                  if (isOn !== this.ocrEnabled) this.onOcrToggle?.();
                })
            }
            .width('100%')
            .margin({ top: 12 })

            // ───── 模型集选择 ─────
            Row({ space: 6 }) {
              Text('⌸').fontSize(13).fontColor('#00E5FF')
              Text('识别模型').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
            }
            .width('100%')
            .margin({ top: 18, bottom: 8 })

            ForEach(this.modelSets, (set: OcrModelSet) => {
              Row({ space: 8 }) {
                Text(this.ocrModelId === set.id ? '●' : '○')
                  .fontSize(12)
                  .fontColor(this.ocrModelId === set.id ? '#00E5FF' : '#6E7681')
                Text(set.name).fontSize(13)
                  .fontColor(this.ocrModelId === set.id ? '#EDEFF2' : '#8E97A3')
                  .fontWeight(FontWeight.Medium)
                  .layoutWeight(1)
                Text(this.ocrModelId === set.id ? '使用中' : '')
                  .fontSize(10).fontColor('#00E5FF')
              }
              .width('100%')
              .padding({ top: 8, bottom: 8, left: 8, right: 8 })
              .borderRadius(10)
              .backgroundColor(this.ocrModelId === set.id ? 'rgba(0,229,255,0.10)' : 'rgba(255,255,255,0.04)')
              .border({ width: 1, color: this.ocrModelId === set.id ? 'rgba(0,229,255,0.45)' : 'rgba(255,255,255,0.10)' })
              .margin({ top: 4 })
              .onClick(() => this.onSelectSet?.(set.id))
            }, (set: OcrModelSet) => `${set.id}_${this.ocrModelId === set.id ? 1 : 0}`)

            Text('票据场景推荐选 PP-OCRv5 票据高精度；新增自定义模型：把 det/cls/rec 模型和字典 txt 放入 应用文件/ocrmodels/<目录名> 即可自动识别')
              .fontSize(10).fontColor('#8E97A3')
              .width('100%').margin({ top: 8 })

            // ───── det 阈值三件套 ─────（idx 对应 localVals 下标，松手提交）
            this.sliderRow(0, '文字检测阈值', '偏低多检、偏高漏检',
              30, 70, 5, '30%', '70%',
              (v: number): void => { this.onDbScoreChange?.(v / 100); })

            this.sliderRow(1, '框置信度阈值', '过滤低质量的候选框',
              20, 60, 5, '20%', '60%',
              (v: number): void => { this.onDbBoxChange?.(v / 100); })

            this.sliderRow(2, '文本框扩展', '越大框越宽松、越小框越贴合',
              12, 22, 1, '1.2', '2.2',
              (v: number): void => { this.onUnclipChange?.(v / 10); })
          }
          .width('100%')
          .padding({ left: 20, right: 20, bottom: 30 })
        }
        .width('100%')
        .layoutWeight(1)
        .scrollBar(BarState.Auto)
        .align(Alignment.TopStart)
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

  // ★ 滑条行：数值/文案在体内直读 this.localVals/localLabels（状态直读才随拖动刷新）
  @Builder
  sliderRow(idx: number, title: string, hint: string,
    min: number, max: number, step: number,
    minLabel: string, maxLabel: string,
    onChange: (v: number) => void) {
    Column() {
      Row({ space: 6 }) {
        Text(title).fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
        Text(hint).fontSize(9).fontColor('#6E7681')
        Blank()
        Text(this.localLabels[idx])
          .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
      }
      .width('100%')
      .margin({ top: 16, bottom: 8 })

      Row({ space: 10 }) {
        Text(minLabel).fontSize(10).fontColor('#6E7681')
        Slider({ value: this.localVals[idx], min: min, max: max, step: step })
          .layoutWeight(1)
          .selectedColor('#00E5FF')
          .trackColor('rgba(255,255,255,0.15)')
          .blockColor('#FFFFFF')
          .onChange((v: number, mode: SliderChangeMode) => {
            // ★ 拖动中实时刷新本地数值显示；松手（End/Click）才提交父组件
            this.localVals[idx] = v;
            this.localLabels[idx] = this.fmtVal(idx, v);
            if (mode === SliderChangeMode.End || mode === SliderChangeMode.Click) {
              onChange(v);
            }
          })
        Text(maxLabel).fontSize(10).fontColor('#6E7681')
      }
      .width('100%')
    }
    .width('100%')
  }
}
