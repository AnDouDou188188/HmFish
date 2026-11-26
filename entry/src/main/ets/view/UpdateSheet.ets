// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ UpdateSheet（UpdateSheet.ets）—— 应用更新半模态弹窗（手动检查入口）            ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  点击顶部 HUD 版本胶囊弹出。集中展示更新相关信息：                             ║
// ║   1. 当前版本号 + 检查更新按钮（手动触发，检查中转菊花不可重复点）             ║
// ║   2. 自动检查开关（开：每小时后台检查 + 启动 20s 后首检 + 24h 间隔节流）       ║
// ║   3. 发现新版本时展示 tag 与"查看详情"（跳更新弹窗）/ 无新版本时提示已是最新    ║
// ║   4. 打开发布页兜底链接（GitHub 不可达时浏览器下载）                           ║
// ║                                                                              ║
// ║ 【数据流】全部状态 @Prop 由 Index 下发；动作经 onXxx 回调上抛（检查/下载/      ║
// ║  开关均在页面侧实现，本组件无副作用）                                          ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 应用更新弹窗：当前版本 / 检查更新 / 自动检查开关 / 新版本入口 / 发布页链接
@Component
export struct UpdateSheet {
  // ★ 版本信息：当前版本号 / 最新版本 tag（有新版本时展示）
  @Prop appVersion: string = '';
  @Prop latestTag: string = '';
  @Prop updateAvailable: boolean = false;
  // ★ 状态位：检查中（按钮转菊花）/ 上次检查结果文案
  @Prop checking: boolean = false;
  @Prop lastCheckText: string = '';
  // ★ 自动检查开关（开：每小时 tick + 24h 节流 + 稍后提醒/跳过版本生效）
  @Prop autoCheck: boolean = true;
  // ── 交互回调：关闭/立即检查/自动检查切换/查看新版本详情/打开发布页 ──
  onClose?: () => void;
  onCheckNow?: () => void;
  onAutoCheckToggle?: () => void;
  onViewUpdate?: () => void;
  onOpenReleases?: () => void;

  @State appear: boolean = false;

  aboutToAppear(): void {
    setTimeout(() => { this.appear = true; }, 30);
  }

  build() {
    Stack({ alignContent: Alignment.Bottom }) {
      // ── 全屏半透明遮罩：点击关闭 ──
      Column()
        .width('100%').height('100%')
        .backgroundColor(this.appear ? 'rgba(0,0,0,0.5)' : 'rgba(0,0,0,0)')
        .animation({ duration: 220, curve: Curve.EaseOut })
        .onClick((): void => { this.onClose?.(); })

      // ── 底部面板：上滑入场 ──
      Column() {
        Row()
          .width(42).height(4).borderRadius(2)
          .backgroundColor('rgba(255,255,255,0.25)')
          .margin({ top: 10 })

        Row() {
          Text('应用更新').fontSize(17).fontColor('#FFFFFF').fontWeight(FontWeight.Bold)
          Blank()
          Text('✕').fontSize(15).fontColor('#8E97A3')
            .padding({ left: 14, right: 14, top: 6, bottom: 6 })
            .onClick((): void => { this.onClose?.(); })
        }
        .width('100%')
        .padding({ left: 20, right: 10, top: 12 })

        // ── 当前版本 + 检查更新按钮 ──
        Row({ space: 8 }) {
          Text('⬆').fontSize(14).fontColor('#00E5FF')
          Text('当前版本').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
          Text(`v${this.appVersion}`).fontSize(13).fontColor('#00E5FF').fontWeight(FontWeight.Bold)
          Blank()
          if (this.checking) {
            Row({ space: 6 }) {
              LoadingProgress().width(16).height(16).color('#00E5FF')
              Text('检查中…').fontSize(12).fontColor('#9FE8FF')
            }
          } else {
            Button('检查更新')
              .fontSize(12).fontColor('#062A33').backgroundColor('#00E5FF')
              .borderRadius(13).height(28)
              .onClick((): void => { this.onCheckNow?.(); })
          }
        }
        .width('100%')
        .margin({ top: 16 })

        if (this.lastCheckText.length > 0) {
          Text(this.lastCheckText)
            .fontSize(10).fontColor('#8E97A3')
            .width('100%').margin({ top: 6 })
        }

        // ── 新版本信息（有新版本时展示 tag + 查看详情入口） ──
        if (this.updateAvailable) {
          Row({ space: 8 }) {
            Text('⤓').fontSize(14).fontColor('#00E5FF')
            Column({ space: 3 }) {
              Text('发现新版本').fontSize(13).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
              Text(this.latestTag).fontSize(11).fontColor('#00E5FF')
            }
            .alignItems(HorizontalAlign.Start)
            .layoutWeight(1)
            Button('查看详情')
              .fontSize(12).fontColor('#062A33').backgroundColor('#00E5FF')
              .borderRadius(13).height(28)
              .onClick((): void => { this.onViewUpdate?.(); })
          }
          .width('100%')
          .padding({ left: 12, right: 12, top: 10, bottom: 10 })
          .margin({ top: 12 })
          .borderRadius(12)
          .backgroundColor('rgba(0,229,255,0.10)')
          .border({ width: 1, color: 'rgba(0,229,255,0.30)' })
        }

        // ── 自动检查开关 ──
        Row({ space: 8 }) {
          Text('⟳').fontSize(14).fontColor('#00E5FF')
          Text('自动检查更新').fontSize(14).fontColor('#EDEFF2').fontWeight(FontWeight.Bold)
          Text(this.autoCheck ? '已开启' : '已关闭').fontSize(11)
            .fontColor(this.autoCheck ? '#00E5FF' : '#6E7681')
          Blank()
          Toggle({ type: ToggleType.Switch, isOn: this.autoCheck })
            .selectedColor('#00E5FF')
            .onChange((isOn: boolean) => {
              if (isOn !== this.autoCheck) this.onAutoCheckToggle?.();
            })
        }
        .width('100%')
        .margin({ top: 20 })

        Text('开启后每小时后台检查一次（24 小时内不重复提醒）；GitHub 无法访问时可打开发布页用浏览器下载')
          .fontSize(10).fontColor('#8E97A3')
          .width('100%').margin({ top: 6 })

        // ── 发布页链接（浏览器兜底） ──
        Text('打开发布页（浏览器）')
          .fontSize(12).fontColor('#00E5FF')
          .margin({ top: 14, bottom: 30 })
          .onClick((): void => { this.onOpenReleases?.(); })
      }
      .width('100%')
      .constraintSize({ maxHeight: '70%' })
      .backgroundColor('#16191F')
      .borderRadius({ topLeft: 24, topRight: 24 })
      .translate({ y: this.appear ? 0 : 620 })
      .animation({ duration: 280, curve: Curve.EaseOut })
    }
    .width('100%').height('100%')
  }
}
