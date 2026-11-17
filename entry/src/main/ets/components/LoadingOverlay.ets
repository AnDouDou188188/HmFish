// ★ 全屏加载遮罩：半透明黑底 + loading 动图（对齐 Android 原版样式）
@Component
export struct LoadingOverlay {
  @Prop visible: boolean = false;

  build() {
    if (this.visible) {
      Stack() {
        // ★ 照抄 Android fragment_demo.xml：全屏 #66000000 半透明黑底
        Column()
          .width('100%').height('100%')
          .backgroundColor('rgba(0, 0, 0, 0.4)')

        // ★ 照抄 Android：加载一律只显示 loading.gif（centerInside 等比缩放居中），
        //   原版无任何文案，全部加载场景统一用它替代
        Image($r('app.media.loading'))
          .width('100%').height('100%')
          .objectFit(ImageFit.Contain)
      }
      .width('100%').height('100%')
      .alignContent(Alignment.Center)
      .hitTestBehavior(HitTestMode.Block)
    }
  }
}
