// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ CaptureMediaView（CaptureMediaView.ets）—— 快门按钮（Canvas 自绘）           ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  底部面板中央的快门按钮，原生相机风格，用 Canvas 手绘而非组合容器：           ║
// ║   待机 = 白色外环 + 白色实心内圆（拍照快门）；录像中 = 白环 + 红色圆角        ║
// ║   方块（停止键）；按下内圆/方块 0.9 缩放回弹。录像计时由 RecTimerPopup        ║
// ║   单独展示，此处保持干净                                                     ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  currentMode/isRecording 由 BottomPanel 从 Index 透传（@Prop + @Watch        ║
// ║  触发重绘）；组件自身不响应点击——点击事件由外层 BottomPanel 的 Stack          ║
// ║  接管（调 onToggleRecord），本组件只负责"画得像"                             ║
// ║                                                                              ║
// ║ 【关键状态】                                                                 ║
// ║  innerScale：按压缩放系数（1.0 常态 / 0.9 按下），驱动 draw() 缩放；          ║
// ║  16ms 定时器仅在按压回弹动画期间触发重绘，平时空转开销极小                    ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 原生相机风格快门按钮（重设计）：
//   待机：白色外环 + 白色实心内圆（原生拍照快门）
//   录像中：白色外环 + 红色圆角方块（原生停止键）
//   按下：内圆/方块 0.9 缩放回弹；计时由 RecTimerPopup 展示，此处保持干净
@Component
export struct CaptureMediaView {
  // ★ 外部状态（@Watch 变化即重绘）：当前拍摄模式/是否录像中/录像秒数（仅透传）
  @Prop @Watch('onModeChange') currentMode: string;
  @Prop @Watch('onStateChange') isRecording: boolean;
  @Prop recordSec: number = -1;

  // ★ 按压缩放系数：1.0 常态 / 0.9 按下（animateTo 过渡），draw() 据此缩放内圆/方块
  @State innerScale: number = 1.0;

  // ★ Canvas 上下文与就绪标志：onReady 前宽高为 0，画了也白画，故先判 ctxReady
  private timer: number = -1;
  private canvasCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(new RenderingContextSettings(true));
  private ctxReady: boolean = false;

  aboutToAppear() {
    // 仅服务按压回弹动画期间的重绘（按下 250ms 内高频，平时空转开销极小）
    this.timer = setInterval(() => {
      if (this.innerScale !== 1.0) this.draw();
    }, 16);
  }

  // ★ 生命周期清理：销毁时必须清掉重绘定时器，防止组件销毁后定时器空转泄漏
  aboutToDisappear() {
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
  }

  // ★ @Watch 回调：模式/录像状态一变立即重绘外观（白圆 ↔ 红方块切换）
  private onModeChange() { this.draw(); }
  private onStateChange() { this.draw(); }

  // ★ 全量重绘：清屏 → 白色外环 →（录像中 ? 红色圆角停止方块 : 白色拍照内圆）
  private draw() {
    if (!this.ctxReady) return;
    const ctx = this.canvasCtx;
    const w = ctx.width, h = ctx.height;
    if (w <= 0 || h <= 0) return;
    const cx = w / 2, cy = h / 2, r = Math.min(w, h) / 2;
    ctx.clearRect(0, 0, w, h);

    // 外环：原生相机快门白环（始终白色，简洁无动画）
    ctx.strokeStyle = 'rgba(255,255,255,0.95)';
    ctx.lineWidth = r * 0.085;
    ctx.beginPath(); ctx.arc(cx, cy, r * 0.955, 0, Math.PI * 2); ctx.stroke();

    if (this.isRecording) {
      // 录像中：红色圆角方块（原生停止键），随按压缩放
      const s = r * 0.46 * this.innerScale;
      const rad = s * 0.32;
      ctx.fillStyle = '#E53935';
      ctx.beginPath();
      ctx.moveTo(cx - s + rad, cy - s);
      ctx.arcTo(cx + s, cy - s, cx + s, cy + s, rad);
      ctx.arcTo(cx + s, cy + s, cx - s, cy + s, rad);
      ctx.arcTo(cx - s, cy + s, cx - s, cy - s, rad);
      ctx.arcTo(cx - s, cy - s, cx + s, cy - s, rad);
      ctx.closePath();
      ctx.fill();
    } else {
      // 待机：白色实心内圆（原生拍照快门），随按压缩放
      ctx.fillStyle = '#FAFAFA';
      ctx.beginPath(); ctx.arc(cx, cy, r * 0.60 * this.innerScale, 0, Math.PI * 2); ctx.fill();
    }
  }

  build() {
    Stack() {
      // ── 画布：尺寸由外部容器（BottomPanel 的 SHUTTER_SIZE）撑满，onReady 首绘 ──
      Canvas(this.canvasCtx)
        .width('100%').height('100%')
        .onReady(() => { this.ctxReady = true; this.draw(); })
    }
    // ★ 尺寸由外部容器（BottomPanel 的 SHUTTER_SIZE）决定，这里 100% 撑满
    .width('100%').height('100%')
    .onTouch((event) => {
      if (event.type === TouchType.Down) {
        animateTo({ duration: 100 }, () => { this.innerScale = 0.9; });
      } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
        animateTo({ duration: 150 }, () => { this.innerScale = 1.0; });
      }
    })
  }
}
