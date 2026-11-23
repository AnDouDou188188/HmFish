// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ TableResultPopup（TableResultPopup.ets）—— 表格识别结果弹窗                    ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  点顶部"表格"徽标触发的 PP-Structure(SLANet) 表格识别结果弹窗，               ║
// ║  两态复用同一弹窗：loading 态（转圈 + 阶段提示 + 预览图大图）→ 结果态          ║
// ║  （结构框标注图缩小为小条 + Grid 绝对定位还原表格 + 复制 TSV/HTML）           ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  rows/cols/cells/html/耗时/loading/loadingHint/previewPath 全部由 Index       ║
// ║  解析 native JSON 后下发；onClose 上抛关闭；复制走系统剪贴板                  ║
// ║  （TSV 制表符分隔可直接粘贴 Excel/WPS）                                       ║
// ║                                                                              ║
// ║ 【渲染要点】                                                                 ║
// ║  TableCellData 为 native JSON "cells" 元素的 TS 映射；GridItem 用              ║
// ║  rowStart/rowEnd/columnStart/columnEnd 绝对定位渲染合并单元格；               ║
// ║  折叠边框（每格画上/左，末行补下、末列补右）避免相邻格重复双线                ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 表格识别结果弹窗：表格文本展示与复制
import { pasteboard } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';

/**
 * 表格识别结果弹窗（PP-Structure SLANet）：票据纸风格
 * - Grid 固定行列模板 + GridItem rowStart/rowEnd/columnStart/columnEnd 绝对定位渲染合并单元格
 * - 表头行（thead）加粗米灰底；正文米白纸底
 * - 复制文本（TSV，可直接粘贴 Excel/WPS）/ 复制 HTML / 关闭
 */

// ★ 单元格数据模型：native JSON "cells" 数组元素的 TS 映射
//   （行列坐标 + 跨行跨列数 + 表头标志 + 文本），导出供 Index 解析时构造
export class TableCellData {
  r: number = 0;   // 起始行
  c: number = 0;   // 起始列
  rs: number = 1;  // 跨行数
  cs: number = 1;  // 跨列数
  h: number = 0;   // 1=表头（thead）
  t: string = '';  // 单元格文本

  constructor(r: number, c: number, rs: number, cs: number, h: number, t: string) {
    this.r = r;
    this.c = c;
    this.rs = rs;
    this.cs = cs;
    this.h = h;
    this.t = t;
  }
}

// ★ TableResultPopup：表格结果弹窗主体（loading/结果两态 + Grid 表格还原）
@Component
export struct TableResultPopup {
  // ★ 表格结构（Index 解析 native JSON 后下发）：行列数/单元格数组/HTML 原文/总耗时 ms
  @Prop rows: number = 0;
  @Prop cols: number = 0;
  @Prop cells: TableCellData[] = [];
  @Prop html: string = '';
  @Prop elapsedMs: number = 0;
  // ★ loading 态：识别中立即弹面板（转圈+阶段/已用时提示），完成后原地切换结果
  @Prop loading: boolean = false;
  @Prop loadingHint: string = '';
  // ★ 预览图：loading 态=被识别帧/结构框标注图，结果态=结构框标注图（SLANet 绿框）
  @Prop previewPath: string = '';
  onClose?: () => void = () => {};

  // 行高自适应：行数多时压缩行高保证弹窗不过分高
  private rowHeightVp(): number {
    if (this.rows <= 10) return 38;
    if (this.rows <= 18) return 32;
    return 28;
  }

  // 列数多时启用横向滚动，保证每列最小宽度
  private useHScroll(): boolean {
    return this.cols > 8;
  }

  // ★ 横向滚动模式下的网格总宽：每列固定 46vp，保证窄列文字可读
  private gridWidthVp(): number {
    return this.cols * 46;
  }

  // ★ TSV 文本：行×列矩阵展开 span（锚点格放文本，跨占格留空），制表符分隔可直接粘 Excel
  private buildTsv(): string {
    if (this.rows <= 0 || this.cols <= 0) return '';
    const matrix: string[][] = [];
    for (let r = 0; r < this.rows; r++) {
      const row: string[] = [];
      for (let c = 0; c < this.cols; c++) {
        row.push('');
      }
      matrix.push(row);
    }
    for (let i = 0; i < this.cells.length; i++) {
      const cell = this.cells[i];
      if (cell.r >= 0 && cell.r < this.rows && cell.c >= 0 && cell.c < this.cols) {
        matrix[cell.r][cell.c] = cell.t;
      }
    }
    const lines: string[] = [];
    for (let r = 0; r < this.rows; r++) {
      lines.push(matrix[r].join('\t'));
    }
    return lines.join('\n');
  }

  // ★ 复制到系统剪贴板并 toast 反馈（成功文案由调用方传入，失败统一提示）
  private copyWithToast(text: string, toastOk: string): void {
    try {
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
      pasteboard.getSystemPasteboard().setData(data)
        .then((): void => {
          try { promptAction.showToast({ message: toastOk, duration: 1500 }); } catch (_) {}
        })
        .catch((): void => {
          try { promptAction.showToast({ message: '复制失败', duration: 1500 }); } catch (_) {}
        });
    } catch (_) {
      try { promptAction.showToast({ message: '复制失败', duration: 1500 }); } catch (_) {}
    }
  }

  build() {
    Column() {
      // ── 标题行：状态圆点 + 标题 + loading 转圈/规模·耗时徽标 + 关闭 ✕ ──
      Row({ space: 8 }) {
        Row().width(6).height(6).borderRadius(3).backgroundColor('#FF9500')
        Text('表格识别结果')
          .fontSize(16).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).layoutWeight(1)
        if (this.loading) {
          Row({ space: 5 }) {
            LoadingProgress().width(12).height(12).color('#FFD60A')
            Text('识别中…')
              .fontSize(10).fontColor('#FFD60A')
          }
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .borderRadius(9)
          .backgroundColor('rgba(255,214,10,0.12)')
          .border({ width: 1, color: 'rgba(255,214,10,0.35)' })
        } else {
          Text(`${this.rows} 行 × ${this.cols} 列 · ${this.elapsedMs} ms`)
            .fontSize(10).fontColor('#FFD60A')
            .padding({ left: 8, right: 8, top: 3, bottom: 3 })
            .borderRadius(9)
            .backgroundColor('rgba(255,214,10,0.12)')
            .border({ width: 1, color: 'rgba(255,214,10,0.35)' })
        }
        Text('✕').fontSize(16).fontColor('#B0B6C0')
          .padding({ left: 8, right: 4, top: 4, bottom: 4 })
          .onClick(() => this.onClose?.())
      }.width('100%')

      // ★ 小票"纸"：米黄底，网格还原表格结构（含合并单元格）；
      //   识别中显示 loading 反馈（转圈 + 阶段/已用时），完成后原地切换
      if (this.loading) {
        Column({ space: 10 }) {
          // ★ 被识别帧/结构框标注图实时预览（stage1=采集帧，stage2=带 SLANet 绿框）：
          //   识别中预览图占弹窗主体（大图），识别完成后缩小为小条
          if (this.previewPath !== '') {
            Image('file://' + this.previewPath)
              .width('100%')
              .height(300)
              .objectFit(ImageFit.Contain)
              .borderRadius(8)
              .backgroundColor('#000000')
          }
          LoadingProgress().width(46).height(46).color('#FFD60A')
          Text('正在识别表格结构')
            .fontSize(15).fontColor('#3E3A33').fontWeight(FontWeight.Bold)
          Text(this.loadingHint)
            .fontSize(11).fontColor('#8A8272')
            .textAlign(TextAlign.Center)
            .maxLines(2)
        }
        .layoutWeight(1)
        .width('100%')
        .justifyContent(FlexAlign.Center)
        .margin({ top: 10 })
        .backgroundColor('#F7F3E9')
        .borderRadius(10)
        .border({ width: 1, color: 'rgba(0,0,0,0.08)' })
        .padding(16)
      } else {
        // ★ 结果态顶部：结构框标注图缩小为小条（识别中是大图，完成后让位给表格内容）
        if (this.previewPath !== '') {
          Image('file://' + this.previewPath)
            .width('100%')
            .height(72)
            .objectFit(ImageFit.Contain)
            .borderRadius(8)
            .margin({ top: 10 })
        }
        // ── 表格还原：外层纵向滚动 + 内层横向滚动（列数>8 时）+ Grid 固定行列模板 ──
        Scroll() {
          Scroll() {
            Grid() {
              ForEach(this.cells, (cell: TableCellData) => {
                GridItem() {
                  Column() {
                    if (cell.t.length > 0) {
                      Text(cell.t)
                        .fontSize(10)
                        .lineHeight(13)
                        .fontColor(cell.h === 1 ? '#3E3A33' : '#2E2B25')
                        .fontWeight(cell.h === 1 ? FontWeight.Bold : FontWeight.Normal)
                        .textAlign(cell.h === 1 ? TextAlign.Center : TextAlign.Start)
                        .maxLines(2)
                        .textOverflow({ overflow: TextOverflow.Ellipsis })
                        .textSelectable(TextSelectableMode.SELECTABLE_FOCUSABLE)
                    }
                  }
                  .width('100%')
                  .height('100%')
                  .justifyContent(FlexAlign.Center)
                  .padding({ left: 3, right: 3, top: 2, bottom: 2 })
                }
                // ★ 绝对定位 + 跨行跨列（rowEnd/columnEnd 为包含语义：跨度 = end - start + 1）
                .rowStart(cell.r)
                .rowEnd(cell.r + cell.rs - 1)
                .columnStart(cell.c)
                .columnEnd(cell.c + cell.cs - 1)
                .backgroundColor(cell.h === 1 ? '#EAE3D2' : '#FBF8F0')
                // ★ 折叠边框：每格画上/左边缘，末行补下边缘、末列补右边缘，避免双线
                .border({
                  width: {
                    top: 1,
                    left: 1,
                    bottom: cell.r + cell.rs >= this.rows ? 1 : 0,
                    right: cell.c + cell.cs >= this.cols ? 1 : 0
                  },
                  color: '#B9B2A3'
                })
              }, (cell: TableCellData) => `${cell.r}_${cell.c}_${cell.rs}_${cell.cs}_${cell.t}`)
            }
            .columnsTemplate(`repeat(${this.cols}, 1fr)`)
            .rowsTemplate(`repeat(${this.rows}, 1fr)`)
            .width(this.useHScroll() ? this.gridWidthVp() : '100%')
            .height(this.rows * this.rowHeightVp())
            .scrollBar(BarState.Off)
          }
          .scrollable(ScrollDirection.Horizontal)
          .scrollBar(BarState.Auto)
          .width('100%')
        }
        .layoutWeight(1)
        .scrollBar(BarState.Auto)
        .width('100%')
        .align(Alignment.TopStart)
        .margin({ top: 10 })
        .backgroundColor('#F7F3E9')
        .borderRadius(10)
        .border({ width: 1, color: 'rgba(0,0,0,0.08)' })
        .padding(4)
      }

      // ── 底部操作行：loading 态只留"关闭"；结果态为 复制文本(TSV)/复制HTML/关闭 ──
      if (this.loading) {
        Button('关闭')
          .fontSize(12).fontColor('#D7DAE1')
          .padding({ left: 4, right: 4 })
          .backgroundColor('rgba(255,255,255,0.08)')
          .borderRadius(18)
          .height(34)
          .width('100%')
          .onClick(() => this.onClose?.())
      } else {
        Row({ space: 8 }) {
          Button('复制文本')
            .fontSize(12).fontColor('#003B44')
            .padding({ left: 4, right: 4 })
            .backgroundColor('#FFD60A')
            .borderRadius(18)
            .height(34)
            .layoutWeight(1)
            .onClick(() => this.copyWithToast(this.buildTsv(), '已复制表格文本（可粘贴 Excel）'))
          Button('复制HTML')
            .fontSize(12).fontColor('#003B44')
            .padding({ left: 4, right: 4 })
            .backgroundColor('#00E5FF')
            .borderRadius(18)
            .height(34)
            .layoutWeight(1)
            .onClick(() => this.copyWithToast(this.html, '已复制表格 HTML'))
          Button('关闭')
            .fontSize(12).fontColor('#D7DAE1')
            .padding({ left: 4, right: 4 })
            .backgroundColor('rgba(255,255,255,0.08)')
            .borderRadius(18)
            .height(34)
            .layoutWeight(1)
            .onClick(() => this.onClose?.())
        }
        .width('100%')
        .margin({ top: 12 })
      }
    }
    .width('88%')
    .constraintSize({ maxHeight: '70%' })
    .padding(16)
    .backgroundColor('rgba(17,21,25,0.96)')
    .borderRadius(14)
    .border({ width: 1, color: 'rgba(255,255,255,0.18)' })
    // ★ 消费弹窗内点击，阻止冒泡到全屏遮罩的关闭逻辑（同 OcrResultPopup）
    .onClick(() => {})
  }
}
