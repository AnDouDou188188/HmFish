// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ UpdateDialog（UpdateDialog.ets）—— 应用内更新弹窗（居中卡片）                  ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】                                                                     ║
// ║  发现新版本时弹出（自动检查命中自动弹 / 设置页手动触发），按状态切换展示：     ║
// ║   ① 检查中（checking）：菊花 + 文案，仅"取消"                                ║
// ║   ② 发现新版本（默认态）：版本号 + 更新说明 + [立即更新][稍后提醒][跳过此版本] ║
// ║      + "打开发布页"兜底链接（GitHub 不可达时浏览器下载）                      ║
// ║   ③ 下载中（downloading）：进度条 + 百分比 + [取消下载]                       ║
// ║   ④ 失败（errorMsg）：原因 + [重试][打开发布页][关闭]                         ║
// ║  全部动作经回调上抛 Index（检查/下载/保存/跳过/稍后提醒均在页面侧实现）        ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// ★ 更新弹窗：状态由 checking/downloading/errorMsg 组合驱动，默认态=发现新版本
@Component
export struct UpdateDialog {
  // 版本信息（发现新版本时展示）
  @Prop tag: string = '';
  @Prop versionName: string = '';
  @Prop notes: string = '';
  @Prop sizeText: string = '';
  // 状态位
  @Prop checking: boolean = false;
  @Prop downloading: boolean = false;
  @Prop progressPct: number = 0;
  @Prop errorMsg: string = '';
  // 回调（全部上抛 Index 处理）
  onUpdateNow?: () => void;
  onSnooze?: () => void;
  onSkip?: () => void;
  onRetry?: () => void;
  onOpenReleases?: () => void;
  onCancelDownload?: () => void;
  onClose?: () => void;

  build() {
    Column() {
      Column({ space: 10 }) {
        // ── 标题：按状态切换 ──
        Text(this.checking ? '正在检查更新'
          : (this.downloading ? '正在下载新版本'
            : (this.errorMsg.length > 0 ? '更新失败'
              : `发现新版本 ${this.tag}`)))
          .fontSize(16).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)

        // ── ① 检查中 ──
        if (this.checking) {
          Row({ space: 8 }) {
            LoadingProgress().width(22).height(22).color('#00E5FF')
            Text('正在连接 GitHub 检查新版本…')
              .fontSize(12).fontColor('#9FE8FF')
          }.margin({ top: 4, bottom: 4 })
        }

        // ── ② 发现新版本：版本名 + 大小 + 更新说明（限高滚动） ──
        if (!this.checking && !this.downloading && this.errorMsg.length === 0) {
          if (this.versionName.length > 0) {
            Text(this.versionName)
              .fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
          }
          if (this.sizeText.length > 0) {
            Text(`安装包大小 ${this.sizeText}`)
              .fontSize(11).fontColor('#8E97A3')
          }
          if (this.notes.length > 0) {
            Scroll() {
              Text(this.notes)
                .fontSize(12).fontColor('#C7CDD6')
                .width('100%')
            }
            .align(Alignment.TopStart)
            .constraintSize({ maxHeight: 160 })
            .width('100%')
            .margin({ top: 2 })
          } else {
            Text('更新说明详见发布页')
              .fontSize(11).fontColor('#8E97A3')
          }
        }

        // ── ③ 下载中：进度条 + 百分比 ──
        if (this.downloading) {
          Row({ space: 8 }) {
            Progress({ value: this.progressPct, total: 100, type: ProgressType.Linear })
              .color('#00E5FF')
              .backgroundColor('rgba(255,255,255,0.12)')
              .height(5).layoutWeight(1)
            Text(`${this.progressPct}%`)
              .fontSize(12).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
              .width(42).textAlign(TextAlign.End)
          }
          .width('100%')
          .margin({ top: 4 })

          Text('下载中断会自动断点续传，最多自动重试 5 次')
            .fontSize(10).fontColor('#8E97A3')
            .margin({ top: 2 })
        }

        // ── ④ 失败：原因 ──
        if (this.errorMsg.length > 0) {
          Text(this.errorMsg)
            .fontSize(12).fontColor('#FF8A80')
            .width('100%')
            .margin({ top: 2 })
        }

        // ── 按钮区：按状态切换 ──
        if (this.downloading) {
          Button('取消下载')
            .fontSize(13).fontColor('#FFFFFF').backgroundColor('rgba(255,255,255,0.12)')
            .borderRadius(17).height(34).width('100%')
            .margin({ top: 6 })
            .onClick((): void => { this.onCancelDownload?.(); })
        } else if (this.checking) {
          Button('取消')
            .fontSize(13).fontColor('#8E97A3').backgroundColor(Color.Transparent)
            .borderRadius(17).height(34).width('100%')
            .margin({ top: 6 })
            .onClick((): void => { this.onClose?.(); })
        } else if (this.errorMsg.length > 0) {
          Row({ space: 10 }) {
            Button('重试')
              .fontSize(13).fontColor('#062A33').backgroundColor('#00E5FF')
              .borderRadius(17).height(34).layoutWeight(1)
              .onClick((): void => { this.onRetry?.(); })
            Button('打开发布页')
              .fontSize(13).fontColor('#E6F7FF').backgroundColor('rgba(255,255,255,0.10)')
              .borderRadius(17).height(34).layoutWeight(1)
              .onClick((): void => { this.onOpenReleases?.(); })
          }
          .width('100%')
          .margin({ top: 10 })
          Button('关闭')
            .fontSize(13).fontColor('#8E97A3').backgroundColor(Color.Transparent)
            .borderRadius(17).height(32).width('100%')
            .margin({ top: 2 })
            .onClick((): void => { this.onClose?.(); })
        } else {
          Button('立即更新')
            .fontSize(14).fontWeight(FontWeight.Bold)
            .fontColor('#062A33').backgroundColor('#00E5FF')
            .borderRadius(18).height(38).width('100%')
            .margin({ top: 8 })
            .onClick((): void => { this.onUpdateNow?.(); })
          Row({ space: 10 }) {
            Button('稍后提醒')
              .fontSize(13).fontColor('#E6F7FF').backgroundColor('rgba(255,255,255,0.10)')
              .borderRadius(17).height(34).layoutWeight(1)
              .onClick((): void => { this.onSnooze?.(); })
            Button('跳过此版本')
              .fontSize(13).fontColor('#8E97A3').backgroundColor('rgba(255,255,255,0.10)')
              .borderRadius(17).height(34).layoutWeight(1)
              .onClick((): void => { this.onSkip?.(); })
          }
          .width('100%')
          .margin({ top: 10 })
          Text('打开发布页（浏览器下载）')
            .fontSize(11).fontColor('#00E5FF')
            .margin({ top: 10 })
            .onClick((): void => { this.onOpenReleases?.(); })
        }
      }
      .width('80%')
      .padding({ left: 18, right: 18, top: 18, bottom: 16 })
      .backgroundColor('#16191F')
      .borderRadius(18)
      .border({ width: 1, color: 'rgba(0,229,255,0.25)' })
      .alignItems(HorizontalAlign.Center)
    }
    .width('100%').height('100%')
    .justifyContent(FlexAlign.Center)
    .backgroundColor('rgba(0,0,0,0.55)')
    .onClick((): void => {
      // 下载中点遮罩不关闭（防止误触丢进度），其余状态可点空白关闭
      if (!this.downloading && !this.checking) {
        this.onClose?.();
      }
    })
  }
}
