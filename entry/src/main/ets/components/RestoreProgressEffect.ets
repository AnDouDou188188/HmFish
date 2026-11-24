// ╔══════════════════════════════════════════════════════════════════╗
// ║ RestoreProgressEffect —— AI 修复进度特效画布（覆盖整张照片区域）  ║
// ╠══════════════════════════════════════════════════════════════════╣
// ║ 【核心概念】进度面纱：一层半透明特效"液体"自照片底部随进度上升，  ║
// ║   覆盖高度 = progress × 画布高——0% 只剩底部一线微光，50% 淹没半张  ║
// ║   照片，100% 半透明铺满全图（照片始终隐约透出）；面纱上沿是动态    ║
// ║   发光边线，面纱内部承载各特效粒子。四种风格（每张照片随机一种，  ║
// ║   effectType 变化自动重播种）：                                    ║
// ║   0 星光点缀：夜幕薄纱 + 面纱内星星闪烁（随上升越露越多）+ 流星，  ║
// ║               上沿为星尘微光带                                    ║
// ║   1 水波涟漪：青蓝水体自底上涨，上沿双正弦波动浪 + 浪下亮带，      ║
// ║               水内随机落点涟漪扩散                                ║
// ║   2 流光溢彩：彩色光雾（青/品红/琥珀，lighter 混合）被困在面纱内   ║
// ║               漂移呼吸，上沿为多层辉光光线                        ║
// ║   3 光之扫描：已扫过区域青色尾迹渐变，上沿为横向扫描亮线（白芯 +  ║
// ║               辉光），火花自亮线坠入已扫区渐隐                    ║
// ║                                                                    ║
// ║ 【百分比】画布中央大字显示当前完成百分比（白色粗体 + 青色辉光 +    ║
// ║   深色描边保证任意照片上可读），下方小字"修复进度"。               ║
// ║                                                                    ║
// ║ 【实现】Canvas 50ms 定时自绘（20fps）；进度由父组件 @Prop 注入     ║
// ║   （0..1，整体单调递增——面纱跨照片连续上涨不回退）。粒子按比例    ║
// ║   坐标播种适配任意画布尺寸；组件卸载清定时器。                     ║
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
  by: number;       // 锚点纵向比例 0..1（偏向下半部——面纱上涨即早可见）
  r: number;        // 半径比例（相对画布短边）
  tint: number;     // 0 青 / 1 品红 / 2 琥珀
  dx: number;       // 横向漂移相位
  dy: number;       // 纵向漂移相位
  phase: number;    // 呼吸/明暗初相
}

// 火花粒子（光之扫描）：从扫描亮线坠入已扫区
interface EffectSpark {
  x: number;        // 横向位置比例 0..1
  y: number;        // 距扫描线的纵向偏移（px，向下增长）
  speed: number;    // 下坠/消耗速率
  life: number;     // 剩余寿命（帧）
  size: number;     // 半径（vp）
}

@Component
export struct RestoreProgressEffect {
  // 特效类型（0..3，父组件每张照片随机指定；变化时自动重播种）
  @Prop effectType: number = 0;
  // 总进度 0..1（父组件模拟曲线注入，驱动面纱上升高度与百分比数字）
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
      // 星光：预播种 110 颗，面纱上涨到哪里星星露到哪里
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
      // 光雾：6 团三种色调，纵向锚点偏下（0.30~0.92）——面纱半程即全部可见
      for (let i = 0; i < 6; i++) {
        const mist: EffectMist = {
          bx: 0.15 + Math.random() * 0.7,
          by: 0.30 + Math.random() * 0.62,
          r: 0.18 + Math.random() * 0.2,
          tint: i % 3,
          dx: Math.random() * Math.PI * 2,
          dy: Math.random() * Math.PI * 2,
          phase: Math.random() * Math.PI * 2
        };
        this.mists.push(mist);
      }
    } else if (this.effectType === 3) {
      // 扫描火花：常驻 14 颗，自扫描线坠入已扫区后重生
      for (let i = 0; i < 14; i++) {
        const spark: EffectSpark = {
          x: Math.random(),
          y: Math.random() * 24,
          speed: 0.5 + Math.random() * 1.5,
          life: 10 + Math.random() * 24,
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
    // ★ 进度面纱几何：覆盖高度 = 进度 × 画布高，自底部上涨
    const prog = Math.max(0, Math.min(1, this.progress));
    const coverH = prog * h;
    const coverTop = h - coverH;
    if (coverH >= 2) {
      if (this.effectType === 0) {
        this.drawStarsVeil(w, h, coverTop, coverH);
      } else if (this.effectType === 1) {
        this.drawWaterVeil(w, h, coverTop, coverH);
      } else if (this.effectType === 2) {
        this.drawMistVeil(w, h, coverTop, coverH);
      } else {
        this.drawScanVeil(w, h, coverTop, coverH);
      }
    } else {
      // 进度≈0：底部一线微光（表达"已启动"）
      const base = this.ctx.createLinearGradient(0, h - 10, 0, h);
      base.addColorStop(0, 'rgba(0,229,255,0)');
      base.addColorStop(1, 'rgba(0,229,255,0.30)');
      this.ctx.fillStyle = base;
      this.ctx.fillRect(0, h - 10, w, 10);
    }
    this.drawPercent(w, h, coverTop, coverH, prog);
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

  // ★ 星光点缀：夜幕薄纱自底上涨（上薄下厚，照片隐约透出），面纱内
  //   星星闪烁 + 周期流星；上沿星尘微光带随时间明灭
  private drawStarsVeil(w: number, h: number, coverTop: number, coverH: number): void {
    const c = this.ctx;
    // 面纱本体：顶边几乎透明 → 底部深蓝半透明
    const veil = c.createLinearGradient(0, coverTop, 0, h);
    veil.addColorStop(0, 'rgba(10,16,38,0.08)');
    veil.addColorStop(0.35, 'rgba(8,13,30,0.28)');
    veil.addColorStop(1, 'rgba(4,8,20,0.55)');
    c.fillStyle = veil;
    c.fillRect(0, coverTop, w, coverH);
    // 粒子区裁剪：星星/流星只出现在面纱内
    c.save();
    c.beginPath();
    c.rect(0, coverTop + 2, w, coverH - 2);
    c.clip();
    for (const s of this.stars) {
      const y = s.y * h;
      if (y < coverTop) {
        continue;   // 尚未被面纱淹没的星星不显示
      }
      const x = s.x * w;
      const tw = 0.30 + 0.70 * (0.5 + 0.5 * Math.sin(this.frame * s.speed + s.phase));
      const r = s.size * (0.6 + 0.4 * tw);
      // 光晕（径向渐变）
      const glow = c.createRadialGradient(x, y, 0, x, y, r * 4);
      glow.addColorStop(0, this.starColor(s.tint, 0.55 * tw));
      glow.addColorStop(1, this.starColor(s.tint, 0));
      c.fillStyle = glow;
      c.beginPath();
      c.arc(x, y, r * 4, 0, Math.PI * 2);
      c.fill();
      // 四芒（十字细线）
      c.strokeStyle = this.starColor(s.tint, 0.85 * tw);
      c.lineWidth = 0.8;
      c.beginPath();
      c.moveTo(x - r * 2.6, y);
      c.lineTo(x + r * 2.6, y);
      c.moveTo(x, y - r * 2.6);
      c.lineTo(x, y + r * 2.6);
      c.stroke();
      // 星核
      c.fillStyle = this.starColor(s.tint, Math.min(1, 0.4 + tw * 0.8));
      c.beginPath();
      c.arc(x, y, r * 0.7, 0, Math.PI * 2);
      c.fill();
    }
    // 流星：26 帧划过一轮（90 帧周期 ≈ 4.5s），裁剪保证只在面纱内可见
    const cometPhase = this.frame % 90;
    if (cometPhase < 26) {
      const p = cometPhase / 26;
      const cx = w * (0.15 + 0.85 * p);
      const cy = h * (0.06 + 0.5 * p);
      const trail = c.createLinearGradient(cx - 70, cy - 38, cx, cy);
      trail.addColorStop(0, 'rgba(160,230,255,0)');
      trail.addColorStop(1, 'rgba(200,245,255,0.85)');
      c.strokeStyle = trail;
      c.lineWidth = 1.4;
      c.beginPath();
      c.moveTo(cx - 70, cy - 38);
      c.lineTo(cx, cy);
      c.stroke();
    }
    c.restore();
    // 上沿：星尘微光带（明灭小点 + 轻微起伏）
    for (let x = 6; x < w; x += 13) {
      const tw = 0.5 + 0.5 * Math.sin(this.frame * 0.13 + x * 0.31);
      const a = 0.20 + 0.55 * tw;
      const ey = coverTop + 2.2 * Math.sin(this.frame * 0.05 + x * 0.06);
      c.fillStyle = `rgba(220,240,255,${a.toFixed(3)})`;
      c.beginPath();
      c.arc(x, ey, 0.9 + 0.9 * tw, 0, Math.PI * 2);
      c.fill();
    }
  }

  // ★ 水波涟漪：青蓝水体自底上涨，上沿双正弦行进波（主浪 + 回声浪 +
  //   辉光），浪下亮带；水内随机落点涟漪扩散（微光 + 主环 + 回声环）
  private waveY(x: number, coverTop: number): number {
    return coverTop + 5 * Math.sin(x * 0.018 + this.frame * 0.07)
      + 2.5 * Math.sin(x * 0.043 - this.frame * 0.045);
  }

  private drawWaterVeil(w: number, h: number, coverTop: number, coverH: number): void {
    const c = this.ctx;
    const minSide = Math.min(w, h);
    // 水体：上沿青亮 → 底部深蓝
    const veil = c.createLinearGradient(0, coverTop, 0, h);
    veil.addColorStop(0, 'rgba(0,90,160,0.30)');
    veil.addColorStop(0.4, 'rgba(0,60,120,0.40)');
    veil.addColorStop(1, 'rgba(0,28,66,0.58)');
    c.fillStyle = veil;
    c.fillRect(0, coverTop, w, coverH);
    // 涟漪（水体内部随机落点，裁剪限水面下）
    c.save();
    c.beginPath();
    c.rect(0, coverTop + 8, w, coverH - 8);
    c.clip();
    if (coverH > 70 && this.frame - this.lastRippleFrame > 14 && this.ripples.length < 9) {
      this.lastRippleFrame = this.frame;
      const ripple: EffectRipple = {
        x: 40 + Math.random() * Math.max(20, w - 80),
        y: coverTop + 26 + Math.random() * Math.max(20, coverH - 52),
        r: 4,
        maxR: minSide * (0.22 + Math.random() * 0.22),
        speed: 1.8 + Math.random() * 1.6
      };
      this.ripples.push(ripple);
    }
    for (let i = this.ripples.length - 1; i >= 0; i--) {
      const rp = this.ripples[i];
      rp.r += rp.speed;
      if (rp.r >= rp.maxR) {
        this.ripples.splice(i, 1);
        continue;
      }
      const fade = 1 - rp.r / rp.maxR;
      // 主环
      c.strokeStyle = `rgba(130,235,255,${(0.75 * fade).toFixed(3)})`;
      c.lineWidth = 1.6 + fade * 1.2;
      c.beginPath();
      c.arc(rp.x, rp.y, rp.r, 0, Math.PI * 2);
      c.stroke();
      // 回声环（半径 62%，更淡更细）
      c.strokeStyle = `rgba(130,235,255,${(0.3 * fade).toFixed(3)})`;
      c.lineWidth = 1;
      c.beginPath();
      c.arc(rp.x, rp.y, rp.r * 0.62, 0, Math.PI * 2);
      c.stroke();
      // 落点微光（刚生成时）
      if (rp.r < 26) {
        const dot = c.createRadialGradient(rp.x, rp.y, 0, rp.x, rp.y, 20);
        dot.addColorStop(0, 'rgba(190,245,255,0.8)');
        dot.addColorStop(1, 'rgba(190,245,255,0)');
        c.fillStyle = dot;
        c.beginPath();
        c.arc(rp.x, rp.y, 20, 0, Math.PI * 2);
        c.fill();
      }
    }
    c.restore();
    // 浪下亮带：波线与 +12px 之间更亮的水色
    c.beginPath();
    c.moveTo(0, this.waveY(0, coverTop));
    for (let x = 6; x <= w; x += 6) {
      c.lineTo(x, this.waveY(x, coverTop));
    }
    c.lineTo(w, coverTop + 14);
    c.lineTo(0, coverTop + 14);
    c.closePath();
    c.fillStyle = 'rgba(120,230,255,0.16)';
    c.fill();
    // 上沿波浪线：辉光 → 主浪 → 回声浪（三条描边）
    for (let pass = 0; pass < 3; pass++) {
      const off = pass === 0 ? 0 : (pass === 1 ? 0 : 5);
      if (pass === 0) {
        c.strokeStyle = 'rgba(120,235,255,0.14)';
        c.lineWidth = 8;
      } else if (pass === 1) {
        c.strokeStyle = 'rgba(170,240,255,0.90)';
        c.lineWidth = 2;
      } else {
        c.strokeStyle = 'rgba(170,240,255,0.35)';
        c.lineWidth = 1.2;
      }
      c.beginPath();
      c.moveTo(0, this.waveY(0, coverTop) + off);
      for (let x = 6; x <= w; x += 6) {
        c.lineTo(x, this.waveY(x, coverTop) + off);
      }
      c.stroke();
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

  // ★ 流光溢彩：暗紫薄纱上涨，彩色光雾被"关"在面纱内漂移呼吸
  //   （lighter 叠加），上沿多层辉光光线
  private drawMistVeil(w: number, h: number, coverTop: number, coverH: number): void {
    const c = this.ctx;
    const minSide = Math.min(w, h);
    const veil = c.createLinearGradient(0, coverTop, 0, h);
    veil.addColorStop(0, 'rgba(12,8,28,0.10)');
    veil.addColorStop(0.4, 'rgba(10,6,24,0.30)');
    veil.addColorStop(1, 'rgba(6,4,18,0.52)');
    c.fillStyle = veil;
    c.fillRect(0, coverTop, w, coverH);
    c.save();
    c.beginPath();
    c.rect(0, coverTop + 2, w, coverH - 2);
    c.clip();
    c.globalCompositeOperation = 'lighter';
    for (const m of this.mists) {
      const t = this.frame;
      const x = (m.bx + 0.10 * Math.sin(t * 0.012 + m.dx)) * w;
      const y = (m.by + 0.10 * Math.cos(t * 0.010 + m.dy)) * h;
      const r = m.r * minSide * (1 + 0.10 * Math.sin(t * 0.02 + m.phase));
      const alpha = 0.22 + 0.06 * Math.sin(t * 0.03 + m.phase) + this.progress * 0.10;
      const grad = c.createRadialGradient(x, y, 0, x, y, r);
      grad.addColorStop(0, `${this.mistColor(m.tint)}${Math.max(0.05, alpha).toFixed(3)})`);
      grad.addColorStop(0.7, `${this.mistColor(m.tint)}${Math.max(0.02, alpha * 0.45).toFixed(3)})`);
      grad.addColorStop(1, `${this.mistColor(m.tint)}0)`);
      c.fillStyle = grad;
      c.beginPath();
      c.arc(x, y, r, 0, Math.PI * 2);
      c.fill();
    }
    c.globalCompositeOperation = 'source-over';
    c.restore();
    // 上沿：三层辉光光线（宽淡 → 中亮 → 细白芯），亮度随时间呼吸
    const breath = 0.55 + 0.25 * Math.sin(this.frame * 0.10);
    const passes: number[] = [10, 4, 1.5];
    const colors: string[] = [
      'rgba(140,240,255,0.10)',
      `rgba(160,245,255,${(0.30 * breath).toFixed(3)})`,
      `rgba(225,250,255,${(0.85 * breath).toFixed(3)})`
    ];
    for (let pass = 0; pass < 3; pass++) {
      c.strokeStyle = colors[pass];
      c.lineWidth = passes[pass];
      c.beginPath();
      c.moveTo(0, coverTop);
      c.lineTo(w, coverTop);
      c.stroke();
    }
  }

  // ★ 光之扫描：已扫过区域青色尾迹渐变（贴线亮、向下渐隐），上沿为横向
  //   扫描亮线（辉光 + 白芯，亮度闪烁），火花自亮线坠入已扫区渐隐
  private drawScanVeil(w: number, h: number, coverTop: number, coverH: number): void {
    const c = this.ctx;
    // 暗色基底 + 青色尾迹（照片隐约透出）
    c.fillStyle = 'rgba(5,10,24,0.22)';
    c.fillRect(0, coverTop, w, coverH);
    const tail = c.createLinearGradient(0, coverTop, 0, h);
    tail.addColorStop(0, 'rgba(0,229,255,0.20)');
    tail.addColorStop(0.3, 'rgba(0,200,255,0.10)');
    tail.addColorStop(1, 'rgba(0,150,220,0.03)');
    c.fillStyle = tail;
    c.fillRect(0, coverTop, w, coverH);
    // 火花：自扫描线坠入已扫区（裁剪限制在面纱内）
    c.save();
    c.beginPath();
    c.rect(0, coverTop + 2, w, coverH - 2);
    c.clip();
    for (let i = 0; i < this.sparks.length; i++) {
      const sp = this.sparks[i];
      sp.y += sp.speed * 2.2;
      sp.life -= sp.speed;
      if (sp.life <= 0) {
        const fresh: EffectSpark = {
          x: Math.random(),
          y: 0,
          speed: 0.5 + Math.random() * 1.5,
          life: 10 + Math.random() * 24,
          size: 0.6 + Math.random() * 1.6
        };
        this.sparks[i] = fresh;
        continue;
      }
      const a = Math.min(1, sp.life / 18) * 0.85;
      c.fillStyle = `rgba(210,245,255,${a.toFixed(3)})`;
      c.beginPath();
      c.arc(sp.x * w, coverTop + sp.y, sp.size, 0, Math.PI * 2);
      c.fill();
    }
    c.restore();
    // 上沿扫描亮线：辉光（宽）+ 白芯（细），亮度高频闪烁
    const flick = 0.75 + 0.2 * Math.sin(this.frame * 0.3);
    c.strokeStyle = `rgba(120,235,255,${(0.22 * flick).toFixed(3)})`;
    c.lineWidth = 12;
    c.beginPath();
    c.moveTo(0, coverTop);
    c.lineTo(w, coverTop);
    c.stroke();
    c.strokeStyle = `rgba(255,255,255,${(0.95 * flick).toFixed(3)})`;
    c.lineWidth = 2;
    c.beginPath();
    c.moveTo(0, coverTop);
    c.lineTo(w, coverTop);
    c.stroke();
  }

  // ★ 百分比大字：面纱中央（面纱太薄时移到边线上方），白色粗体 +
  //   青色辉光 + 深色描边（任意照片背景可读），下带"修复进度"小字
  private drawPercent(w: number, h: number, coverTop: number,
    coverH: number, prog: number): void {
    const c = this.ctx;
    const pct = Math.round(prog * 100);
    const fontSize = Math.max(24, Math.min(46, Math.min(w, h) * 0.15));
    // 纵向落点：面纱够高 → 面纱中央；面纱太薄 → 边线上方（不低于顶、不出底）
    let py: number;
    if (coverH > fontSize * 2.2) {
      py = coverTop + coverH / 2;
    } else {
      py = Math.max(fontSize * 0.8 + 6, coverTop - fontSize * 0.7);
      if (py > h - fontSize * 0.7 - 4) {
        py = h - fontSize * 0.7 - 4;
      }
    }
    c.textAlign = 'center';
    c.textBaseline = 'middle';
    c.font = `bold ${fontSize}px sans-serif`;
    c.shadowColor = 'rgba(0,229,255,0.85)';
    c.shadowBlur = 16;
    c.lineWidth = 3;
    c.strokeStyle = 'rgba(0,12,20,0.55)';
    c.strokeText(`${pct}%`, w / 2, py);
    c.fillStyle = 'rgba(255,255,255,0.96)';
    c.fillText(`${pct}%`, w / 2, py);
    // 小字标签（百分比下方）
    c.font = `${Math.max(10, Math.round(fontSize * 0.28))}px sans-serif`;
    c.shadowColor = 'rgba(0,229,255,0.6)';
    c.shadowBlur = 8;
    c.lineWidth = 2;
    c.strokeStyle = 'rgba(0,12,20,0.5)';
    c.strokeText('修复进度', w / 2, py + fontSize * 0.75);
    c.fillStyle = 'rgba(159,232,255,0.92)';
    c.fillText('修复进度', w / 2, py + fontSize * 0.75);
    c.shadowBlur = 0;
  }
}
