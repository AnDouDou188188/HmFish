// ★ 照抄 Android fragment_demo.xml recTimerLayout：
//   红点(camera_rec_red_point) + "00:00" 白字 20sp，浮于控制面板上方居中，
//   录像时显示、停止后隐藏（停止录像走底部圆圈按钮，与 Android 一致）
@Component
export struct RecTimerPopup {
  @Prop recordSec: number = 0;

  private fmt(): string {
    const s = Math.max(0, Math.floor(this.recordSec));
    const m = Math.floor(s / 60);
    const r = s % 60;
    const mm: string = m < 10 ? `0${m}` : `${m}`;
    const ss: string = r < 10 ? `0${r}` : `${r}`;
    return `${mm}:${ss}`;
  }

  build() {
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
