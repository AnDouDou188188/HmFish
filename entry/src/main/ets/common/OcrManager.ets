// ★ OCR 模型集管理：内置 paddle/paddle5 两套 + 沙箱外部模型集自动发现
//   （filesDir/ocrmodels/<id>/），rawfile 提取到沙箱供 native 加载
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import entry from 'libentry.so';

// ★ OCR 模型集：一个目录 = 一套 det/cls/rec/keys（任意命名）。
//   内置两套：paddle（PP-OCRv4 普通文本）、paddle5（PP-OCRv5 票据高精度）；
//   外部模型集把模型文件放入 filesDir/ocrmodels/<id>/ 即自动出现在长按 OCR 的模型选择列表
export interface OcrModelSet {
  id: string;
  name: string;
  dir: string;
  det: string;
  cls: string;
  rec: string;
  keys: string;
}

// config.json 字段（外部模型集可选提供：显式指定文件名与显示名）
interface OcrSetConfig {
  name?: string;
  det?: string;
  cls?: string;
  rec?: string;
  keys?: string;
}

// 内置模型集定义：rawfile 文件名 → 沙箱目录
interface BuiltinSetDef {
  id: string;
  name: string;
  det: string;
  cls: string;
  rec: string;
  keys: string;
}

const BUILTIN_SETS: BuiltinSetDef[] = [
  {
    id: 'paddle',
    name: 'PaddleOCR 普通文本',
    det: 'ch_PP-OCRv4_det_infer.onnx',
    cls: 'ch_ppocr_mobile_v2.0_cls_infer.onnx',
    rec: 'ch_PP-OCRv4_rec_infer.onnx',
    keys: 'ppocr_keys_v1.txt'
  },
  {
    id: 'paddle5',
    name: 'PP-OCRv5 票据高精度',
    det: 'ch_PP-OCRv5_det_mobile.onnx',
    cls: 'ch_PP-LCNet_x0_25_textline_ori_cls_mobile.onnx',
    rec: 'ch_PP-OCRv5_rec_mobile.onnx',
    keys: 'ppocrv5_dict.txt'
  }
];

// 目录名 → 显示名（config.json 的 name 优先）
const SET_NAMES: Record<string, string> = {
  'paddle': 'PaddleOCR 普通文本',
  'paddle5': 'PP-OCRv5 票据高精度',
  'xiaomi': 'Xiaomi-OCR 票据格式'
};

export class OcrManager {
  private inited: boolean = false;
  private initing: boolean = false;
  private initedSetId: string = '';

  get ready(): boolean {
    return this.inited;
  }

  get currentSetId(): string {
    return this.initedSetId;
  }

  // 内置模型集定义查找
  private findBuiltinDef(id: string): BuiltinSetDef | null {
    for (let i = 0; i < BUILTIN_SETS.length; i++) {
      if (BUILTIN_SETS[i].id === id) return BUILTIN_SETS[i];
    }
    return null;
  }

  getBuiltinSet(ctx: common.UIAbilityContext, id: string): OcrModelSet {
    const def = this.findBuiltinDef(id) ?? BUILTIN_SETS[0];
    return {
      id: def.id,
      name: def.name,
      dir: `${ctx.filesDir}/ocrmodels/${def.id}`,
      det: def.det,
      cls: def.cls,
      rec: def.rec,
      keys: def.keys
    };
  }

  // 内置模型集：rawfile → 沙箱（已存在且够大跳过）
  private ensureBuiltinFiles(ctx: common.UIAbilityContext, id: string): void {
    const def = this.findBuiltinDef(id);
    if (def === null) return;
    const dir = `${ctx.filesDir}/ocrmodels/${def.id}`;
    try { fs.mkdirSync(dir, true); } catch (_) {}
    const names: string[] = [def.det, def.cls, def.rec, def.keys];
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      const dest = `${dir}/${name}`;
      let need = true;
      try {
        const st = fs.statSync(dest);
        if (st.size > 100) need = false;
      } catch (_) {}
      if (need) {
        const data = ctx.resourceManager.getRawFileContentSync(name);
        const f = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
        fs.writeSync(f.fd, data.buffer);
        fs.closeSync(f);
      }
    }
  }

  // 列出可用模型集：内置两套 + 扫描 ocrmodels/*（config.json 优先，否则 det/cls/rec+txt 启发式）
  listSets(ctx: common.UIAbilityContext): OcrModelSet[] {
    const sets: OcrModelSet[] = [];
    for (let i = 0; i < BUILTIN_SETS.length; i++) {
      this.ensureBuiltinFiles(ctx, BUILTIN_SETS[i].id);
      sets.push(this.getBuiltinSet(ctx, BUILTIN_SETS[i].id));
    }
    try {
      const root = `${ctx.filesDir}/ocrmodels`;
      const names = fs.listFileSync(root);
      for (let i = 0; i < names.length; i++) {
        const id = names[i];
        if (this.findBuiltinDef(id) !== null) continue;
        const dir = `${root}/${id}`;
        try {
          const st = fs.statSync(dir);
          if (!st.isDirectory()) continue;
        } catch (_) { continue; }
        const set = this.scanDir(id, dir);
        if (set !== null) sets.push(set);
      }
    } catch (_) {}
    return sets;
  }

  // 扫描一个模型集目录：config.json 显式指定 → 校验存在；否则按文件名启发式匹配
  private scanDir(id: string, dir: string): OcrModelSet | null {
    // config.json 优先
    try {
      const cfgPath = `${dir}/config.json`;
      if (fs.accessSync(cfgPath)) {
        const cfg = JSON.parse(fs.readTextSync(cfgPath)) as OcrSetConfig;
        if (cfg.det && cfg.cls && cfg.rec && cfg.keys &&
          fs.accessSync(`${dir}/${cfg.det}`) && fs.accessSync(`${dir}/${cfg.cls}`) &&
          fs.accessSync(`${dir}/${cfg.rec}`) && fs.accessSync(`${dir}/${cfg.keys}`)) {
          const set: OcrModelSet = {
            id: id,
            name: cfg.name ?? (SET_NAMES[id] ?? id),
            dir: dir,
            det: cfg.det,
            cls: cfg.cls,
            rec: cfg.rec,
            keys: cfg.keys
          };
          return set;
        }
      }
    } catch (_) {}
    // 启发式：det/cls/rec onnx + 第一个 txt
    try {
      const files = fs.listFileSync(dir);
      let det = '';
      let cls = '';
      let rec = '';
      let keys = '';
      for (let i = 0; i < files.length; i++) {
        const f = files[i];
        const lower = f.toLowerCase();
        if (lower.endsWith('.onnx')) {
          if (lower.includes('det') && det === '') det = f;
          else if (lower.includes('cls') && cls === '') cls = f;
          else if (lower.includes('rec') && rec === '') rec = f;
        } else if (lower.endsWith('.txt') && keys === '') {
          keys = f;
        }
      }
      if (det !== '' && cls !== '' && rec !== '' && keys !== '') {
        const set: OcrModelSet = {
          id: id,
          name: SET_NAMES[id] ?? id,
          dir: dir,
          det: det,
          cls: cls,
          rec: rec,
          keys: keys
        };
        return set;
      }
    } catch (_) {}
    return null;
  }

  // 初始化指定模型集（按显式文件路径，任意命名均可）
  async initSet(ctx: common.UIAbilityContext, setId: string): Promise<boolean> {
    if (this.initing) return false;
    if (this.inited && this.initedSetId === setId) return true;
    let set: OcrModelSet | null = null;
    if (this.findBuiltinDef(setId) !== null) {
      this.ensureBuiltinFiles(ctx, setId);
      set = this.getBuiltinSet(ctx, setId);
    } else {
      set = this.scanDir(setId, `${ctx.filesDir}/ocrmodels/${setId}`);
    }
    if (set === null) {
      console.error('[OcrManager] model set not found: ' + setId);
      return false;
    }
    this.initing = true;
    try {
      const ok = await entry.initOcrPaths(
        `${set.dir}/${set.det}`,
        `${set.dir}/${set.cls}`,
        `${set.dir}/${set.rec}`,
        `${set.dir}/${set.keys}`);
      this.inited = ok;
      if (ok) {
        this.initedSetId = setId;
      } else {
        console.error('[OcrManager] initOcrPaths failed: ' + setId);
      }
      return ok;
    } catch (e) {
      console.error('[OcrManager] init failed', e);
      return false;
    } finally {
      this.initing = false;
    }
  }

  // 切换模型集：释放旧引擎再按新路径初始化
  async switchSet(ctx: common.UIAbilityContext, setId: string): Promise<boolean> {
    try { entry.setOcrEnabled(false); } catch (_) {}
    try { entry.releaseOcr(); } catch (_) {}
    this.inited = false;
    this.initedSetId = '';
    return this.initSet(ctx, setId);
  }

  // 兼容旧入口：初始化当前（默认 paddle）模型集
  async ensureInited(ctx: common.UIAbilityContext): Promise<boolean> {
    return this.initSet(ctx, this.initedSetId !== '' ? this.initedSetId : 'paddle');
  }

  // ═══════ 表格结构识别（PP-Structure SLANet）═══════
  // rawfile → filesDir/ocrmodels/table/，幂等（已存在且够大跳过）
  private tableModelPath(ctx: common.UIAbilityContext): string {
    return `${ctx.filesDir}/ocrmodels/table/ch_ppstructure_mobile_v2_SLANet.onnx`;
  }

  private ensureTableModel(ctx: common.UIAbilityContext): void {
    const dir = `${ctx.filesDir}/ocrmodels/table`;
    try { fs.mkdirSync(dir, true); } catch (_) {}
    const dest = this.tableModelPath(ctx);
    let need = true;
    try {
      const st = fs.statSync(dest);
      if (st.size > 100) need = false;
    } catch (_) {}
    if (need) {
      const data = ctx.resourceManager.getRawFileContentSync('ch_ppstructure_mobile_v2_SLANet.onnx');
      const f = fs.openSync(dest, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(f.fd, data.buffer);
      fs.closeSync(f);
    }
  }

  private tableInited: boolean = false;
  private tableIniting: boolean = false;

  // 加载 SLANet 表格模型（幂等；模型常驻，不随 OCR 引擎释放）
  async initTable(ctx: common.UIAbilityContext): Promise<boolean> {
    if (this.tableInited) return true;
    if (this.tableIniting) return false;
    this.tableIniting = true;
    try {
      this.ensureTableModel(ctx);
      const ok = await entry.initTable(this.tableModelPath(ctx));
      this.tableInited = ok;
      if (!ok) {
        console.error('[OcrManager] initTable failed');
      }
      return ok;
    } catch (e) {
      console.error('[OcrManager] initTable exception', e);
      return false;
    } finally {
      this.tableIniting = false;
    }
  }
}
