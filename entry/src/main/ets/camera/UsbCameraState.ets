// ★ 相机状态监听 + 权限申请：CameraManager 状态回调判定 USB 相机插拔
//   （APPEAR/DISAPPEAR/AVAILABLE），麦克风权限按需补申请
import { camera } from '@kit.CameraKit';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

export type CameraSource = 'BUILT_IN' | 'USB';

export interface CameraStatusEvent {
  source: CameraSource;
  action: 'APPEAR' | 'DISAPPEAR' | 'AVAILABLE';
  cameraId: string;
}

export class UsbCameraState {
  private cameraManager: camera.CameraManager | null = null;
  private onUsbStatusChange?: (event: CameraStatusEvent) => void;
  private micGranted: boolean = false;

  private statusCallback = (err: BusinessError, info: camera.CameraStatusInfo): void => {
    if (err && err.code !== 0) return;
    if (!info || !info.camera) return;
    const isUsb = info.camera.connectionType === camera.ConnectionType.CAMERA_CONNECTION_USB_PLUGIN;
    if (!isUsb) return;

    let action: 'APPEAR' | 'DISAPPEAR' | 'AVAILABLE' = 'AVAILABLE';
    if (info.status === camera.CameraStatus.CAMERA_STATUS_APPEAR) action = 'APPEAR';
    else if (info.status === camera.CameraStatus.CAMERA_STATUS_DISAPPEAR) action = 'DISAPPEAR';

    const ev: CameraStatusEvent = { source: 'USB', action, cameraId: info.camera.cameraId };
    if (this.onUsbStatusChange) this.onUsbStatusChange(ev);
  };

  async init(context: common.UIAbilityContext): Promise<boolean> {
    const granted = await this.requestPermissions(context);
    if (!granted) return false;
    this.cameraManager = camera.getCameraManager(context);
    this.cameraManager.on('cameraStatus', this.statusCallback);
    return true;
  }

  private async requestPermissions(context: common.UIAbilityContext): Promise<boolean> {
    const at = abilityAccessCtrl.createAtManager();
    const perms: Permissions[] = [
      'ohos.permission.CAMERA',
      'ohos.permission.MICROPHONE'
    ];
    try {
      const res = await at.requestPermissionsFromUser(context, perms);
      const cameraGranted = res.authResults.length > 0 && res.authResults[0] === 0;
      this.micGranted = res.authResults.length > 1 && res.authResults[1] === 0;
      console.info(`[UsbCameraState] camera=${cameraGranted}, mic=${this.micGranted}`);
      return cameraGranted;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[UsbCameraState] requestPermission failed: ${err.message}`);
      return false;
    }
  }

  isMicGranted(): boolean { return this.micGranted; }

  getUsbCamera(): camera.CameraDevice | null {
    if (!this.cameraManager) return null;
    const list = this.cameraManager.getSupportedCameras();
    for (let i = 0; i < list.length; i++) {
      if (list[i].connectionType === camera.ConnectionType.CAMERA_CONNECTION_USB_PLUGIN) {
        return list[i];
      }
    }
    return null;
  }

  getBuiltInCamera(position: number): camera.CameraDevice | null {
    if (!this.cameraManager) return null;
    const list = this.cameraManager.getSupportedCameras();
    // position: 0=后, 1=前, 2=USB(回退到后置)
    const pos = position === 1
      ? camera.CameraPosition.CAMERA_POSITION_FRONT
      : camera.CameraPosition.CAMERA_POSITION_BACK;

    for (let i = 0; i < list.length; i++) {
      if (list[i].cameraPosition === pos &&
        list[i].connectionType !== camera.ConnectionType.CAMERA_CONNECTION_USB_PLUGIN) {
        return list[i];
      }
    }
    for (let i = 0; i < list.length; i++) {
      if (list[i].connectionType !== camera.ConnectionType.CAMERA_CONNECTION_USB_PLUGIN) {
        return list[i];
      }
    }
    return null;
  }

  setOnUsbStatusChange(cb: (event: CameraStatusEvent) => void): void {
    this.onUsbStatusChange = cb;
  }

  release(): void {
    if (this.cameraManager) {
      try { this.cameraManager.off('cameraStatus'); } catch (_) {}
      this.cameraManager = null;
    }
  }
}