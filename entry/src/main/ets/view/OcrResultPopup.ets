// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ OcrResultPopup（OcrResultPopup.ets）—— OCR 识别结果弹窗（小票风格）          ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  点击顶部 HUD"文字识别"行的识别摘要弹出。以"小票/票据"拟物风格全量展示        ║
// ║  OCR 识别文本：米黄纸底 + 上下虚线 + 居中票头（标题+时间戳）+ 逐行内容 +       ║
// ║  行数落款；支持一键复制（保留 \n 换行）与长按自由选词                         ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  text/modelName 由 Index 下发（@Prop 只读）；onClose 上抛关闭；               ║
// ║  时间戳组件内自理（aboutToAppear 定格，不随后续刷新变化）；                   ║
// ║  复制走系统剪贴板（纯文本 MIME）+ toast 反馈                                  ║
// ║                                                                              ║
// ║ 【关闭契约】                                                                 ║
// ║  弹窗体 onClick 置空消费点击，阻止冒泡到 Index 的全屏遮罩关闭逻辑——           ║
// ║  关闭只走 ✕ / 关闭按钮 / 点遮罩空白处，长按系统选词不受影响                   ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ OCR 识别结果弹窗：文本展示与一键复制
import { pasteboard } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';

/**
 * OCR 识别结果弹窗：小票/票据风格展示
 * - 米黄"纸"底 + 上下虚线 + 居中标题 + 时间戳 + 逐行内容 + 底部行数落款
 * - 复制带格式原文（保留 \n 换行）
 * - 长按行可自由选词（系统选择手柄）
 */
// ★ OcrResultPopup：OCR 结果弹窗主体（票头/正文/落款 + 复制/关闭操作行）
@Component
export struct OcrResultPopup {
  // ★ OCR 识别全文（\n 分行，弹窗按行 ForEach 渲染，空行渲染为占位空格）
  @Prop text: string = '';
  // ★ 当前模型集名（票据顶部展示，区分是哪个模型的输出）
  @Prop modelName: string = '';
  // ★ 关闭回调（Index 隐藏弹窗与遮罩）
  onClose?: () => void = () => {};

  // ★ 票据时间戳：弹窗打开时定格
  private stamp: string = '';

  // ★ 时间戳定格：打开时刻的 年-月-日 时:分（两位补零），后续刷新不变
  aboutToAppear(): void {
    const d = new Date();
    const p2 = (n: number): string => (n < 10 ? '0' + n : `${n}`);
    this.stamp = `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ${p2(d.getHours())}:${p2(d.getMinutes())}`;
  }

  // ★ 复制全文到系统剪贴板：纯文本 MIME 保留 \n 换行；成功/失败均 toast 反馈
  private copyToClipboard(): void {
    try {
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.text);
      pasteboard.getSystemPasteboard().setData(data)
        .then((): void => {
          try { promptAction.showToast({ message: '已复制到剪贴板（保留换行格式）', duration: 1500 }); } catch (_) {}
        })
        .catch((): void => {
          try { promptAction.showToast({ message: '复制失败', duration: 1500 }); } catch (_) {}
        });
    } catch (_) {
      try { promptAction.showToast({ message: '复制失败', duration: 1500 }); } catch (_) {}
    }
  }

  // ★ 票据虚线分隔（纸色系）
  @Builder
  receiptDivider() {
    Row()
      .width('100%')
      .height(1)
      .border({ width: { bottom: 1 }, style: BorderStyle.Dashed, color: '#B9B2A3' })
      .margin({ top: 8, bottom: 8 })
  }

  build() {
    Column() {
      // ── 标题行：状态圆点 + "识别结果" + 模型集徽标 + 关闭 ✕ ──
      Row({ space: 8 }) {
        Row().width(6).height(6).borderRadius(3).backgroundColor('#00E5FF')
        Text('识别结果')
          .fontSize(16).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).layoutWeight(1)
        if (this.modelName.length > 0) {
          Text(this.modelName)
            .fontSize(10).fontColor('#00E5FF')
            .padding({ left: 8, right: 8, top: 3, bottom: 3 })
            .borderRadius(9)
            .backgroundColor('rgba(0,229,255,0.12)')
            .border({ width: 1, color: 'rgba(0,229,255,0.35)' })
        }
        Text('✕').fontSize(16).fontColor('#B0B6C0')
          .padding({ left: 8, right: 4, top: 4, bottom: 4 })
          .onClick(() => this.onClose?.())
      }.width('100%')

      // ★ 小票"纸"：米黄底 + 深字，逐行展示识别内容
      Scroll() {
        Column() {
          // 票头：居中标题 + 时间戳
          Text('· 识 别 结 果 ·')
            .fontSize(13).fontColor('#3E3A33').fontWeight(FontWeight.Bold)
            .width('100%').textAlign(TextAlign.Center)
          Text(this.stamp)
            .fontSize(10).fontColor('#8A8375')
            .width('100%').textAlign(TextAlign.Center)
            .margin({ top: 3 })

          this.receiptDivider()

          if (this.text.length === 0) {
            Text('暂无识别内容')
              .fontSize(13).fontColor('#8A8375')
              .padding({ top: 20, bottom: 20 })
              .width('100%').textAlign(TextAlign.Center)
          } else {
            ForEach(this.text.split('\n'), (line: string, idx: number) => {
              Text(line.length > 0 ? line : ' ')
                .fontSize(14)
                .fontColor('#2E2B25')
                .lineHeight(22)
                .width('100%')
                // ★ 自由选词：长按可选中和拖选任意字词（配合系统选择手柄）
                .textSelectable(TextSelectableMode.SELECTABLE_FOCUSABLE)
            }, (line: string, idx: number) => `${idx}_${line}`)
          }

          this.receiptDivider()

          // 票尾：行数落款
          Text(this.text.length > 0 ? `共 ${this.text.split('\n').length} 行 · — 完 —` : '— 完 —')
            .fontSize(10).fontColor('#8A8375')
            .width('100%').textAlign(TextAlign.Center)
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
        .padding({ left: 14, right: 14, top: 12, bottom: 12 })
        .backgroundColor('#F7F3E9')
        .borderRadius(10)
        .border({ width: 1, color: 'rgba(0,0,0,0.08)' })
      }
      .layoutWeight(1)
      .scrollBar(BarState.Auto)
      .width('100%')
      .align(Alignment.TopStart)
      .margin({ top: 10 })

      // ── 底部操作行：复制全文（保留换行格式）/ 关闭 ──
      Row({ space: 10 }) {
        Button('复制')
          .fontSize(14).fontColor('#003B44')
          .backgroundColor('#00E5FF')
          .borderRadius(18)
          .height(36)
          .layoutWeight(1)
          .onClick(() => this.copyToClipboard())
        Button('关闭')
          .fontSize(14).fontColor('#D7DAE1')
          .backgroundColor('rgba(255,255,255,0.08)')
          .borderRadius(18)
          .height(36)
          .layoutWeight(1)
          .onClick(() => this.onClose?.())
      }
      .width('100%')
      .margin({ top: 12 })
    }
    .width('86%')
    .constraintSize({ maxHeight: '62%' })
    .padding(16)
    .backgroundColor('rgba(17,21,25,0.96)')
    .borderRadius(14)
    .border({ width: 1, color: 'rgba(255,255,255,0.18)' })
    // ★ 消费弹窗内点击，阻止冒泡到全屏遮罩的关闭逻辑：
    //   点弹窗内容不再"一点就退出"，长按系统选字（textSelectable）不受影响；
    //   关闭仍走 ✕ / 关闭按钮 / 点遮罩空白处
    .onClick(() => {})
  }
}
