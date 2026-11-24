// ╔══════════════════════════════════════════════════════════════════╗
// ║ RestoreProgressEffect —— AI 修复进度特效画布（覆盖整张照片区域）  ║
// ╠══════════════════════════════════════════════════════════════════╣
// ║ 【职责】AI 高清彩色修复进行中，在照片预览上叠加进度特效（四选一，  ║
// ║   每张照片由父组件随机指定一种，effectType 变化自动重播种粒子）：  ║
// ║   0 星光点缀：星星数量随进度增多 + 独立闪烁 + 周期性流星划过      ║
// ║   1 水波涟漪：随机落点涟漪扩散（落点微光 + 主环 + 回声环），      ║
// ║               中心一个半径=进度的进度大环                        ║
// ║   2 流光溢彩：多团彩色光雾（青/品红/琥珀）漂移呼吸，lighter 叠加  ║
// ║   3 光之扫描：垂直光带前沿位置 = 进度（白芯 + 辉光 + 尾迹渐变），  ║
// ║               前沿持续迸出向后飘散的火花粒子                     ║
// ║                                                                    ║
// ║ 【实现】Canvas 50ms 定时自绘（20fps，粒子级动画足够流畅）；进度由  ║
// ║   父组件 @Prop 注入（0..1，模拟曲线平滑递增）。组件卸载时清定时器。║
// ║   粒子全部按比例坐标播种，适配任意画布尺寸。                      ║
// ╚══════════════════════════════════════════════════════════════════╝

// 星星粒子（星光点缀）：比例坐标 + 闪烁参数 + 色调
interface EffectStar {
  x: number;        // 横向位置比例 0..1
  y: number;        // 纵向位置比例 0..1
  size: number;     // 基础半径（vp）
  phase: number;    // 闪烁初相（弧度）
  speed: number;    // 闪烁速率（每帧弧度增量）
  tint: number;     // 0 白 / 1 冰蓝 / 2 暖金
}

// 涟漪粒子（水波涟漪）：像素坐标 + 半径生长参数
interface EffectRipple {
  x: number;        // 圆心横坐标（px）
  y: number;        // 圆心纵坐标（px）
  r: number;        // 当前半径（px）
  maxR: number;     // 消散半径（px）
  speed: number;    // 半径增速（px/帧）
}

// 光雾团（流光溢彩）：比例锚点 + 漂移相位 + 色调
interface EffectMist {
  bx: number;       // 锚点横向比例 0..1
  by: number;       // 锚点纵向比例 0..1
  r: number;        // 半径比例（相对画布短边）
  tint: number;     // 0 青 / 1 品红 / 2 琥珀
  dx: number;       // 横向漂移相位
  dy: number;       // 纵向漂移相位
  phase: number;    // 呼吸/明暗初相
}

// 火花粒子（光之扫描）：从光带前沿向后飘散的亮点
interface EffectSpark {
  y: number;        // 纵向位置比例 0..1
  speed: number;    // 消耗速率（寿命帧/帧）
  life: number;     // 剩余寿命（帧）
  size: number;     // 半径（vp）
}

@Component
export struct RestoreProgressEffect {
  // 特效类型（0..3，父组件每张照片随机指定；变化时自动重播种）
  @Prop effectType: number = 0;
  // 总进度 0..1（父组件模拟曲线注入，驱动星星数量/涟漪速度/扫描位置）
  @Prop progress: number = 0;

  private ctx: CanvasRenderingContext2D =
    new CanvasRenderingContext2D(new RenderingContextSettings(true));
  private tickId: number = -1;
  private frame: number = 0;          // 帧计数（闪烁/漂移的时间轴）
  private seededType: number = -1;    // 已播种的特效类型（检测变化重播种）
  private stars: EffectStar[] = [];
  private ripples: EffectRipple[] = [];
  private lastRippleFrame: number = 0;
  private mists: EffectMist[] = [];
  private sparks: EffectSpark[] = [];

  aboutToDisappear(): void {
    this.stopTick();
  }

  private stopTick(): void {
    if (this.tickId >= 0) {
      clearInterval(this.tickId);
      this.tickId = -1;
    }
  }

  // ★ 播种：按特效类型生成粒子集（照片切换换特效时自动调用）
  private reseed(): void {
    this.stars = [];
    this.ripples = [];
    this.mists = [];
    this.sparks = [];
    this.lastRippleFrame = 0;
    if (this.effectType === 0) {
      // 星光：预播种 110 颗（实际显示数量由 progress 决定）
      for (let i = 0; i < 110; i++) {
        const roll = Math.random();
        const star: EffectStar = {
          x: Math.random(),
          y: Math.random(),
          size: 0.8 + Math.random() * 2.2,
          phase: Math.random() * Math.PI * 2,
          speed: 0.04 + Math.random() * 0.10,
          tint: roll < 0.68 ? 0 : (roll < 0.88 ? 1 : 2)
        };
        this.stars.push(star);
      }
    } else if (this.effectType === 2) {
      // 光雾：6 团三种色调
      for (let i = 0; i < 6; i++) {
        const mist: EffectMist = {
          bx: 0.15 + Math.random() * 0.7,
          by: 0.15 + Math.random() * 0.7,
          r: 0.18 + Math.random() * 0.2,
          tint: i % 3,
          dx: Math.random() * Math.PI * 2,
          dy: Math.random() * Math.PI * 2,
          phase: Math.random() * Math.PI * 2
        };
        this.mists.push(mist);
      }
    } else if (this.effectType === 3) {
      // 扫描火花：常驻 12 颗循环重生
      for (let i = 0; i < 12; i++) {
        const spark: EffectSpark = {
          y: Math.random(),
          speed: 0.4 + Math.random() * 1.4,
          life: 10 + Math.random() * 22,
          size: 0.6 + Math.random() * 1.6
        };
        this.sparks.push(spark);
      }
    }
    // 类型 1（涟漪）粒子按帧动态生成，无需预播种
    this.seededType = this.effectType;
  }

  build() {
    Canvas(this.ctx)
      .width('100%')
      .height('100%')
      .onReady((): void => {
        // 画布就绪后启动自绘循环（20fps；卸载时 aboutToDisappear 清理）
        this.stopTick();
        this.tickId = setInterval((): void => { this.drawFrame(); }, 50);
      })
  }

  private drawFrame(): void {
    const w = this.ctx.width;
    const h = this.ctx.height;
    if (w <= 0 || h <= 0) {
      return;
    }
    if (this.seededType !== this.effectType) {
      this.reseed();
    }
    this.frame++;
    this.ctx.clearRect(0, 0, w, h);
    if (this.effectType === 0) {
      this.drawStars(w, h);
    } else if (this.effectType === 1) {
      this.drawRipples(w, h);
    } else if (this.effectType === 2) {
      this.drawMist(w, h);
    } else {
      this.drawSweep(w, h);
    }
  }

  // 星色：白 / 冰蓝 / 暖金三种色调
  private starColor(tint: number, alpha: number): string {
    if (tint === 1) {
      return `rgba(140,225,255,${alpha.toFixed(3)})`;
    }
    if (tint === 2) {
      return `rgba(255,215,150,${alpha.toFixed(3)})`;
    }
    return `rgba(255,255,255,${alpha.toFixed(3)})`;
  }

  // ★ 星光点缀：深蓝夜幕薄纱上，星星数量随进度增多，各自闪烁，
  //   每 ~4.5s 一颗流星沿对角线划过
  private drawStars(w: number, h: number): void {
    this.ctx.fillStyle = 'rgba(6,12,30,0.42)';
    this.ctx.fillRect(0, 0, w, h);
    // 显示数量 = 6 颗底星 + 进度 × 剩余星（进度涨星星密）
    const count = Math.max(6,
      Math.min(this.stars.length, Math.floor(6 + this.progress * (this.stars.length - 6))));
    for (let i = 0; i < count; i++) {
      const s = this.stars[i];
      const tw = 0.30 + 0.70 * (0.5 + 0.5 * Math.sin(this.frame * s.speed + s.phase));
      const x = s.x * w;
      const y = s.y * h;
      const r = s.size * (0.6 + 0.4 * tw);
      // 光晕（径向渐变）
      const glow = this.ctx.createRadialGradient(x, y, 0, x, y, r * 4);
      glow.addColorStop(0, this.starColor(s.tint, 0.55 * tw));
      glow.addColorStop(1, this.starColor(s.tint, 0));
      this.ctx.fillStyle = glow;
      this.ctx.beginPath();
      this.ctx.arc(x, y, r * 4, 0, Math.PI * 2);
      this.ctx.fill();
      // 四芒（十字细线）
      this.ctx.strokeStyle = this.starColor(s.tint, 0.85 * tw);
      this.ctx.lineWidth = 0.8;
      this.ctx.beginPath();
      this.ctx.moveTo(x - r * 2.6, y);
      this.ctx.lineTo(x + r * 2.6, y);
      this.ctx.moveTo(x, y - r * 2.6);
      this.ctx.lineTo(x, y + r * 2.6);
      this.ctx.stroke();
      // 星核
      this.ctx.fillStyle = this.starColor(s.tint, Math.min(1, 0.4 + tw * 0.8));
      this.ctx.beginPath();
      this.ctx.arc(x, y, r * 0.7, 0, Math.PI * 2);
      this.ctx.fill();
    }
    // 流星：26 帧划过一轮（90 帧周期 ≈ 4.5s），带渐隐拖尾
    const cometPhase = this.frame % 90;
    if (cometPhase < 26) {
      const p = cometPhase / 26;
      const cx = w * (0.15 + 0.85 * p);
      const cy = h * (0.06 + 0.5 * p);
      const trail = this.ctx.createLinearGradient(cx - 70, cy - 38, cx, cy);
      trail.addColorStop(0, 'rgba(160,230,255,0)');
      trail.addColorStop(1, 'rgba(200,245,255,0.85)');
      this.ctx.strokeStyle = trail;
      this.ctx.lineWidth = 1.4;
      this.ctx.beginPath();
      this.ctx.moveTo(cx - 70, cy - 38);
      this.ctx.lineTo(cx, cy);
      this.ctx.stroke();
    }
  }

  // ★ 水波涟漪：随机落点扩散环（主环 + 回声环 + 落点微光），
  //   扩散速度随进度加快；中心一个半径 = 进度 × 半屏的进度大环
  private drawRipples(w: number, h: number): void {
    this.ctx.fillStyle = 'rgba(3,18,32,0.40)';
    this.ctx.fillRect(0, 0, w, h);
    const minSide = Math.min(w, h);
    // 动态生成涟漪：约 0.7s 一个，同时最多 9 个
    if (this.frame - this.lastRippleFrame > 14 && this.ripples.length < 9) {
      this.lastRippleFrame = this.frame;
      const ripple: EffectRipple = {
        x: 60 + Math.random() * (w - 120),
        y: 60 + Math.random() * (h - 120),
        r: 4,
        maxR: minSide * (0.22 + Math.random() * 0.22),
        speed: 1.8 + Math.random() * 1.6
      };
      this.ripples.push(ripple);
    }
    for (let i = this.ripples.length - 1; i >= 0; i--) {
      const rp = this.ripples[i];
      rp.r += rp.speed * (0.6 + this.progress);
      if (rp.r >= rp.maxR) {
        this.ripples.splice(i, 1);
        continue;
      }
      const fade = 1 - rp.r / rp.maxR;
      // 主环
      this.ctx.strokeStyle = `rgba(130,235,255,${(0.75 * fade).toFixed(3)})`;
      this.ctx.lineWidth = 1.6 + fade * 1.2;
      this.ctx.beginPath();
      this.ctx.arc(rp.x, rp.y, rp.r, 0, Math.PI * 2);
      this.ctx.stroke();
      // 回声环（半径 62%，更淡更细）
      this.ctx.strokeStyle = `rgba(130,235,255,${(0.3 * fade).toFixed(3)})`;
      this.ctx.lineWidth = 1;
      this.ctx.beginPath();
      this.ctx.arc(rp.x, rp.y, rp.r * 0.62, 0, Math.PI * 2);
      this.ctx.stroke();
      // 落点微光（刚生成时）
      if (rp.r < 26) {
        const dot = this.ctx.createRadialGradient(rp.x, rp.y, 0, rp.x, rp.y, 20);
        dot.addColorStop(0, 'rgba(190,245,255,0.8)');
        dot.addColorStop(1, 'rgba(190,245,255,0)');
        this.ctx.fillStyle = dot;
        this.ctx.beginPath();
        this.ctx.arc(rp.x, rp.y, 20, 0, Math.PI * 2);
        this.ctx.fill();
      }
    }
    // 中心进度大环：半径 = 进度 × 0.55 × 短边，直观表达整体进度
    const bigR = this.progress * minSide * 0.55;
    if (bigR > 2) {
      this.ctx.strokeStyle = 'rgba(0,229,255,0.30)';
      this.ctx.lineWidth = 2;
      this.ctx.beginPath();
      this.ctx.arc(w / 2, h / 2, bigR, 0, Math.PI * 2);
      this.ctx.stroke();
    }
  }

  // 光雾色：青 / 品红 / 琥珀
  private mistColor(tint: number): string {
    if (tint === 1) {
      return 'rgba(255,120,220,';
    }
    if (tint === 2) {
      return 'rgba(255,190,90,';
    }
    return 'rgba(0,229,255,';
  }

  // ★ 流光溢彩：多团彩色光雾按正弦轨迹漂移、呼吸缩放，lighter 混合叠加，
  //   亮度随进度微增；顶部一条进度光带从左向右推进
  private drawMist(w: number, h: number): void {
    this.ctx.fillStyle = 'rgba(10,6,26,0.45)';
    this.ctx.fillRect(0, 0, w, h);
    const minSide = Math.min(w, h);
    this.ctx.globalCompositeOperation = 'lighter';
    for (const m of this.mists) {
      const t = this.frame;
      const x = (m.bx + 0.10 * Math.sin(t * 0.012 + m.dx)) * w;
      const y = (m.by + 0.10 * Math.cos(t * 0.010 + m.dy)) * h;
      const r = m.r * minSide * (1 + 0.10 * Math.sin(t * 0.02 + m.phase));
      const alpha = 0.20 + 0.06 * Math.sin(t * 0.03 + m.phase) + this.progress * 0.10;
      const grad = this.ctx.createRadialGradient(x, y, 0, x, y, r);
      grad.addColorStop(0, `${this.mistColor(m.tint)}${Math.max(0.05, alpha).toFixed(3)})`);
      grad.addColorStop(0.7, `${this.mistColor(m.tint)}${Math.max(0.02, alpha * 0.45).toFixed(3)})`);
      grad.addColorStop(1, `${this.mistColor(m.tint)}0)`);
      this.ctx.fillStyle = grad;
      this.ctx.beginPath();
      this.ctx.arc(x, y, r, 0, Math.PI * 2);
      this.ctx.fill();
    }
    this.ctx.globalCompositeOperation = 'source-over';
    // 顶部进度光带
    this.ctx.fillStyle = 'rgba(255,255,255,0.10)';
    this.ctx.fillRect(0, 0, w * this.progress, 2);
  }

  // ★ 光之扫描：垂直光带前沿 = 进度位置（尾迹渐变 + 白芯辉光），
  //   前沿持续迸出向后飘散、渐隐的火花粒子
  private drawSweep(w: number, h: number): void {
    this.ctx.fillStyle = 'rgba(5,10,24,0.42)';
    this.ctx.fillRect(0, 0, w, h);
    const edge = Math.max(2, this.progress * w);
    // 尾迹：已扫过区域微亮渐变（营造"完成感"）
    const tailStart = Math.max(0, edge - w * 0.45);
    const tail = this.ctx.createLinearGradient(tailStart, 0, edge, 0);
    tail.addColorStop(0, 'rgba(0,229,255,0)');
    tail.addColorStop(1, 'rgba(0,229,255,0.16)');
    this.ctx.fillStyle = tail;
    this.ctx.fillRect(tailStart, 0, edge - tailStart, h);
    // 光带主体：后缘辉光 → 白芯 → 前缘淡出
    const band = this.ctx.createLinearGradient(edge - 7, 0, edge + 3, 0);
    band.addColorStop(0, 'rgba(120,235,255,0)');
    band.addColorStop(0.75, 'rgba(160,240,255,0.55)');
    band.addColorStop(0.92, 'rgba(255,255,255,0.95)');
    band.addColorStop(1, 'rgba(255,255,255,0)');
    this.ctx.fillStyle = band;
    this.ctx.fillRect(edge - 7, 0, 10, h);
    // 前沿火花：向后飘散并渐隐，寿命尽后在随机高度重生
    for (let i = 0; i < this.sparks.length; i++) {
      const sp = this.sparks[i];
      sp.life -= sp.speed;
      if (sp.life <= 0) {
        const fresh: EffectSpark = {
          y: Math.random(),
          speed: 0.4 + Math.random() * 1.4,
          life: 10 + Math.random() * 22,
          size: 0.6 + Math.random() * 1.6
        };
        this.sparks[i] = fresh;
        continue;
      }
      const drift = (1 - sp.life / 32) * 18;
      const a = Math.min(1, sp.life / 20) * 0.9;
      this.ctx.fillStyle = `rgba(210,245,255,${a.toFixed(3)})`;
      this.ctx.beginPath();
      this.ctx.arc(edge - drift, sp.y * h, sp.size, 0, Math.PI * 2);
      this.ctx.fill();
    }
  }
}
