// ★ 应用入口 Ability：启动即申请相机/麦克风权限（探鱼相机两者缺一不可），
//   创建窗口加载 pages/Index，并设置全屏沉浸布局。
//   权限在 loadContent 之前串行申请：首页一进来即可直接开相机，
//   避免 Index 初始化中途再弹权限框打断流程
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';

// ★ hilog 日志域与标签（hdc 过滤本应用日志用）
const DOMAIN = 0x0000;
const TAG = 'EntryAbility';

export default class EntryAbility extends UIAbility {
  // ★ onCreate：进程/Ability 创建（当前无额外初始化，仅生命周期打点）
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }

  // ★ onWindowStageCreate：权限申请 → 加载首页 → 全屏化，顺序不可颠倒
  async onWindowStageCreate(windowStage: window.WindowStage): Promise<void> {
    hilog.info(DOMAIN, TAG, 'onWindowStageCreate');

    const perms: Permissions[] = [
      'ohos.permission.CAMERA',
      'ohos.permission.MICROPHONE'
    ];
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      const result = await atManager.requestPermissionsFromUser(this.context, perms);
      hilog.info(DOMAIN, TAG, `permission result: ${JSON.stringify(result.authResults)}`);
    } catch (e) {
      hilog.error(DOMAIN, TAG, `request permission failed`);
    }

    windowStage.loadContent('pages/Index', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, TAG, `loadContent failed: ${err.message}`);
        return;
      }
      hilog.info(DOMAIN, TAG, 'loadContent succeeded');
      // ★ 折叠屏展开/旋转后 app 整体全屏：内容铺满整个窗口（含系统栏区域），
      //   否则展开态窗口按安全区缩排，app 看起来不是全屏
      try {
        const win = windowStage.getMainWindowSync();
        win.setWindowLayoutFullScreen(true);
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'setWindowLayoutFullScreen failed');
      }
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onWindowStageDestroy');
  }

  // ★ 前后台切换回调：当前仅打点（相机释放/恢复由 Index 页面生命周期负责）
  onForeground(): void {
    hilog.info(DOMAIN, TAG, 'onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground');
  }
}