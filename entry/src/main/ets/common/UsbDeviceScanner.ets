// ★ UVC 设备枚举（ArkTS usbManager 双通道之 USB 总线通道）：
//   当 USB 摄像头未被系统相机服务枚举（拿不到 CameraDevice）时，
//   走 USB 总线层直接发现 UVC 设备——等价 Android UsbManager.getDeviceList
//   + default_device_filter.xml 过滤，含 requestRight 授权弹窗与插拔广播订阅。
//   发现的设备名与 usbfs 文件描述符(fd)交给 native libuvc（libentry.so 内）
//   做 fd 注入直开摄像头，绕开系统相机服务对部分 UVC 设备的兼容性问题
import { usbManager, commonEventManager } from '@kit.BasicServicesKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ 照抄 Android libausbc/utils/CameraUtils.kt 的 USB 类别常量
const USB_CLASS_AUDIO = 1;      // UsbConstants.USB_CLASS_AUDIO
const USB_CLASS_VIDEO = 14;     // UsbConstants.USB_CLASS_VIDEO (UVC)
const USB_CLASS_MISC = 239;     // UsbConstants.USB_CLASS_MISC (0xEF)

// ★ 照抄 Android libausbc/res/xml/default_device_filter.xml 的类别组合
//   （Android 9.0 及部分特殊设备走这些类别；239/2 即 UVC IAD 组合）
const FILTER_CLASS_SUBCLASS: number[][] = [
  [239, 2],   // <usb-device class="239" subclass="2" /> all device of UVC
  [14, 9],    // <usb-device class="14" subclass="9" /> a few android 9.0
  [2, 0],     // <usb-device class="2" subclass="0" />
  [39, 0]     // <usb-device class="39" subclass="0" />
];

// ★ 照抄 default_device_filter.xml 的 VID/PID 白名单（vendorId_productId）
const FILTER_VID_PID: string[] = [
  '9573_4836', '1133_2229', '1409_640', '9228_258', '19_16981', '1409_5120'
];

// ★ 扫描出的 UVC 相机信息：busNum/devAddress 定位物理端口，
//   hasMic 决定录像音源选择，hasRight 表示已授权（可免弹窗直连）
export interface UsbCamInfo {
  name: string;
  productName: string;
  manufacturerName: string;
  vendorId: number;
  productId: number;
  busNum: number;
  devAddress: number;
  hasMic: boolean;
  hasRight: boolean;
}

// ★ connectDevice 返回的 pipe + getFileDescriptor 得到的 fd
//   （等价 Android UsbDeviceConnection.getFileDescriptor()）
export interface UsbPipeHandle {
  pipe: usbManager.USBDevicePipe;
  fd: number;
}

// ───────── 静态工具：识别 / 扫描 / 授权 / 打开管道 ─────────
// ★ UsbDeviceScanner：静态工具（无状态）+ 实例订阅（插拔广播）两部分
export class UsbDeviceScanner {
  // ★ 照抄 Android CameraUtils.isUsbCamera + isFilterDevice(default_device_filter.xml)：
  //   1. 设备类 = VIDEO 直接是相机；MISC(0xEF)/0 则遍历接口找 VIDEO 类
  //   2. 命中 default_device_filter.xml 的 class/subclass 组合或 VID/PID 白名单也算
  static isUsbCamera(dev: usbManager.USBDevice): boolean {
    if (dev.clazz === USB_CLASS_VIDEO) return true;
    // ★ isFilterDevice：default_device_filter.xml 的 class/subclass 组合
    for (let i = 0; i < FILTER_CLASS_SUBCLASS.length; i++) {
      const cs = FILTER_CLASS_SUBCLASS[i];
      if (dev.clazz === cs[0] && dev.subClass === cs[1]) return true;
    }
    // ★ isFilterDevice：default_device_filter.xml 的 VID/PID 白名单
    if (FILTER_VID_PID.indexOf(`${dev.vendorId}_${dev.productId}`) >= 0) return true;
    if (dev.clazz === USB_CLASS_MISC || dev.clazz === 0) {
      for (let c = 0; c < dev.configs.length; c++) {
        const ifaces = dev.configs[c].interfaces;
        for (let i = 0; i < ifaces.length; i++) {
          if (ifaces[i].clazz === USB_CLASS_VIDEO) return true;
        }
      }
    }
    return false;
  }

  // ★ 照抄 Android CameraUtils.isCameraContainsMic：接口里有 AUDIO 类即含麦克风
  static containsMic(dev: usbManager.USBDevice): boolean {
    for (let c = 0; c < dev.configs.length; c++) {
      const ifaces = dev.configs[c].interfaces;
      for (let i = 0; i < ifaces.length; i++) {
        if (ifaces[i].clazz === USB_CLASS_AUDIO) return true;
      }
    }
    return false;
  }

  // ★ 等价 Android UsbManager.getDeviceList() + 过滤 UVC 设备
  static scan(): UsbCamInfo[] {
    const out: UsbCamInfo[] = [];
    try {
      const devices: Array<usbManager.USBDevice> = usbManager.getDevices();
      for (let i = 0; i < devices.length; i++) {
        const d = devices[i];
        if (!UsbDeviceScanner.isUsbCamera(d)) continue;
        let hasRight = false;
        try { hasRight = usbManager.hasRight(d.name); } catch (_) {}
        const info: UsbCamInfo = {
          name: d.name,
          productName: d.productName,
          manufacturerName: d.manufacturerName,
          vendorId: d.vendorId,
          productId: d.productId,
          busNum: d.busNum,
          devAddress: d.devAddress,
          hasMic: UsbDeviceScanner.containsMic(d),
          hasRight: hasRight
        };
        out.push(info);
      }
    } catch (e) {
      console.warn('[UsbDeviceScanner] getDevices failed', e);
    }
    return out;
  }

  // ★ 等价 Android UsbManager.requestPermission()：调起系统弹窗申请 USB 设备访问权限
  static async requestRight(deviceName: string): Promise<boolean> {
    try {
      return await usbManager.requestRight(deviceName);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[UsbDeviceScanner] requestRight failed: ${err.message}`);
      return false;
    }
  }

  // ★ 等价 Android UsbManager.openDevice + UsbDeviceConnection.getFileDescriptor：
  //   connectDevice 拿 pipe，getFileDescriptor 把 usbfs fd dup 进本进程，
  //   该 fd 交给 native libuvc 做 fd 注入（等价 UVCCamera.connect）
  static openPipe(deviceName: string): UsbPipeHandle | null {
    try {
      const devices: Array<usbManager.USBDevice> = usbManager.getDevices();
      for (let i = 0; i < devices.length; i++) {
        if (devices[i].name === deviceName) {
          const pipe: usbManager.USBDevicePipe = usbManager.connectDevice(devices[i]);
          const fd: number = usbManager.getFileDescriptor(pipe);
          if (fd <= 0) {
            console.error(`[UsbDeviceScanner] getFileDescriptor failed: ${fd}`);
            return null;
          }
          const handle: UsbPipeHandle = { pipe: pipe, fd: fd };
          return handle;
        }
      }
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[UsbDeviceScanner] openPipe failed: ${err.message}`);
    }
    return null;
  }

  // ★ 插拔广播订阅者句柄：unsubscribe 后置空，避免重复取消订阅报错
  private subscriber: commonEventManager.CommonEventSubscriber | null = null;

  // ───────── 实例方法：USB 插拔广播订阅 ─────────

  // ★ 等价 Android USB_DEVICE_ATTACHED / USB_DEVICE_DETACHED 广播：
  //   订阅系统公共事件监听 USB 设备插拔
  subscribe(onAttach: () => void, onDetach: () => void): void {
    const info: commonEventManager.CommonEventSubscribeInfo = {
      events: [
        'usual.event.hardware.usb.action.USB_DEVICE_ATTACHED',
        'usual.event.hardware.usb.action.USB_DEVICE_DETACHED'
      ]
    };
    commonEventManager.createSubscriber(info,
      (err: BusinessError, sub: commonEventManager.CommonEventSubscriber) => {
        if (err) {
          console.error(`[UsbDeviceScanner] createSubscriber failed: ${err.message}`);
          return;
        }
        this.subscriber = sub;
        commonEventManager.subscribe(sub,
          (err2: BusinessError, data: commonEventManager.CommonEventData) => {
            if (err2) return;
            if (data.event === 'usual.event.hardware.usb.action.USB_DEVICE_ATTACHED') {
              onAttach();
            } else if (data.event === 'usual.event.hardware.usb.action.USB_DEVICE_DETACHED') {
              onDetach();
            }
          });
      });
  }

  // ★ 退订插拔广播：页面卸载/Ability 销毁时调用，防回调打到已释放的 UI
  unsubscribe(): void {
    if (this.subscriber) {
      try { commonEventManager.unsubscribe(this.subscriber); } catch (_) {}
      this.subscriber = null;
    }
  }
}
