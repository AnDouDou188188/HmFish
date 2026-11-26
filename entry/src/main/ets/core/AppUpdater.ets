// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ AppUpdater（AppUpdater.ets）—— 应用内自动更新（GitHub Releases）              ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】                                                                     ║
// ║  1. checkLatest：GET releases/latest（3 次重试，5s/15s 退避；GitHub 在        ║
// ║     国内常不可达，重试是必须的），解析 .hap 资产 → ReleaseInfo                ║
// ║  2. isNewer：tag（vX.Y）与本地版本逐段数值比较                                ║
// ║  3. downloadHap：requestInStream 流式下载到沙箱 cache（Range 断点续传；       ║
// ║     最多 5 次重试，3s/10s/30s/60s 退避；完成后按资产大小校验再落最终文件）     ║
// ║  4. saveToPublicDir：DocumentViewPicker.save 存到"下载"等公共目录，           ║
// ║     用户点 hap 由系统安装器安装（普通应用无静默安装权限）                      ║
// ║                                                                              ║
// ║ 【发布约定】                                                                 ║
// ║  GitHub Release tag = vX.Y（如 v2.2），附件 = 签名 hap                        ║
// ║  （entry-default-signed.hap 或 HmFish_vX.Y.hap 均可，取第一个 .hap）          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { http } from '@kit.NetworkKit';
import { common } from '@kit.AbilityKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { picker } from '@kit.CoreFileKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ GitHub releases/latest API（仓库固定）
const GH_RELEASE_API: string = 'https://api.github.com/repos/AnDouDou188188/HmFish/releases/latest';

// ★ 更新信息（解析后的最终模型，页面直接消费）
export class ReleaseInfo {
  tag: string = '';
  name: string = '';
  notes: string = '';
  url: string = '';
  assetName: string = '';
  assetUrl: string = '';
  assetSize: number = 0;
}

// ★ GitHub API JSON 原始模型（字段可选，逐个校验后再拷入 ReleaseInfo）
class GhAssetRaw {
  name?: string;
  browser_download_url?: string;
  size?: number;
}

class GhReleaseRaw {
  tag_name?: string;
  name?: string;
  body?: string;
  html_url?: string;
  assets?: GhAssetRaw[];
}

// ★ sleep 工具（重试退避用）
function sleepMs(ms: number): Promise<void> {
  return new Promise<void>((resolve) => setTimeout(resolve, ms));
}

// ★ AppUpdater：检查/下载/保存三段式更新器
export class AppUpdater {
  private canceled: boolean = false;

  // ★ 取消进行中的下载（下载中用户点"取消"）：下一次重试前抛出
  cancel(): void {
    this.canceled = true;
  }

  // ★ 检查最新版本：3 次重试（5s/15s 退避），全部失败抛错；
  //   成功但 latest 不含 .hap 资产时 assetUrl 为空（页面提示走浏览器）
  async checkLatest(): Promise<ReleaseInfo> {
    let lastErr: BusinessError | null = null;
    for (let attempt = 0; attempt < 3; attempt++) {
      if (attempt > 0) await sleepMs(attempt === 1 ? 5000 : 15000);
      try {
        return await this.fetchReleaseOnce();
      } catch (e) {
        lastErr = e as BusinessError;
      }
    }
    throw lastErr !== null ? lastErr : new Error('检查更新失败');
  }

  // ★ 单次检查：GET releases/latest → 校验 → 解析 .hap 资产
  private async fetchReleaseOnce(): Promise<ReleaseInfo> {
    const client = http.createHttp();
    try {
      const header: Record<string, string> = {
        'User-Agent': 'HmFish-Updater',
        'Accept': 'application/vnd.github+json'
      };
      const resp = await client.request(GH_RELEASE_API, {
        method: http.RequestMethod.GET,
        connectTimeout: 10000,
        readTimeout: 15000,
        header: header
      });
      if (resp.responseCode !== 200) {
        throw new Error(`HTTP ${resp.responseCode}`);
      }
      const text = typeof resp.result === 'string' ? resp.result : '';
      if (text.length === 0) {
        throw new Error('响应为空');
      }
      const raw = JSON.parse(text) as GhReleaseRaw;
      if (raw.tag_name === undefined || raw.tag_name === null || raw.tag_name.length === 0) {
        throw new Error('发布信息缺失 tag');
      }
      const info = new ReleaseInfo();
      info.tag = raw.tag_name;
      info.name = raw.name ?? '';
      info.notes = raw.body ?? '';
      info.url = raw.html_url ?? '';
      if (raw.assets !== undefined && raw.assets !== null) {
        for (const a of raw.assets) {
          const an = a.name ?? '';
          const au = a.browser_download_url ?? '';
          if (an.endsWith('.hap') && au.length > 0) {
            info.assetName = an;
            info.assetUrl = au;
            info.assetSize = a.size ?? 0;
            break;
          }
        }
      }
      return info;
    } finally {
      client.destroy();
    }
  }

  // ★ 版本比较：远端 tag（可带 v 前缀）是否比本地 version 新（逐段数值比较）
  isNewer(remoteTag: string, currentVersion: string): boolean {
    const r = remoteTag.replace(/^v/i, '').split('.');
    const c = currentVersion.replace(/^v/i, '').split('.');
    const n = Math.max(r.length, c.length);
    for (let i = 0; i < n; i++) {
      const rv = i < r.length ? parseInt(r[i]) : 0;
      const cv = i < c.length ? parseInt(c[i]) : 0;
      if (isNaN(rv) || isNaN(cv)) return false;
      if (rv > cv) return true;
      if (rv < cv) return false;
    }
    return false;
  }

  // ★ 下载安装包：流式下载 + 断点续传 + 5 次重试（3s/10s/30s/60s 退避）。
  //   返回沙箱最终路径；进度回调 0~100
  async downloadHap(ctx: common.UIAbilityContext, info: ReleaseInfo,
    onProgress: (pct: number) => void): Promise<string> {
    if (info.assetUrl.length === 0) {
      throw new Error('该版本未提供安装包，请到发布页下载');
    }
    this.canceled = false;
    const finalPath = `${ctx.cacheDir}/update_${info.tag}.hap`;
    const partPath = `${ctx.cacheDir}/update_${info.tag}.hap.part`;
    const backoffs: number[] = [0, 3000, 10000, 30000, 60000];
    let lastErr: BusinessError | null = null;
    for (let attempt = 0; attempt < backoffs.length; attempt++) {
      if (backoffs[attempt] > 0) await sleepMs(backoffs[attempt]);
      if (this.canceled) throw new Error('已取消下载');
      try {
        await this.downloadOnce(info, partPath, onProgress);
        // 完整性校验：资产大小已知时必须精确匹配，不匹配视为损坏重来
        if (info.assetSize > 0 && fs.statSync(partPath).size !== info.assetSize) {
          try { fs.unlinkSync(partPath); } catch (_) {}
          throw new Error('安装包校验失败');
        }
        // part → 最终文件（先清旧）
        try { if (fs.accessSync(finalPath)) fs.unlinkSync(finalPath); } catch (_) {}
        fs.renameSync(partPath, finalPath);
        onProgress(100);
        return finalPath;
      } catch (e) {
        lastErr = e as BusinessError;
        // 部分文件保留（下次重试 Range 续传）；只有"校验失败"内部已清
      }
    }
    throw lastErr !== null ? lastErr : new Error('下载失败，请检查网络');
  }

  // ★ 单次下载：Range 续传（有 part 时）→ requestInStream 流式追加 →
  //   事件驱动写文件 + 进度回调；HTTP ≥400 抛错
  private async downloadOnce(info: ReleaseInfo, partPath: string,
    onProgress: (pct: number) => void): Promise<void> {
    let have = 0;
    try {
      if (fs.accessSync(partPath)) have = fs.statSync(partPath).size;
    } catch (_) {
      have = 0;
    }
    // 已下完整（上次运行就完成了）：直接返回走校验
    if (info.assetSize > 0 && have === info.assetSize) {
      onProgress(100);
      return;
    }
    const client = http.createHttp();
    // 文件句柄：续传追加 / 全新截断
    let file: fs.File | null = null;
    try {
      const header: Record<string, string> = { 'User-Agent': 'HmFish-Updater' };
      let resume = false;
      if (have > 0 && (info.assetSize <= 0 || have < info.assetSize)) {
        header['Range'] = `bytes=${have}-`;
        resume = true;
      }
      file = fs.openSync(partPath, resume
        ? fs.OpenMode.READ_WRITE | fs.OpenMode.APPEND
        : fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      const fd = file.fd;
      await new Promise<void>((resolve, reject) => {
        let done = false;
        let ended = false;
        client.on('dataReceive', (chunk: ArrayBuffer): void => {
          if (done) return;
          try {
            fs.writeSync(fd, chunk);
          } catch (_) {}
        });
        client.on('dataEnd', (): void => {
          ended = true;
          if (!done) {
            done = true;
            resolve();
          }
        });
        client.on('dataReceiveProgress', (p: http.DataReceiveProgressInfo): void => {
          if (info.assetSize > 0 && !done) {
            // 续传时 receiveSize 为本次响应已收字节，进度 = (have+received)/total
            const cur = have + p.receiveSize;
            onProgress(Math.max(0, Math.min(100, Math.round(cur * 100 / info.assetSize))));
          }
        });
        client.requestInStream(info.assetUrl, {
          method: http.RequestMethod.GET,
          connectTimeout: 10000,
          readTimeout: 120000,
          header: header
        }).then((code: number): void => {
          if (code >= 400) {
            if (!done) {
              done = true;
              reject(new Error(`HTTP ${code}`));
            }
            return;
          }
          // 兼容两种实现时序：promise 在 dataEnd 前或后完成均可
          if (ended && !done) {
            done = true;
            resolve();
          }
        }).catch((e: BusinessError): void => {
          if (!done) {
            done = true;
            reject(e);
          }
        });
      });
    } finally {
      if (file !== null) {
        try { fs.closeSync(file); } catch (_) {}
      }
      client.destroy();
    }
    // 续传被服务器忽略（返回 200 全量）时文件会超长，交给上层校验清理
  }

  // ★ 保存到公共目录（"下载"等）：系统保存选择器 → fd 分块拷贝；
  //   用户取消返回 false
  async saveToPublicDir(ctx: common.UIAbilityContext, srcPath: string,
    fileName: string): Promise<boolean> {
    try {
      if (!fs.accessSync(srcPath)) return false;
      const docPicker = new picker.DocumentViewPicker(ctx);
      const saveOptions = new picker.DocumentSaveOptions();
      saveOptions.newFileNames = [fileName];
      const uris = await docPicker.save(saveOptions);
      if (uris === undefined || uris.length === 0) return false;
      const dstFile = fs.openSync(uris[0], fs.OpenMode.READ_WRITE);
      const srcFile = fs.openSync(srcPath, fs.OpenMode.READ_ONLY);
      try {
        const buf = new ArrayBuffer(1024 * 1024);
        let read = fs.readSync(srcFile.fd, buf);
        while (read > 0) {
          fs.writeSync(dstFile.fd, buf, { length: read });
          read = fs.readSync(srcFile.fd, buf);
        }
      } finally {
        fs.closeSync(srcFile);
        fs.closeSync(dstFile);
      }
      return true;
    } catch (_) {
      return false;
    }
  }
}
