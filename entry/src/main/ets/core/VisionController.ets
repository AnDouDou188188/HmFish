// ★ AI 视觉控制中枢：ArkTS 侧对 native（libentry.so）识别产出的唯一轮询入口。
//   native 侧自行跑 YOLO/OCR/水深/动态检测并把结果写入共享状态；本控制器
//   以 120ms 定时轮询拉取：检测框（getLatestResults）、动态等级、水深、
//   OCR 文本与进度、推理计数（算 fps），经过滤/聚合后经回调分发给
//   Index.ets 画框、计数 HUD、语音播报与自动录像触发
import entry from 'libentry.so';
import { Det, Merged, CountInfo } from '../common/Types';

// ★ VisionController：轮询生命周期管理 + 数据加工（阈值过滤/类别聚合/排序）
export class VisionController {
  // ★ 轮询定时器句柄：stop 后置 -1（-1 = 未在轮询）
  private pollTimer: number = -1;
  // ★ fps 统计基准：每满 1s 用推理计数差值算一次帧率
  private lastFpsTime: number = 0;
  private lastInfer: number = 0;
  // ★ 当前模型的类别表（label 下标 → 中文名）
  private modelLabels: string[] = [];
  // ★ 置信度阈值：低于该值的检测框在 ArkTS 侧再过滤一道
  private confThresh: number = 0.25;
  private yoloEnabled: boolean = true;

  // ★ 近 1 秒推理帧率（HUD 显示）
  public fps: number = 0;
  // ★ OCR 去重：文本/进度变化才触发回调，避免每 120ms 刷一次 UI
  private lastOcrText: string = '';
  private lastOcrProgress: string = '';
  public onResult?: (list: Det[], merged: Merged[], motionLevel: number, depthCm: number) => void;
  public onFpsUpdate?: (fps: number) => void;
  public onOcrText?: (text: string) => void;
  // ★ OCR 识别进度（done/total）：供 HUD"识别中"徽标显示
  public onOcrProgress?: (done: number, total: number) => void;

  // ★ setConfig：切换模型/调整置信度后同步过滤参数（Index 在模型切换时调用）
  setConfig(labels: string[], conf: number, yoloEnabled: boolean) {
    this.modelLabels = labels;
    this.confThresh = conf;
    this.yoloEnabled = yoloEnabled;
  }

  // ★ setYoloEnabled：YOLO 总开关——同时下发 native（停推理省电）与本地过滤
  setYoloEnabled(enabled: boolean) {
    this.yoloEnabled = enabled;
    try { entry.setYoloEnabled(enabled); } catch (_) {}
  }

  // ★ start：启动 120ms 轮询循环（先 stop 防重复启动）；
  //   轮询体内所有 native 调用均有 try 兜底，单次异常不会中断循环
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

  // ★ stop：停轮询（页面隐藏/切后台时调用，回到前台重新 start）
  stop() {
    if (this.pollTimer !== -1) { clearTimeout(this.pollTimer); this.pollTimer = -1; }
  }

  // ★ consume：native 裸数组 → Det 列表的加工线：
  //   过滤残行/低置信 → 查类别名 → 同类聚合（CountInfo→Merged）→
  //   按均分/得分降序排序（HUD 统计顺序与画框优先级依据）
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