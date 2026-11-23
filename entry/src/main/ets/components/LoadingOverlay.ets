// ★ 全屏加载遮罩组件：半透明黑底 + loading 动图（对齐 Android 原版样式）。
//   由 Index.ets 在模型释放、相机会话建立等耗时阶段盖在页面上；
//   visible=true 期间 hitTestBehavior(Block) 挡住底层所有交互防误触
@Component
// ★ LoadingOverlay：纯展示组件，唯一输入 visible（父组件单向同步）
export struct LoadingOverlay {
  // ★ 显隐开关：@Prop 由父组件传入，true 时才渲染遮罩层
  @Prop visible: boolean = false;

  // ★ build：visible=false 时整棵子树不渲染（零开销隐藏）
  build() {
    if (this.visible) {
      // ★ Stack 双层叠加：先铺半透明底，再居中放动图
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
