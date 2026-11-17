// ★ YOLO 模型管理：rawfile 模型提取到沙箱、模型清单解析与标签加载
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';

export interface ModelInfo {
  id: string;
  name: string;
  paramFile: string;
  binFile: string;
  labelsFile: string;
  inputSize: number;
  type: string;
}

export interface ModelBundle {
  model: ModelInfo;
  paramPath: string;
  binPath: string;
  labels: string[];
}

interface ModelsJson {
  defaultModelId: string;
  models: ModelInfo[];
}

export class ModelManager {
  private ctx: common.UIAbilityContext | null = null;
  private models: ModelInfo[] = [];
  private defaultId: string = '';
  private cache: Map<string, ModelBundle> = new Map();

  async init(ctx: common.UIAbilityContext): Promise<void> {
    this.ctx = ctx;
    try {
      const raw = ctx.resourceManager.getRawFileContentSync('models/models.json');
      const text = bufferToUtf8(raw);
      const json = JSON.parse(text) as ModelsJson;
      this.models = json.models;
      if (json.defaultModelId) {
        this.defaultId = json.defaultModelId;
      } else if (this.models.length > 0) {
        this.defaultId = this.models[0].id;
      }
      console.info(`[ModelManager] loaded ${this.models.length} models, default=${this.defaultId}`);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[ModelManager] ❌ load failed: ${err.code} ${err.message}`);
    }
  }

  getModels(): ModelInfo[] { return this.models; }
  getDefaultId(): string { return this.defaultId; }

  findById(id: string): ModelInfo | null {
    for (let i = 0; i < this.models.length; i++) {
      if (this.models[i].id === id) return this.models[i];
    }
    return null;
  }

  async prepare(id: string): Promise<ModelBundle | null> {
    if (!this.ctx) return null;
    const cached = this.cache.get(id);
    if (cached) return cached;

    const info = this.findById(id);
    if (!info) return null;

    try {
      // 拷贝 .param 和 .bin 到沙箱
      const sandboxParam = this.copyRawToSandbox(info.paramFile);
      const sandboxBin = this.copyRawToSandbox(info.binFile);

      // 读取类别 txt
      const labelsText = this.readRawText(info.labelsFile);
      const labels = labelsText.split('\n').map(s => s.trim()).filter(s => s.length > 0);

      const bundle: ModelBundle = {
        model: info,
        paramPath: sandboxParam,
        binPath: sandboxBin,
        labels: labels
      };
      this.cache.set(id, bundle);
      return bundle;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[ModelManager] ❌ prepare ${id} failed: ${err.code} ${err.message}`);
      return null;
    }
  }

  private copyRawToSandbox(rawPath: string): string {
    const ctx = this.ctx;
    if (!ctx) throw new Error('ctx null');
    const parts = rawPath.split('/');
    const fileName = parts.length > 0 ? parts[parts.length - 1] : 'model.bin';
    const sandbox = ctx.filesDir + '/' + fileName;
    try {
      const st = fs.statSync(sandbox);
      if (st.size > 1024) return sandbox; // 文件存在直接返回
    } catch (_) {}

    const raw = ctx.resourceManager.getRawFileContentSync(rawPath);
    const f = fs.openSync(sandbox, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
    fs.writeSync(f.fd, raw.buffer);
    fs.closeSync(f);
    return sandbox;
  }

  private readRawText(rawPath: string): string {
    const ctx = this.ctx;
    if (!ctx) throw new Error('ctx null');
    const raw = ctx.resourceManager.getRawFileContentSync(rawPath);
    return bufferToUtf8(raw);
  }
}

function bufferToUtf8(buf: Uint8Array): string {
  const decoder = new util.TextDecoder('utf-8');
  return decoder.decodeToString(buf);
}