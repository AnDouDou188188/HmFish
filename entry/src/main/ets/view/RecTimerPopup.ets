// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ RecTimerPopup（RecTimerPopup.ets）—— 录像计时浮层                             ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【组件职责】                                                                 ║
// ║  录像中的红点 + "mm:ss" 计时浮标，浮于底部控制面板上方居中；                  ║
// ║  纯显示组件：不响应任何触摸（HitTestMode.None），点击穿透不拦截下层按钮       ║
// ║                                                                              ║
// ║ 【数据流（与 Index.ets 的交互契约）】                                         ║
// ║  recordSec 由 Index 逐秒下发（@Prop 单向）；显隐由 Index 按录像状态控制；     ║
// ║  本组件无任何内部状态，只做秒数格式化与渲染                                  ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
// ★ 照抄 Android fragment_demo.xml recTimerLayout：
//   红点(camera_rec_red_point) + "00:00" 白字 20sp，浮于控制面板上方居中，
//   录像时显示、停止后隐藏（停止录像走底部圆圈按钮，与 Android 一致）
// ★ RecTimerPopup：录像计时浮标（红点 + mm:ss 计时文字）
@Component
export struct RecTimerPopup {
  // ★ 录像已进行秒数（Index 逐秒刷新；fmt 内钳到 0 防负值显示异常）
  @Prop recordSec: number = 0;

  // ★ 秒数格式化为 mm:ss（两位补零，60 秒进位到分）
  private fmt(): string {
    const s = Math.max(0, Math.floor(this.recordSec));
    const m = Math.floor(s / 60);
    const r = s % 60;
    const mm: string = m < 10 ? `0${m}` : `${m}`;
    const ss: string = r < 10 ? `0${r}` : `${r}`;
    return `${mm}:${ss}`;
  }

  build() {
    // ── 红点 + 计时文字横排；hitTest(None) 保证触摸全部穿透到下层 ──
    Row({ space: 5 }) {
      Circle({ width: 10, height: 10 })
        .fill('#FF5252')
      Text(this.fmt())
        .fontSize(20)
        .fontColor('#FFFFFF')
    }
    .alignItems(VerticalAlign.Center)
    .padding({ left: 10, right: 14, top: 4, bottom: 4 })
    // 纯显示浮层（Android recTimerLayout 无点击），不拦截下层按钮
    .hitTestBehavior(HitTestMode.None)
  }
}
