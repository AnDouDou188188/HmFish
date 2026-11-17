// ★ 应用入口 Ability：相机/麦克风权限申请、窗口创建与主页面加载
import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { window } from '@kit.ArkUI';
import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';

const DOMAIN = 0x0000;
const TAG = 'EntryAbility';

export default class EntryAbility extends UIAbility {
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'onCreate');
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'onDestroy');
  }

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

  onForeground(): void {
    hilog.info(DOMAIN, TAG, 'onForeground');
  }

  onBackground(): void {
    hilog.info(DOMAIN, TAG, 'onBackground');
  }
}