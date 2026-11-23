// ★ YOLO 模型管理：rawfile 模型提取到沙箱、模型清单（models/models.json）
//   解析与类别标签加载。数据流：init 读清单 → prepare(id) 把 NCNN 的
//   .param/.bin 释放到 filesDir 并缓存 ModelBundle → Index 拿 bundle 里的
//   沙箱路径调 native（libentry.so）加载推理，labels 供检测框命名与勾选列表
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { util } from '@kit.ArkTS';

// ★ ModelInfo：models.json 中一个模型的描述（文件均指 rawfile 内相对路径）
export interface ModelInfo {
  id: string;
  name: string;
  paramFile: string;
  binFile: string;
  labelsFile: string;
  inputSize: number;
  type: string;
}

// ★ ModelBundle：prepare 的产物——沙箱绝对路径 + 标签列表（可直接喂给 native）
export interface ModelBundle {
  model: ModelInfo;
  paramPath: string;
  binPath: string;
  labels: string[];
}

// ★ models.json 根结构：默认模型 id + 模型数组
interface ModelsJson {
  defaultModelId: string;
  models: ModelInfo[];
}

// ★ ModelManager：模型清单管理 + 沙箱释放缓存（cache 按 id 复用，避免重复大 IO）
export class ModelManager {
  private ctx: common.UIAbilityContext | null = null;
  private models: ModelInfo[] = [];
  private defaultId: string = '';
  private cache: Map<string, ModelBundle> = new Map();

  // ★ init：解析 rawfile 的 models/models.json，确定默认模型 id；
  //   清单缺失/损坏时保持空列表，上层按"无模型"降级处理
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

  // ★ 模型列表（模型选择弹窗数据源）与默认模型 id
  getModels(): ModelInfo[] { return this.models; }
  getDefaultId(): string { return this.defaultId; }

  // ★ 按 id 精确查找模型定义（prepare 的前置校验）
  findById(id: string): ModelInfo | null {
    for (let i = 0; i < this.models.length; i++) {
      if (this.models[i].id === id) return this.models[i];
    }
    return null;
  }

  // ★ prepare：模型上线——释放 .param/.bin 到沙箱、读类别标签、组装并缓存
  //   bundle；同 id 二次调用直接命中缓存（rawfile 释放是几十 MB 级 IO，只做一次）
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

  // ★ rawfile → 沙箱拷贝：目标已存在且 >1KB 视为上次已释放，跳过重复拷贝
  //   （rawfile 只读、不存在被改写的场景），大幅缩短二次启动时间
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

  // ★ 读 rawfile 文本（类别标签 txt），统一 UTF-8 解码
  private readRawText(rawPath: string): string {
    const ctx = this.ctx;
    if (!ctx) throw new Error('ctx null');
    const raw = ctx.resourceManager.getRawFileContentSync(rawPath);
    return bufferToUtf8(raw);
  }
}

// ★ bufferToUtf8：Uint8Array → 字符串（models.json 与标签 txt 共用）
function bufferToUtf8(buf: Uint8Array): string {
  const decoder = new util.TextDecoder('utf-8');
  return decoder.decodeToString(buf);
}