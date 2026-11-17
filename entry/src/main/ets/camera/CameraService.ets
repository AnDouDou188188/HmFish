// ★ 手机内置相机会话管理：预览档位选择、变焦范围查询、USB/内置双源路由，
//   渲染侧据此决定旋转/镜像与预览尺寸对齐
import { camera } from '@kit.CameraKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { UsbCameraState, CameraSource } from './UsbCameraState';

export class CameraService {
  private cameraManager: camera.CameraManager | null = null;
  private captureSession: camera.CaptureSession | null = null;
  private zoomRange: [number, number] = [1.0, 1.0];
  private usbState: UsbCameraState = new UsbCameraState();
  // ★ 当前会话实际选中的相机设备信息（供渲染侧决定旋转/镜像）
  private activeOrientation: number = 0;
  private activePosition: camera.CameraPosition = camera.CameraPosition.CAMERA_POSITION_BACK;
  // ★ 当前会话实际选中的预览档位尺寸：init 完成后 Index 用它对齐 native 管线
  private activePreviewSize: [number, number] = [640, 480];
  // ★ 当前会话选中摄像头的型号描述（如"手机后置主摄"）：供变焦气泡展示
  private activeLabel: string = '手机后置主摄';

  async init(context: common.UIAbilityContext,
    displaySurfaceId: string,
    secondSurfaceId: string,
    preferredPosition: number,
    mode: string): Promise<CameraSource> {
    const ok = await this.usbState.init(context);
    if (!ok) throw new Error('camera permission denied');

    this.cameraManager = camera.getCameraManager(context);

    let target: camera.CameraDevice | null = null;
    let source: CameraSource = 'BUILT_IN';

    if (preferredPosition === 2) {
      target = this.usbState.getUsbCamera();
      if (target) source = 'USB';
    }
    if (!target) {
      target = this.usbState.getBuiltInCamera(preferredPosition);
      source = 'BUILT_IN';
    }
    if (!target) throw new Error('no camera available');

    // ★ 记录实际选中设备：cameraOrientation 供渲染旋转、cameraPosition 供前摄镜像判断
    this.activeOrientation = target.cameraOrientation;
    this.activePosition = target.cameraPosition;
    this.activeLabel = this.describeDevice(target);
    console.info(`[CameraService] target cameraId=${target.cameraId} ` +
      `orientation=${target.cameraOrientation} position=${target.cameraPosition} ` +
      `connection=${target.connectionType} label=${this.activeLabel}`);

    await this.configureSession(target, displaySurfaceId, secondSurfaceId, mode);
    return source;
  }

  async switchTo(context: common.UIAbilityContext,
    displaySurfaceId: string,
    secondSurfaceId: string,
    preferredPosition: number,
    mode: string): Promise<CameraSource> {
    await this.release();
    this.cameraManager = camera.getCameraManager(context);
    return this.init(context, displaySurfaceId, secondSurfaceId, preferredPosition, mode);
  }

  private async configureSession(device: camera.CameraDevice,
    displaySurfaceId: string,
    secondSurfaceId: string,
    mode: string): Promise<void> {
    const mgr = this.cameraManager;
    if (!mgr) throw new Error('cameraManager null');

    const input = mgr.createCameraInput(device);
    await input.open();

    const cap = mgr.getSupportedOutputCapability(device, camera.SceneMode.NORMAL_PHOTO);
    const displayProfile = this.pickPreviewProfile(cap.previewProfiles);
    console.info(`[CameraService] displayProfile ${displayProfile.size.width}x${displayProfile.size.height} ` +
      `fmt=${displayProfile.format}`);
    // ★ 记录实际选中的预览档位：init 完成后 Index 用它对齐 native 管线处理尺寸
    this.activePreviewSize = [displayProfile.size.width, displayProfile.size.height];
    const displayOutput = mgr.createPreviewOutput(displayProfile, displaySurfaceId);

    let secondOutput: camera.CameraOutput;
    if (mode === 'ai') {
      const aiProfile = this.pickPreviewProfile(cap.previewProfiles);
      secondOutput = mgr.createPreviewOutput(aiProfile, secondSurfaceId);
    } else {
      const videoProfile = this.pickVideoProfile(cap.videoProfiles, displayProfile);
      console.info(`[CameraService] videoProfile ${videoProfile.size.width}x${videoProfile.size.height} ` +
        `fps=${videoProfile.frameRateRange.min}-${videoProfile.frameRateRange.max}`);
      secondOutput = mgr.createVideoOutput(videoProfile, secondSurfaceId);
    }

    this.captureSession = mgr.createCaptureSession();
    this.captureSession.beginConfig();
    this.captureSession.addInput(input);
    this.captureSession.addOutput(displayOutput);
    this.captureSession.addOutput(secondOutput);
    await this.captureSession.commitConfig();
    await this.captureSession.start();

    try {
      const ps = this.captureSession as Object as camera.PhotoSession;
      const range = ps.getZoomRatioRange();
      if (range && range.length >= 2) this.zoomRange = [range[0], range[1]];
    } catch (_) {}
  }

  // ★ 分辨率策略：原则上采用摄像头支持的最高分辨率（不再默认 640x480）；
  //   上限 1920x1080，防止 4K 帧逐帧 RGBA 转换/叠加/录像推流过重；
  //   面积并列时取更宽的（1920x1080 优于 1440x1440：16:9 适配屏幕与标准视频尺寸）
  private pickPreviewProfile(profiles: camera.Profile[]): camera.Profile {
    const CAP = 1920 * 1080;
    let best: camera.Profile | null = null;
    let bestArea = -1;
    for (let i = 0; i < profiles.length; i++) {
      const p = profiles[i];
      const area = p.size.width * p.size.height;
      if (area > CAP) continue;
      const better = best === null || area > bestArea
        || (area === bestArea && p.size.width > best.size.width);
      if (better) {
        best = p;
        bestArea = area;
      }
    }
    return best !== null ? best : profiles[0];
  }

  private pickVideoProfile(profiles: camera.VideoProfile[], fallback: camera.Profile): camera.VideoProfile {
    if (profiles && profiles.length > 0) {
      const CAP = 1920 * 1080;
      let best: camera.VideoProfile | null = null;
      let bestArea = -1;
      for (let i = 0; i < profiles.length; i++) {
        const p = profiles[i];
        const area = p.size.width * p.size.height;
        if (area > CAP) continue;
        const better = best === null || area > bestArea
          || (area === bestArea && p.size.width > best.size.width);
        if (better) {
          best = p;
          bestArea = area;
        }
      }
      if (best !== null) return best;
      return profiles[0];
    }
    return {
      size: fallback.size,
      format: fallback.format,
      frameRateRange: { min: 30, max: 30 }
    };
  }

  setZoomRatio(ratio: number): void {
    if (!this.captureSession) { console.warn('[pinchZoom] no captureSession'); return; }
    try {
      const ps = this.captureSession as Object as camera.PhotoSession;
      ps.setZoomRatio(ratio);
    } catch (e) {
      console.error(`[pinchZoom] setZoomRatio err: ${JSON.stringify(e)}`);
    }
  }

  getZoomRange(): [number, number] { return this.zoomRange; }
  getUsbState(): UsbCameraState { return this.usbState; }

  // ★ 摄像头型号描述：SDK CameraDevice 无营销型号字段（hostDeviceName 仅远程设备有值），
  //   用系统枚举拼描述：位置（前置/后置）+ 类型（主摄/广角/超广角/长焦/深感）
  private describeDevice(d: camera.CameraDevice): string {
    if (d.connectionType === camera.ConnectionType.CAMERA_CONNECTION_USB_PLUGIN) return 'USB 摄像头';
    let pos = '手机';
    if (d.cameraPosition === camera.CameraPosition.CAMERA_POSITION_FRONT) pos = '手机前置';
    else if (d.cameraPosition === camera.CameraPosition.CAMERA_POSITION_BACK) pos = '手机后置';
    let type = '主摄';
    if (d.cameraType === camera.CameraType.CAMERA_TYPE_WIDE_ANGLE) type = '广角';
    else if (d.cameraType === camera.CameraType.CAMERA_TYPE_ULTRA_WIDE) type = '超广角';
    else if (d.cameraType === camera.CameraType.CAMERA_TYPE_TELEPHOTO) type = '长焦';
    else if (d.cameraType === camera.CameraType.CAMERA_TYPE_TRUE_DEPTH) type = '深感';
    return pos + type;
  }
  getActiveLabel(): string { return this.activeLabel; }

  // ★ 当前会话选中设备的传感器安装角度（CameraDevice.cameraOrientation）
  getActiveOrientation(): number { return this.activeOrientation; }
  // ★ 当前会话实际选中的预览档位尺寸（宽x高，相机坐标系）
  getActivePreviewSize(): [number, number] { return this.activePreviewSize; }
  // ★ 当前会话选中设备的位置（FRONT=1 需要镜像渲染）
  getActivePosition(): camera.CameraPosition { return this.activePosition; }
  // ★ 是否前置摄像头（渲染需要水平镜像，对齐 Android 前摄自拍镜像预览）
  isFrontCamera(): boolean {
    return this.activePosition === camera.CameraPosition.CAMERA_POSITION_FRONT;
  }

  async release(): Promise<void> {
    if (this.captureSession) {
      try { await this.captureSession.stop(); } catch (_) {}
      try { await this.captureSession.release(); } catch (_) {}
      this.captureSession = null;
    }
  }

  releaseAll(): void {
    this.release();
    this.usbState.release();
  }
}