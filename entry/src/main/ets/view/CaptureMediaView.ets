// ★ 原生相机风格快门按钮（重设计）：
//   待机：白色外环 + 白色实心内圆（原生拍照快门）
//   录像中：白色外环 + 红色圆角方块（原生停止键）
//   按下：内圆/方块 0.9 缩放回弹；计时由 RecTimerPopup 展示，此处保持干净
@Component
export struct CaptureMediaView {
  @Prop @Watch('onModeChange') currentMode: string;
  @Prop @Watch('onStateChange') isRecording: boolean;
  @Prop recordSec: number = -1;

  @State innerScale: number = 1.0;

  private timer: number = -1;
  private canvasCtx: CanvasRenderingContext2D = new CanvasRenderingContext2D(new RenderingContextSettings(true));
  private ctxReady: boolean = false;

  aboutToAppear() {
    // 仅服务按压回弹动画期间的重绘（按下 250ms 内高频，平时空转开销极小）
    this.timer = setInterval(() => {
      if (this.innerScale !== 1.0) this.draw();
    }, 16);
  }

  aboutToDisappear() {
    if (this.timer !== -1) { clearInterval(this.timer); this.timer = -1; }
  }

  private onModeChange() { this.draw(); }
  private onStateChange() { this.draw(); }

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
