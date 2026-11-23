// ★ 通用工具集：跨页面公共函数（不含 UI 依赖，可被任意层引用）
//   1) saveRgbaWithContext：把 native 吐出的 RGBA/BGRA 帧打包成 PNG 落盘沙箱，
//      供拍照（MediaController.snapshot）保存到系统图库；
//   2) MotionDetector：检测框帧间位移度量，供"画面动起来再跑 YOLO"的
//      空闲降频策略使用（Index.ets 依据 motionLevel 决定识别节奏）
//   与 native（libentry.so）无直接交互，只消费 native 产出的帧/检测结果数据
import { image } from '@kit.ImageKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ MotionInput：参与位移度量的检测框输入（label + 归一化坐标），
//   与 native 返回的 raw 检测行结构对应（去掉 name/score 冗余字段）
export interface MotionInput {
  label: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

// ★ Box：位移度量内部使用的最小矩形（归一化 x/y/w/h）
interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}

// ★ saveRgbaWithContext：RGBA 缓冲 → PixelMap → PNG → 沙箱文件
//   返回沙箱路径（失败返回 null，由调用方决定是否回退）。
//   注意 pixelFormat 声明为 BGRA_8888：native 渲染产物按 BGRA 字节序输出
export async function saveRgbaWithContext(ctx: common.UIAbilityContext,
  rgba: ArrayBuffer, w: number, h: number): Promise<string | null> {
  try {
    const opts: image.InitializationOptions = {
      size: { width: w, height: h },
      pixelFormat: image.PixelMapFormat.BGRA_8888,
      editable: false
    };
    const pixelMap = await image.createPixelMap(rgba, opts);
    const packer = image.createImagePacker();
    const data = await packer.packing(pixelMap, { format: 'image/png', quality: 100 });
    packer.release();
    pixelMap.release();

    const path = ctx.filesDir + `/snapshot_${Date.now()}.png`;
    const f = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    fs.writeSync(f.fd, data);
    fs.closeSync(f);
    console.info(`[Utils] snapshot saved: ${path}`);
    return path;
  } catch (e) {
    const err = e as BusinessError;
    console.error(`[Utils] snapshot failed: ${err.message}`);
    return null;
  }
}

// ★ MotionDetector：帧间位移度量器
//   用"label+粗略x"哈希把上一帧与当前帧的检测框配对，
//   输出平均位移量（0=完全静止），值越大画面越"动"
export class MotionDetector {
  // ★ 上一帧检测框快照（key=配对哈希），update 结束整体替换
  private prev: Map<number, Box> = new Map();

  // ★ update：输入本帧检测框，返回与上一帧的平均位移
  //   key = label*10000 + round(x*1000)：同类目标按横向位置粗配对，
  //   无需精确跟踪（IoU 匹配太重），够区分"静止/缓动/剧烈移动"即可
  update(dets: MotionInput[]): number {
    let totalShift = 0;
    let matched = 0;
    const cur: Map<number, Box> = new Map();
    for (let i = 0; i < dets.length; i++) {
      const d = dets[i];
      const key = d.label * 10000 + Math.round(d.x * 1000);
      const box: Box = { x: d.x, y: d.y, w: d.w, h: d.h };
      cur.set(key, box);
    }
    cur.forEach((box: Box, key: number) => {
      const p = prev_get(this.prev, key);
      if (p) {
        const shift = Math.abs(box.x - p.x) + Math.abs(box.y - p.y)
          + Math.abs(box.w - p.w) + Math.abs(box.h - p.h);
        totalShift += shift;
        matched++;
      }
    });
    this.prev = cur;
    return matched > 0 ? totalShift / matched : 0;
  }
}

// ★ prev_get：Map.get 的具名包装（取值逻辑集中一处，便于空安全收窄）
function prev_get(m: Map<number, Box>, key: number): Box | undefined {
  return m.get(key);
}