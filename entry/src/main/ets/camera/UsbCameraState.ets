// ★ 相机状态监听 + 权限申请（USB/UVC 发现双通道之"相机服务"通道）：
//   CameraManager 的 cameraStatus 回调判定 USB 相机插拔
//   （APPEAR/DISAPPEAR/AVAILABLE）。USB 摄像头被系统相机服务枚举到时
//   走本文件的 CameraDevice 直连；否则回退 UsbDeviceScanner 的
//   libuvc（USB 总线）通道。相机/麦克风权限在 init 时一次性申请
import { camera } from '@kit.CameraKit';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ 当前画面来源：内置摄像头 or USB 外接（Index 据此走不同渲染/控制逻辑）
export type CameraSource = 'BUILT_IN' | 'USB';

// ★ USB 相机状态事件：插上/拔出/可用，Index 收到后刷新 UI 或自动重连
export interface CameraStatusEvent {
  source: CameraSource;
  action: 'APPEAR' | 'DISAPPEAR' | 'AVAILABLE';
  cameraId: string;
}

// ★ UsbCameraState：相机权限 + 设备枚举 + USB 插拔监听的聚合状态类
export class UsbCameraState {
  private cameraManager: camera.CameraManager | null = null;
  private onUsbStatusChange?: (event: CameraStatusEvent) => void;
  private micGranted: boolean = false;

  // ★ 系统相机状态回调：只关心 USB 外接设备，内置摄像头变化直接忽略
  //   （前后摄切换由业务主动 switchTo 触发，无需监听）
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

  // ★ init：先拿权限（被拒则整体失败，上层提示去设置开权限），再挂状态监听
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

  // ★ 枚举当前已接入并被系统识别的 USB 相机（无则返回 null，上层回退内置）
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

  // ★ 枚举内置摄像头：0=后置，1=前置，2（USB 回退场景）当后置处理；
  //   指定位置找不到时退而求其次返回任意非 USB 摄像头（保底有画面）
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

  // ★ release：解绑状态监听（防 Ability 销毁后回调打到已释放的页面）
  release(): void {
    if (this.cameraManager) {
      try { this.cameraManager.off('cameraStatus'); } catch (_) {}
      this.cameraManager = null;
    }
  }
}