// ★ 自动录像触发音效服务：目标触发自动录像时先播提示音（再接 TTS 播报）。
//   默认 rawfile/alarm.mp3，支持用户自定义 MP3（沙箱路径）与按录像时长截断播放。
//   播放链路为纯 ArkTS（AVPlayer），与 native（libentry.so）无交互；
//   静音判定复用 SpeechService.isRingerSilent，保证"静音模式下一切不出声"
import { media } from '@kit.MediaKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { SpeechService } from './SpeechService';

/**
 * 自动录像触发音效：
 * - 默认播放 rawfile/alarm.mp3；用户选择自定义 MP3 后优先播放自定义文件（沙箱路径）
 * - 静音/震动模式下不播放任何提示音（铃声模式 != NORMAL 直接拦截）
 * - enabled=false（记录设置里关掉"触发音效"）时不播放
 * - maxPlayMs（录像时长）>0 时：MP3 总时长不超过它则完整播放；
 *   超过则跳到"高潮部分"（约 35% 处，通常副歌附近）只播不超过录像时长的一段
 */
// ★ AlertService：单实例 AVPlayer 串行播放（busy 期间新请求直接回调跳过）
export class AlertService {
  private player: media.AVPlayer | null = null;
  // ★ 播放互斥：一次只播一段，期间的新请求视为放弃（防告警音叠播）
  private busy: boolean = false;
  // ★ 一次性完成回调：finish 时取出并清空，保证只触发一次
  private onDone: (() => void) | null = null;
  // ★ 截断播放：到点 stop()，触发 'stopped' 后走完成回调
  private stopTimer: number = -1;
  // ★ 本轮播放的时长上限（<=0 = 不限制，完整播放）
  private maxPlayMs: number = 0;

  enabled: boolean = true;
  // ★ 自定义 MP3 沙箱路径（空串 = 默认 alarm.mp3）
  customPath: string = '';

  // ★ 播放完成回调（onFinished）：播放真正结束（completed/stopped/error/未播放）后触发，
  //   调用方用它串联后续语音（先 MP3 告警、再 TTS 播报）
  // ★ maxPlayMs：最多播放多久（录像时长）；MP3 比它短则全播，比它长只播高潮段
  play(ctx: common.UIAbilityContext, onFinished?: () => void, maxPlayMs?: number): void {
    if (!this.enabled) {
      if (onFinished) onFinished();
      return;
    }
    if (SpeechService.isRingerSilent()) {
      if (onFinished) onFinished();
      return;
    }
    if (this.busy) {
      if (onFinished) onFinished();
      return;
    }
    this.busy = true;
    this.onDone = onFinished ? onFinished : null;
    this.maxPlayMs = maxPlayMs && maxPlayMs > 0 ? maxPlayMs : 0;
    this.doPlay(ctx);
  }

  // ★ 统一收尾：清定时器 + 释放 busy + 触发一次性完成回调
  private finish(): void {
    if (this.stopTimer >= 0) {
      clearTimeout(this.stopTimer);
      this.stopTimer = -1;
    }
    this.busy = false;
    if (this.onDone) {
      const cb = this.onDone;
      this.onDone = null;
      cb();
    }
  }

  // ★ doPlay：走 AVPlayer 状态机（idle→initialized→prepared→playing→completed），
  //   每次播放重建播放器（上次资源先 release），流转全部由 stateChange 驱动
  private async doPlay(ctx: common.UIAbilityContext): Promise<void> {
    try {
      if (this.player) {
        try { await this.player.release(); } catch (_) {}
      }
      this.player = await media.createAVPlayer();

      // ★ 先注册监听再设置数据源，避免错过 initialized 状态转换
      this.player.on('stateChange', (state: string) => {
        if (state === 'initialized') {
          if (this.player) this.player.prepare();
        } else if (state === 'prepared') {
          this.onPrepared();
        } else if (state === 'completed') {
          if (this.player) this.player.reset();
          this.finish();
        } else if (state === 'stopped') {
          // ★ 截断播放 stop() 到达：同样走收尾（回调触发，语音接着播）
          if (this.player) this.player.reset();
          this.finish();
        }
      });
      this.player.on('error', (err: BusinessError) => {
        console.error(`[AlertService] player error: ${err.code} ${err.message}`);
        this.finish();
      });

      // ★ 自定义 MP3 存在且可读 → 播自定义；否则播内置 alarm.mp3
      let useCustom = false;
      if (this.customPath.length > 0) {
        try {
          useCustom = fs.accessSync(this.customPath);
        } catch (_) {
          useCustom = false;
        }
      }
      if (useCustom) {
        // ★ 沙箱 MP3 必须用 fd:// 播放（AVPlayer 不支持 file://，会静默失败无声）
        const file = fs.openSync(this.customPath, fs.OpenMode.READ_ONLY);
        this.player.url = `fd://${file.fd}`;
      } else {
        const mgr = ctx.resourceManager;
        const fd = await mgr.getRawFd('alarm.mp3');
        this.player.fdSrc = { fd: fd.fd, offset: fd.offset, length: fd.length };
      }
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[AlertService] play failed: ${err.message}`);
      this.finish();
    }
  }

  // ───────── 播放策略（prepared 后决策） ─────────
  // ★ prepared 后决定播放策略：
  //   时长未知(-1) 或 <= maxPlayMs → 完整播放；
  //   超过 maxPlayMs → seek 到 35% 处（高潮段启发式），播 min(maxPlayMs, 剩余) 后 stop；
  //   未传 maxPlayMs 时兜底 30s 硬上限，杜绝音乐一直播放
  private onPrepared(): void {
    if (!this.player) return;
    const dur = this.player.duration;
    const capMs = this.maxPlayMs > 0 ? this.maxPlayMs : 30000;
    if (dur <= 0 || dur <= capMs) {
      this.player.play();
      return;
    }
    // ★ 高潮部分：跳过前奏，从约 35% 处（副歌附近）开始
    const startMs = Math.floor(dur * 0.35);
    const playLen = Math.min(capMs, dur - startMs);
    this.player.on('seekDone', () => {
      if (!this.player) return;
      this.player.play();
      this.stopTimer = setTimeout(() => {
        this.stopTimer = -1;
        if (this.player) {
          try { this.player.stop(); } catch (_) { this.finish(); }
        } else {
          this.finish();
        }
      }, playLen);
    });
    this.player.seek(startMs, media.SeekMode.SEEK_PREV_SYNC);
  }

  // ★ release：彻底释放播放器（页面销毁/设置变更时调用）
  async release(): Promise<void> {
    if (this.player) {
      try { await this.player.release(); } catch (_) {}
      this.player = null;
      this.finish();
    }
  }
}
