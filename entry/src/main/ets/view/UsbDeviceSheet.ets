// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ UsbDeviceSheet（UsbDeviceSheet.ets）—— USB 设备选择半模态弹窗                  ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  枚举到的 UVC 外接摄像头列表，点选连接（对齐 Android 设备选择 Dialog）：       ║
// ║  每行 = 图标 + 设备名 + VID:PID + 厂商 + 含麦克风/授权状态徽标 + 连接按钮；    ║
// ║  空列表给"插入 OTG 摄像头"引导文案；右上角"刷新"重新扫描                     ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  devices 由 Index 经 UsbDeviceScanner 扫描下发（UsbCamInfo）；               ║
// ║  点行/连接 → onConnect 上抛（Index 先触发系统授权弹窗                         ║
// ║  usbManager.requestRight，通过后建链取流）；刷新 → onRescan；                 ║
// ║  关闭 → onClose                                                              ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ USB 设备选择弹窗：枚举到的 UVC 摄像头列表点选连接（对齐 Android 设备选择 Dialog）
import { UsbCamInfo } from '../common/UsbDeviceScanner';

// ★ 照抄 Android 多摄/设备选择 Dialog：列出枚举到的 UVC 设备，
//   点击触发系统授权弹窗（usbManager.requestRight）后连接
// ★ UsbDeviceSheet：USB 设备列表弹窗主体（标题/列表或空态/关闭）
@Component
export struct UsbDeviceSheet {
  // ★ 设备列表（Index 扫描下发）与交互回调（连接/重新扫描/关闭）
  @Prop devices: UsbCamInfo[] = [];
  onConnect?: (dev: UsbCamInfo) => void;
  onRescan?: () => void;
  onClose?: () => void;

  // ★ 数字格式化为 4 位大写十六进制（0xXXXX）：展示 USB VID/PID 用，不足位补零
  private hex4(n: number): string {
    let s = '';
    try { s = n.toString(16).toUpperCase(); } catch (_) { s = `${n}`; }
    while (s.length < 4) s = `0${s}`;
    return `0x${s}`;
  }

  // ★ 设备行：图标 + 设备名 + VID:PID/厂商 + 含麦克风/授权徽标 + 连接按钮，
  //   整行可点（点击即发起连接授权流程）
  @Builder
  DeviceRow(d: UsbCamInfo) {
    Row({ space: 10 }) {
      Text('📷')
        .fontSize(22)
        .width(44).height(44)
        .textAlign(TextAlign.Center)
        .backgroundColor('rgba(0,229,255,0.10)')
        .borderRadius(10)
      Column({ space: 3 }) {
        Text(d.productName && d.productName.length > 0 ? d.productName : d.name)
          .fontSize(14)
          .fontColor('#FFFFFF')
          .fontWeight(FontWeight.Bold)
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(`${this.hex4(d.vendorId)}:${this.hex4(d.productId)}  ${d.manufacturerName}`)
          .fontSize(11)
          .fontColor('#8E8E93')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
        Row({ space: 6 }) {
          if (d.hasMic) {
            Text('含麦克风')
              .fontSize(10).fontColor('#34C759')
              .padding({ left: 6, right: 6, top: 2, bottom: 2 })
              .backgroundColor('rgba(52,199,89,0.12)')
              .borderRadius(4)
          }
          Text(d.hasRight ? '已授权' : '未授权')
            .fontSize(10)
            .fontColor(d.hasRight ? '#34C759' : '#FF9500')
            .padding({ left: 6, right: 6, top: 2, bottom: 2 })
            .backgroundColor(d.hasRight ? 'rgba(52,199,89,0.12)' : 'rgba(255,149,10,0.12)')
            .borderRadius(4)
        }
      }
      .alignItems(HorizontalAlign.Start)
      .layoutWeight(1)

      Text('连接')
        .fontSize(13)
        .fontColor('#062A33')
        .fontWeight(FontWeight.Bold)
        .padding({ left: 16, right: 16, top: 8, bottom: 8 })
        .backgroundColor('#00E5FF')
        .borderRadius(14)
    }
    .width('100%')
    .padding(12)
    .backgroundColor('rgba(255,255,255,0.05)')
    .borderRadius(12)
    .margin({ bottom: 8 })
    .onClick(() => this.onConnect?.(d))
  }

  build() {
    Column() {
      // ── 标题行："USB 摄像头设备" + 右上角"刷新"按钮（重新扫描设备） ──
      // 拖拽条
      Row()
        .width(36).height(4).borderRadius(2)
        .backgroundColor('rgba(255,255,255,0.25)')
        .margin({ top: 10, bottom: 14 })

      Row() {
        Text('USB 摄像头设备')
          .fontSize(16)
          .fontColor('#FFFFFF')
          .fontWeight(FontWeight.Bold)
          .layoutWeight(1)
        Text('刷新')
          .fontSize(13)
          .fontColor('#00E5FF')
          .padding({ left: 10, right: 10, top: 6, bottom: 6 })
          .onClick(() => this.onRescan?.())
      }
      .width('100%')
      .margin({ bottom: 12 })

      // ── 空态：无设备时给"插入 OTG 摄像头后刷新"的引导文案 ──
      if (this.devices.length === 0) {
        Column({ space: 8 }) {
          Text('🤷')
            .fontSize(30)
          Text('未检测到 USB 视频设备\n请插入 OTG 摄像头后点右上角"刷新"')
            .fontSize(13)
            .fontColor('#8E8E93')
            .textAlign(TextAlign.Center)
            .lineHeight(20)
        }
        .width('100%')
        .padding(30)
      } else {
        // ── 设备列表：限高滚动（120~320），设备多时不撑爆弹窗 ──
        Scroll() {
          Column() {
            ForEach(this.devices, (d: UsbCamInfo) => {
              this.DeviceRow(d)
            }, (d: UsbCamInfo) => d.name)
          }
          .width('100%')
        }
        .width('100%')
        .constraintSize({ minHeight: 120, maxHeight: 320 })
        .scrollBar(BarState.Auto)
      }

      Button('关闭')
        .width('100%')
        .height(44)
        .fontSize(15)
        .fontColor('#FFFFFF')
        .backgroundColor('rgba(255,255,255,0.08)')
        .borderRadius(22)
        .margin({ top: 10 })
        .onClick(() => this.onClose?.())
    }
    .width('100%')
    .padding({ left: 16, right: 16, bottom: 34 })
    .backgroundColor('#16191F')
    .borderRadius({ topLeft: 24, topRight: 24 })
  }
}
