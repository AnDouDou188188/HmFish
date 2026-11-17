// ★ 状态 HUD：识别结果徽标/运动等级/识别中等状态展示
// entry/src/main/ets/view/StatusHudView.ets
@Component
export struct StatusHudView {
  @Prop cameraSources: string[] = ['USB', '后置', '前置']
  @Prop currentSourceIndex: number = 0
  @Prop yoloEnabled: boolean = false
  @Prop ocrEnabled: boolean = false
  @Prop ocrRunning: boolean = false
  @Prop ocrTotal: number = 0
  @Prop ocrHasText: number = 0
  @Prop motionEnabled: boolean = false
  @Prop depthEnabled: boolean = false
  @Prop cardAlpha: number = 0.82

  @Link threshold: number
  @Link message: string

  onSourceSelected?: (index: number) => void
  onYoloToggle?: () => void
  onOcrToggle?: () => void
  onCardAlphaChanged?: (alpha: number) => void

  private settings: RenderingContextSettings = new RenderingContextSettings(true)
  private ctx: CanvasRenderingContext2D = new CanvasRenderingContext2D(this.settings)

  // 以下常量与 Android 版一一对应
  private density: number = vp2px(1)
  private padH: number = 14 * this.density
  private padV: number = 10 * this.density
  private cardRadius: number = 8 * this.density
  private camBadgeH: number = 28 * this.density
  private chipH: number = 20 * this.density
  private alphaSliderBarW: number = 90 * this.density
  // ... 其余尺寸常量同理

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .onReady(() => {
        this.drawHud()
      })
      .onTouch((event: TouchEvent) => {
        // 触摸命中判断逻辑（与 Android onTouchEvent 一致）
        // 需要把坐标换算到 canvas 坐标系
      })
  }

  private drawHud() {
    const W = this.ctx.width
    const H = this.ctx.height
    if (W <= 0 || H <= 0) return

    // 1. 卡片背景（渐变 + 圆角）
    const gradient = this.ctx.createLinearGradient(0, 0, 0, 400 * this.density)
    gradient.addColorStop(0, this.hexWithAlpha(0x111519, this.cardAlpha))
    gradient.addColorStop(1, this.hexWithAlpha(0x0A0E14, Math.max(0.35, this.cardAlpha - 0.06)))
    this.ctx.fillStyle = gradient
    this.ctx.beginPath()
    this.ctx.roundRect(0, 0, W, H, this.cardRadius)
    this.ctx.fill()

    // 2. 边框
    this.ctx.strokeStyle = 'rgba(255,255,255,0.20)'
    this.ctx.lineWidth = 1 * this.density
    this.ctx.stroke()

    let currentTop = this.padV

    // 3. 摄像头胶囊行（居中）
    if (this.cameraSources.length > 0) {
      currentTop = this.drawCameraRow(currentTop, W)
      currentTop = this.drawStatusRow(currentTop, W)
    }

    // 4. YOLO 列表 / OCR 进度 / 运动 / 水深
    // ... 与 Android drawYoloList / drawOcrRow / drawMotion / drawDepth 逻辑一一对应
  }

  private drawCameraRow(startTop: number, W: number): number {
    const centerY = startTop + this.camBadgeH / 2
    // 计算总宽度 → 居中起点 → 逐个绘制胶囊
    // 选中：彩色实心 + 高光描边；未选中：灰色半透明 + 圆点
    // ...
    return startTop + this.camBadgeH + 6 * this.density
  }

  private drawStatusRow(startTop: number, W: number): number {
    // 左侧 YOLO/OCR chip，右侧透明度滑块
    // chip 绘制：roundRect + ✓/✗ 文字
    // 滑块绘制：轨道 + 渐变填充 + 三层圆 thumb
    // ...
    return startTop + 22 * this.density + 6 * this.density
  }

  private hexWithAlpha(rgb: number, alpha: number): string {
    const a = Math.round(Math.max(0, Math.min(1, alpha)) * 255)
    const r = (rgb >> 16) & 0xFF
    const g = (rgb >> 8) & 0xFF
    const b = rgb & 0xFF
    return `rgba(${r},${g},${b},${a / 255})`
  }
}