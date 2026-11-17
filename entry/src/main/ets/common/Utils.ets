// ★ 通用工具：RGBA 帧保存为图片（沙箱→图库）等跨页面公共函数
import { image } from '@kit.ImageKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

export interface MotionInput {
  label: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}

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

export class MotionDetector {
  private prev: Map<number, Box> = new Map();

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

function prev_get(m: Map<number, Box>, key: number): Box | undefined {
  return m.get(key);
}