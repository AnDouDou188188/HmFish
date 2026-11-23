// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ PowerSheet（PowerSheet.ets）—— 能耗设置半模态弹窗                             ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  点顶部"能耗"徽标弹出。平衡流畅度与耗电：                                     ║
// ║   1. 四档模式：省电/标准/性能（POWER_PRESETS 预设）+ 自定义（恢复上次微调）    ║
// ║   2. 三档参数对照表：直观展示各档默认参数差异（各档参数不同）                  ║
// ║   3. 六项节奏滑条微调：预览帧间隔 20~100ms / 识别间隔·有目标 100~1000ms /     ║
// ║      识别间隔·空闲 300~2000ms / 文字识别间隔 1000~10000ms step500 /           ║
// ║      水深采样间隔 100~1000ms / 推理线程数 2~8；所有更改立即保存立即生效        ║
// ║      （native 原子参数热更新，不重建管线）                                    ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  @Prop @Watch syncLocal：Index 选预设档改参数时同步本地缓冲，滑条跟着跳；      ║
// ║  六滑条共用 localVals/localLabels 数组缓冲，松手才提交；                      ║
// ║  ★ @Builder 按值传参不随状态刷新——数值/文案必须在体内直读 this.localXxx      ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 能耗模式弹窗：三档预设 + 渲染/识别/传感节奏滑条微调
import { POWER_PRESETS } from '../common/PowerPresets';

/**
 * 能耗设置弹窗：四档选择（省电/标准/性能/自定义）+ 六项节奏滑条微调
 * 样式照抄 MotionSheet（运动设置）：底部弹窗 + 预设行 + 滑条 + 完成按钮
 * 所有更改立即保存并立即生效（native 原子参数热更新）
 *
 * ★ 关键：@Builder 按值传参不会随状态变化刷新 UI——
 *   滑条数值/文案必须在 @Builder 体内直接读 this.localVals/localLabels（状态直读才响应刷新）
 */
// ★ PowerSheet：能耗设置弹窗主体（四档选择 + 参数对照 + 六滑条微调）
@Component
export struct PowerSheet {
  // ★ @Watch：父组件改参数（选预设档）时同步本地缓冲，弹窗内滑条跟着跳到预设值
  @Prop @Watch('syncLocal') powerMode: number = 1;        // 0省电/1标准/2性能/3自定义
  @Prop @Watch('syncLocal') powerRenderMs: number = 40;
  @Prop @Watch('syncLocal') powerYoloMs: number = 300;
  @Prop @Watch('syncLocal') powerYoloIdleMs: number = 800;
  @Prop @Watch('syncLocal') powerSensorMs: number = 250;
  @Prop @Watch('syncLocal') powerOcrMs: number = 1500;
  @Prop @Watch('syncLocal') powerThreads: number = 8;

  // ── 交互回调：关闭/选预设档/自定义档/六滑条提交（松手才触发，立即生效） ──
  onClose?: () => void;
  onPresetSelect?: (mode: number) => void;
  onCustomSelect?: () => void;
  onRenderMsChange?: (v: number) => void;
  onYoloMsChange?: (v: number) => void;
  onYoloIdleMsChange?: (v: number) => void;
  onSensorMsChange?: (v: number) => void;
  onOcrMsChange?: (v: number) => void;
  onThreadsChange?: (v: number) => void;

  @State appear: boolean = false;
  // ★ 六滑条本地缓冲（与滑条同单位）：拖动期间只更新本地值，松手才提交父组件
  @State localVals: number[] = [40, 300, 800, 1500, 250, 8];
  // ★ 数值文案（@Builder 体内直读才刷新，不能按值传入）
  @State localLabels: string[] = ['40ms', '300ms', '800ms', '4.0s', '250ms', '8'];

  aboutToAppear(): void {
    this.syncLocal();
    setTimeout(() => { this.appear = true; }, 30);
  }

  // ★ 本地缓冲与 @Prop 对齐（初始化 + 父组件改参数时同步）
  private syncLocal(): void {
    this.localVals = [this.powerRenderMs, this.powerYoloMs, this.powerYoloIdleMs,
      this.powerOcrMs, this.powerSensorMs, this.powerThreads];
    this.localLabels = [this.fmtVal(0, this.localVals[0]), this.fmtVal(1, this.localVals[1]),
      this.fmtVal(2, this.localVals[2]), this.fmtVal(3, this.localVals[3]),
      this.fmtVal(4, this.localVals[4]), this.fmtVal(5, this.localVals[5])];
  }

  // ★ 各滑条数值文案（3=文字间隔转秒、5=线程无单位，其余 ms）
  private fmtVal(idx: number, v: number): string {
    if (idx === 3) return `${(v / 1000).toFixed(1)}s`;
    if (idx === 5) return `${v}`;
    return `${v}ms`;
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
          Text('能耗设置').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
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

        // ── 滚动配置区：四档 pill + 三档参数对照 + 六节奏滑条 ──
        Scroll() {
          Column() {
            // ───── 四档选择：省电/标准/性能 + 自定义 ─────
            Row({ space: 8 }) {
              this.modePill('省电', 0, '#34C759', 'rgba(52,199,89,0.22)')
              this.modePill('标准', 1, '#0A84FF', 'rgba(10,132,255,0.22)')
              this.modePill('性能', 2, '#FF9500', 'rgba(255,149,0,0.22)')
              this.modePill('自定义', 3, '#AF52DE', 'rgba(175,82,222,0.22)')
            }
            .width('100%')
            .margin({ top: 14 })

            if (this.powerMode === 3) {
              Text('自定义档：恢复上次手动微调的参数；拖动滑条会更新自定义档参数')
                .fontSize(10).fontColor('#AF52DE').width('100%').margin({ top: 6 })
            }

            // ★ 三档参数对照：直观展示各档默认参数（三档各不相同），滑条两端为可调范围
            Column({ space: 0 }) {
              Text('三档参数对照')
                .fontSize(11).fontColor('#8E97A3')
                .width('100%').margin({ bottom: 6 })
              ForEach(POWER_PRESETS, (p: number[], mode: number) => {
                Row({ space: 6 }) {
                  Text(['省电', '标准', '性能'][mode])
                    .fontSize(11)
                    .fontColor(this.powerMode === mode ? this.presetColor(mode) : '#8E97A3')
                    .fontWeight(FontWeight.Bold)
                    .width(30)
                  Text(`帧间隔${p[0]}ms · 识别${p[1]}/${p[2]}ms · 文字${(p[3] / 1000).toFixed(0)}s · 水深${p[4]}ms · ${p[5]}线程`)
                    .fontSize(10)
                    .fontColor(this.powerMode === mode ? '#EDEFF2' : '#6E7681')
                    .layoutWeight(1)
                }
                .width('100%')
                .padding({ top: 3, bottom: 3 })
              }, (p: number[], mode: number) => `${mode}_${this.powerMode}`)
            }
            .width('100%')
            .padding({ left: 12, right: 12, top: 10, bottom: 10 })
            .margin({ top: 12 })
            .borderRadius(12)
            .backgroundColor('rgba(255,255,255,0.04)')
            .border({ width: 1, color: 'rgba(255,255,255,0.06)' })

            // ───── 节奏参数滑条 ─────（idx 对应 localVals 下标，松手提交）
            this.sliderRow(0, '◐', '预览帧间隔', 20, 100, 10,
              (v: number): void => { this.onRenderMsChange?.(v); })
            this.sliderRow(1, '◎', '识别间隔 · 有目标', 100, 1000, 50,
              (v: number): void => { this.onYoloMsChange?.(v); })
            this.sliderRow(2, '○', '识别间隔 · 空闲', 300, 2000, 100,
              (v: number): void => { this.onYoloIdleMsChange?.(v); })
            this.sliderRow(3, '⌗', '文字识别间隔', 1000, 10000, 500,
              (v: number): void => { this.onOcrMsChange?.(v); })
            this.sliderRow(4, '≈', '水深采样间隔', 100, 1000, 50,
              (v: number): void => { this.onSensorMsChange?.(v); })
            this.sliderRow(5, '☰', '推理线程数', 2, 8, 2,
              (v: number): void => { this.onThreadsChange?.(v); })

            Text('间隔越小越流畅也越耗电；省电档适合长时间挂机，性能档适合快速追踪目标')
              .fontSize(10).fontColor('#8E97A3')
              .width('100%').margin({ top: 10 })
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

  // ★ 档位配色（省电绿/标准蓝/性能橙/自定义紫）：pill 与对照表共用
  private presetColor(mode: number): string {
    if (mode === 0) return '#34C759';
    if (mode === 1) return '#0A84FF';
    if (mode === 2) return '#FF9500';
    return '#AF52DE';
  }

  // ★ 模式 pill：选中用各档专属配色高亮，点击选档（自定义档走独立回调恢复上次微调）
  @Builder
  modePill(text: string, mode: number, activeColor: string, activeBg: string) {
    Text(text)
      .fontSize(13)
      .fontColor(this.powerMode === mode ? activeColor : '#8E97A3')
      .fontWeight(FontWeight.Bold)
      .padding({ left: 12, right: 12, top: 7, bottom: 7 })
      .borderRadius(14)
      .backgroundColor(this.powerMode === mode ? activeBg : 'rgba(255,255,255,0.06)')
      .border({ width: 1, color: this.powerMode === mode ? activeColor : 'rgba(255,255,255,0.15)' })
      .layoutWeight(1)
      .textAlign(TextAlign.Center)
      .onClick(() => {
        // ★ 自定义档走独立回调（恢复上次手动微调的参数）
        if (mode === 3) {
          this.onCustomSelect?.();
        } else {
          this.onPresetSelect?.(mode);
        }
      })
  }

  // ★ 滑条行：数值/文案在体内直读 this.localVals/localLabels（状态直读才随拖动刷新）
  @Builder
  sliderRow(idx: number, icon: string, title: string,
    min: number, max: number, step: number,
    onChange: (v: number) => void) {
    Column() {
      Row({ space: 6 }) {
        Text(icon).fontSize(13).fontColor('#00E5FF')
        Text(title).fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
        Blank()
        Text(this.localLabels[idx])
          .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
      }
      .width('100%')
      .margin({ top: 16, bottom: 8 })

      Row({ space: 10 }) {
        Text(`${min}`).fontSize(10).fontColor('#6E7681')
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
        Text(`${max}`).fontSize(10).fontColor('#6E7681')
      }
      .width('100%')
    }
    .width('100%')
  }
}
