// ★ 备份扩展 Ability：接入系统备份/恢复框架（module.json5 中声明）。
//   当前仅打日志——应用数据（设置 preferences、沙箱模型）默认不参与
//   云备份/换机迁移；如需支持，在此实现目录级的备份/恢复逻辑
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BackupExtensionAbility, BundleVersion } from '@kit.CoreFileKit';

const DOMAIN = 0x0000;

// ★ EntryBackupAbility：系统备份钩子的最小实现（无自定义数据要备份）
export default class EntryBackupAbility extends BackupExtensionAbility {
  // ★ onBackup：系统发起备份时回调
  async onBackup() {
    hilog.info(DOMAIN, 'testTag', 'onBackup ok');
    await Promise.resolve();
  }

  // ★ onRestore：系统恢复数据时回调，bundleVersion 携带来源应用版本信息
  async onRestore(bundleVersion: BundleVersion) {
    hilog.info(DOMAIN, 'testTag', 'onRestore ok %{public}s', JSON.stringify(bundleVersion));
    await Promise.resolve();
  }
}