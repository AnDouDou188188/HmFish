// ★ 表格识别结果弹窗：表格文本展示与复制
import { pasteboard } from '@kit.BasicServicesKit';
import { promptAction } from '@kit.ArkUI';

/**
 * 表格识别结果弹窗（PP-Structure SLANet）：票据纸风格
 * - Grid 固定行列模板 + GridItem rowStart/rowEnd/columnStart/columnEnd 绝对定位渲染合并单元格
 * - 表头行（thead）加粗米灰底；正文米白纸底
 * - 复制文本（TSV，可直接粘贴 Excel/WPS）/ 复制 HTML / 关闭
 */

// 单元格数据（native JSON 的 "cells" 数组元素）
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

@Component
export struct TableResultPopup {
  @Prop rows: number = 0;
  @Prop cols: number = 0;
  @Prop cells: TableCellData[] = [];
  @Prop html: string = '';
  @Prop elapsedMs: number = 0;
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

  private gridWidthVp(): number {
    return this.cols * 46;
  }

  // TSV 文本：行×列矩阵展开 span（锚点格放文本，跨占格留空），制表符分隔可直接粘 Excel
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
      Row({ space: 8 }) {
        Row().width(6).height(6).borderRadius(3).backgroundColor('#FF9500')
        Text('表格识别结果')
          .fontSize(16).fontColor('#FFFFFF').fontWeight(FontWeight.Bold).layoutWeight(1)
        Text(`${this.rows} 行 × ${this.cols} 列 · ${this.elapsedMs} ms`)
          .fontSize(10).fontColor('#FFD60A')
          .padding({ left: 8, right: 8, top: 3, bottom: 3 })
          .borderRadius(9)
          .backgroundColor('rgba(255,214,10,0.12)')
          .border({ width: 1, color: 'rgba(255,214,10,0.35)' })
        Text('✕').fontSize(16).fontColor('#B0B6C0')
          .padding({ left: 8, right: 4, top: 4, bottom: 4 })
          .onClick(() => this.onClose?.())
      }.width('100%')

      // ★ 小票"纸"：米黄底，网格还原表格结构（含合并单元格）
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
