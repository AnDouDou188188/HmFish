// ★ 拍照/录像产物管理：拍照（native 抓当前合成帧→PNG→图库）与录像产物
//   （RecorderService 的 MP4）统一以 Fish_年月日_时分秒 命名保存到系统图库。
//   与 native（libentry.so）交互：entry.captureLatestFrame/getFrameSize 取
//   渲染合成帧；保存链路纯 ArkTS（photoAccessHelper 直接入库），
//   权限被拒时回退系统保存弹窗——无人值守自动录像与手动保存都能落库
import { RecorderService } from '../camera/RecorderService';
import { common } from '@kit.AbilityKit';
import { photoAccessHelper } from '@kit.MediaLibraryKit';
import { fileIo as fs, fileUri } from '@kit.CoreFileKit';
import { saveRgbaWithContext } from '../common/Utils';
import entry from 'libentry.so';

/** 保存结果原因分类 */
export type SaveReason = 'success' | 'canceled' | 'timeout' | 'not_exist' | 'unknown';

/** 保存到相册的结果 */
export interface SaveResult {
  success: boolean;
  reason: SaveReason;
  errorCode?: number;
}

// ★ MediaController：媒体产物出口（截图/入库），聚合 RecorderService 录像能力
export class MediaController {
  // ★ 录像服务实例：Index 通过它启停录像、取产物路径
  public recorder: RecorderService = new RecorderService();

  // ★ 专用命名：Fish_年月日_时分秒（截图/录像统一前缀；
  //   鸿蒙第三方应用无法在媒体库自建目录，用 Fish_日期 命名让图库按日分组可辨识）
  private buildFishTitle(): string {
    const d = new Date();
    const p2 = (v: number): string => v < 10 ? '0' + v.toString() : v.toString();
    return `Fish_${d.getFullYear()}${p2(d.getMonth() + 1)}${p2(d.getDate())}_${p2(d.getHours())}${p2(d.getMinutes())}${p2(d.getSeconds())}`;
  }

  // ★ snapshot：拍照 = native 取最新合成帧 → 沙箱 PNG → 存图库；
  //   任一环节失败返回 false（UI 提示"拍照失败"）
  async snapshot(ctx: common.UIAbilityContext): Promise<boolean> {
    try {
      const buf = entry.captureLatestFrame();
      const size = entry.getFrameSize() as number[];
      if (!buf || buf.byteLength === 0 || size.length < 2) return false;

      const path = await saveRgbaWithContext(ctx, buf, size[0], size[1]);
      if (!path) return false;

      const result = await this.saveToGallery(ctx, path, false);
      return result.success;
    } catch {
      return false;
    }
  }

  /**
   * 保存文件到系统相册
   * ★ 照抄 Android MediaStore 直接入库：申请 WRITE_IMAGEVIDEO 后用
   *   MediaAssetChangeRequest 入库，无弹窗、自动录像无人值守也能保存；
   *   权限被拒时回退系统保存弹窗（SaveUIExtensionAbility）。
   */
  async saveToGallery(ctx: common.UIAbilityContext, srcPath: string, isVideo: boolean): Promise<SaveResult> {
    try {
      // 源文件不存在
      if (!fs.accessSync(srcPath)) {
        console.error(`[MediaController] file not exist: ${srcPath}`);
        return { success: false, reason: 'not_exist' };
      }

      // ① 直接入库（无弹窗，需 ohos.permission.WRITE_IMAGEVIDEO）
      try {
        const srcUri = fileUri.getUriFromPath(srcPath);
        const helper = photoAccessHelper.getPhotoAccessHelper(ctx);
        const req = isVideo
          ? photoAccessHelper.MediaAssetChangeRequest.createVideoAssetRequest(ctx, srcUri)
          : photoAccessHelper.MediaAssetChangeRequest.createImageAssetRequest(ctx, srcUri);
        await helper.applyChanges(req);
        console.info(`[MediaController] direct save ok: ${srcPath}`);
        return { success: true, reason: 'success' };
      } catch (e1) {
        const err1 = e1 as BusinessError;
        console.warn(`[MediaController] direct save failed: ${err1.code} ${err1.message}, fallback to dialog`);
      }

      // ② 回退：系统保存确认弹窗（无需权限，需用户点击确认）
      // 14000011: Internal system error —— 本场景几乎都由保存弹窗
      //   SaveUIExtensionAbility 冻结/加载超时导致（日志伴随 100015 extension_lifecycle_timeout），
      //   等 1.5s 让相册进程解冻后重试一次
      const helper = photoAccessHelper.getPhotoAccessHelper(ctx);
      let desFileUris: Array<string> = [];
      let dialogErr: BusinessError | null = null;
      for (let attempt = 0; attempt < 2; attempt++) {
        try {
          // ★ 根因修复：SDK 要求 URI 必须由 API 生成（不能手工拼 'file://'+path）。
          //   手工拼的 URI 无应用授权 authority → 系统无法转 fd → 保存扩展收到
          //   空 fds 报 "uris invalid" → 14000011 → toast "保存失败：timeout"。
          //   getUriFromPath 生成 file://com.fish.ayf/... 合法 URI
          const srcUri = fileUri.getUriFromPath(srcPath).toString();
          desFileUris = await helper.showAssetsCreationDialog(
            [srcUri],
            [{
              title: this.buildFishTitle(),
              fileNameExtension: isVideo ? 'mp4' : 'jpg',
              photoType: isVideo ? photoAccessHelper.PhotoType.VIDEO : photoAccessHelper.PhotoType.IMAGE,
              subtype: photoAccessHelper.PhotoSubtype.DEFAULT,
            }]
          );
          dialogErr = null;
          break;
        } catch (e2) {
          const err2 = e2 as BusinessError;
          console.error(`[MediaController] save dialog attempt ${attempt} failed: ${err2.code} ${err2.message}`);
          if (err2.code === 14000011 && attempt === 0) {
            await new Promise<void>((resolve: () => void) => { setTimeout(resolve, 1500); });
            continue;
          }
          dialogErr = err2;
          break;
        }
      }
      if (dialogErr !== null) {
        if (dialogErr.code === 14000011) {
          return { success: false, reason: 'timeout', errorCode: dialogErr.code };
        }
        return { success: false, reason: 'unknown', errorCode: dialogErr.code };
      }

      // 用户确认保存
      if (desFileUris && desFileUris.length > 0) {
        const destUri = desFileUris[0];
        const srcFile = fs.openSync(srcPath, fs.OpenMode.READ_ONLY);
        const destFile = fs.openSync(destUri, fs.OpenMode.READ_WRITE);
        fs.copyFileSync(srcFile.fd, destFile.fd);
        fs.closeSync(srcFile);
        fs.closeSync(destFile);
        return { success: true, reason: 'success' };
      }

      // 返回空数组：用户主动取消
      console.warn(`[MediaController] save to gallery canceled by user`);
      return { success: false, reason: 'canceled' };
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[MediaController] saveToGallery failed: ${err.code} ${err.message}`);
      return { success: false, reason: 'unknown', errorCode: err.code };
    }
  }

  // ★ 当前/最近一段录像产物路径透传（自动录像完成后存相册用）
  getRecorderPath(): string {
    return this.recorder.getVideoPath();
  }
}