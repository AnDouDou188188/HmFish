// entry/src/main/ets/camera/PhoneCameraController.ets
import camera from '@ohos.multimedia.camera'
import { BusinessError } from '@ohos.base'

export class PhoneCameraController {
  private cameraManager: camera.CameraManager | null = null
  private cameraInput: camera.CameraInput | null = null
  private previewOutput: camera.PreviewOutput | null = null
  private session: camera.CaptureSession | null = null
  private frameCallback: ((frameData: ArrayBuffer, width: number, height: number) => void) | null = null

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
  setFrameCallback(cb: (frameData: ArrayBuffer, width: number, height: number) => void) {
    this.frameCallback = cb
  }

  async close(): Promise<void> {
    await this.session?.release()
    await this.cameraInput?.close()
  }
}