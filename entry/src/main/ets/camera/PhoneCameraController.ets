// ★ 手机相机轻量控制器（早期/简化版，正式链路见 CameraService.ets）：
//   管理前后摄选择打开与预览帧回调挂接，帧数据经回调送 YOLO/OCR 推理。
//   Index.ets 通过 frameCallback 接帧；与 native（libentry.so）无直接调用
// entry/src/main/ets/camera/PhoneCameraController.ets
import camera from '@ohos.multimedia.camera'
import { BusinessError } from '@ohos.base'

// ★ PhoneCameraController：单相机生命周期（open→close）+ 帧回调转发
export class PhoneCameraController {
  private cameraManager: camera.CameraManager | null = null
  private cameraInput: camera.CameraInput | null = null
  private previewOutput: camera.PreviewOutput | null = null
  private session: camera.CaptureSession | null = null
  // ★ 预览帧回调：ArkTS 侧接收帧后转交推理管线的唯一出口
  private frameCallback: ((frameData: ArrayBuffer, width: number, height: number) => void) | null = null

  // ★ open：按前后置枚举设备并打开输入（找不到目标摄像头直接抛错）
  async open(isFront: boolean): Promise<void> {
    this.cameraManager = camera.getCameraManager(getContext(this))
    const cameras = this.cameraManager.getSupportedCameras()
    const target = cameras.find(c =>
    isFront ? c.cameraPosition === camera.CameraPosition.CAMERA_POSITION_FRONT
      : c.cameraPosition === camera.CameraPosition.CAMERA_POSITION_BACK
    )
    if (!target) throw new Error('camera not found')

    this.cameraInput = this.cameraManager.createCameraInput(target)
    await this.cameraInput.open()

    // 创建预览输出（绑定到 XComponent 的 surfaceId）
    // 创建 CaptureSession，添加 input + previewOutput
    // session.begin()
  }

  /**
   * 获取预览帧数据（用于 YOLO / OCR 推理）
   * Camera Kit 通过 ImageReceiver 或 PreviewOutput 的帧回调获取
   */
  // ★ setFrameCallback：挂接帧回调（推理层注册，open 后由预览输出持续触发）
  setFrameCallback(cb: (frameData: ArrayBuffer, width: number, height: number) => void) {
    this.frameCallback = cb
  }

  // ★ close：释放会话与输入（可选链保证未 open 时调用也不崩）
  async close(): Promise<void> {
    await this.session?.release()
    await this.cameraInput?.close()
  }
}