// ★ 识别结果轮询控制器：统一轮询 native 检测结果/fps/OCR 进度并回调分发
import entry from 'libentry.so';
import { Det, Merged, CountInfo } from '../common/Types';

export class VisionController {
  private pollTimer: number = -1;
  private lastFpsTime: number = 0;
  private lastInfer: number = 0;
  private modelLabels: string[] = [];
  private confThresh: number = 0.25;
  private yoloEnabled: boolean = true;

  public fps: number = 0;
  private lastOcrText: string = '';
  private lastOcrProgress: string = '';
  public onResult?: (list: Det[], merged: Merged[], motionLevel: number, depthCm: number) => void;
  public onFpsUpdate?: (fps: number) => void;
  public onOcrText?: (text: string) => void;
  // ★ OCR 识别进度（done/total）：供 HUD"识别中"徽标显示
  public onOcrProgress?: (done: number, total: number) => void;

  setConfig(labels: string[], conf: number, yoloEnabled: boolean) {
    this.modelLabels = labels;
    this.confThresh = conf;
    this.yoloEnabled = yoloEnabled;
  }

  setYoloEnabled(enabled: boolean) {
    this.yoloEnabled = enabled;
    try { entry.setYoloEnabled(enabled); } catch (_) {}
  }

  start() {
    this.stop();
    this.lastFpsTime = Date.now();
    this.lastInfer = entry.getInferCount() as number;

    const poll = (): void => {
      try {
        const raw = entry.getLatestResults() as number[][];
        const motionLevel = entry.getMotionLevel() as number;
        const depthCm = entry.getDepthCm() as number;
        this.consume(raw, motionLevel, depthCm);

        const ocrText = entry.getOcrText() as string;
        if (ocrText !== this.lastOcrText) {
          this.lastOcrText = ocrText;
          this.onOcrText?.(ocrText);
        }

        // ★ OCR 识别进度：只在变化时回调（HUD 识别中徽标显示 done/total）
        const prog = entry.getOcrProgress() as number[];
        if (prog && prog.length >= 2) {
          const key = `${prog[0]}/${prog[1]}`;
          if (key !== this.lastOcrProgress) {
            this.lastOcrProgress = key;
            this.onOcrProgress?.(prog[0], prog[1]);
          }
        }

        const now = Date.now();
        if (now - this.lastFpsTime >= 1000) {
          const cur = entry.getInferCount() as number;
          this.fps = cur - this.lastInfer;
          this.lastInfer = cur;
          this.lastFpsTime = now;
          this.onFpsUpdate?.(this.fps);
        }
      } catch (_) {}
      this.pollTimer = setTimeout(poll, 120);
    };
    this.pollTimer = setTimeout(poll, 120);
  }

  stop() {
    if (this.pollTimer !== -1) { clearTimeout(this.pollTimer); this.pollTimer = -1; }
  }

  private consume(raw: number[][], motionLevel: number, depthCm: number): void {
    if (!this.yoloEnabled) {
      this.onResult?.([], [], motionLevel, depthCm);
      return;
    }

    const list: Det[] = [];
    const map: Map<number, CountInfo> = new Map();

    for (let i = 0; i < raw.length; i++) {
      const d = raw[i];
      if (!d || d.length < 6) continue;
      const label = Math.floor(d[5]);
      const score = d[4];
      if (score < this.confThresh) continue;

      const name = label >= 0 && label < this.modelLabels.length ? this.modelLabels[label] : `目标${label}`;
      const det: Det = { label, name, score, x: d[0], y: d[1], w: d[2], h: d[3] };
      list.push(det);

      let e = map.get(label);
      if (!e) { e = { count: 0, total: 0, name }; map.set(label, e); }
      e.count++; e.total += score;
    }

    const merged: Merged[] = [];
    map.forEach((v: CountInfo, k: number) => {
      const m: Merged = { label: k, name: v.name, count: v.count, avgScore: v.total / v.count };
      merged.push(m);
    });
    merged.sort((a, b) => b.avgScore - a.avgScore);
    list.sort((a, b) => b.score - a.score);

    this.onResult?.(list, merged, motionLevel, depthCm);
  }
}