// ★ 语音播报服务：CoreSpeechKit TTS 封装（懒创建/静音拦截/抢占播报/节流）
import { textToSpeech } from '@kit.CoreSpeechKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';

/**
 * 语音播报服务：封装 CoreSpeechKit TTS 引擎
 * - 懒创建引擎（首次播报才 createEngine，避免不用语音也占实例名额）
 * - 静音/震动模式下不播报任何语音（铃声模式 != NORMAL 直接拦截）
 * - 音量 0~100 线性映射 TTS volume 0~1（默认 30%，避免默认太响）
 * - 抢占模式播报（queueMode=1）：新播报打断旧播报，避免排长队
 * - 识别结果播报自带节流：同内容不重复、最短间隔 5 秒
 */
export class SpeechService {
  private engine: textToSpeech.TextToSpeechEngine | null = null;
  private creating: boolean = false;
  private reqSeq: number = 0;
  private lastAnnounceMs: number = 0;
  private lastAnnounceText: string = '';

  enabled: boolean = true;
  volumePct: number = 30;

  // ★ 静音判定：铃声模式非响铃（静音/震动）时，一切语音与提示音都不播
  static isRingerSilent(): boolean {
    try {
      const mgr = audio.getAudioManager().getVolumeManager()
        .getVolumeGroupManagerSync(audio.DEFAULT_VOLUME_GROUP_ID);
      return mgr.getRingerModeSync() !== audio.AudioRingMode.RINGER_MODE_NORMAL;
    } catch (_) {
      return false;
    }
  }

  // ★ 懒创建引擎：createEngine 名额全场最多 3 个，不用不建
  private async ensureEngine(): Promise<boolean> {
    if (this.engine) return true;
    if (this.creating) return false;
    this.creating = true;
    try {
      const extraParam: Record<string, Object> = { 'style': 'interaction-broadcast' };
      const createParams: textToSpeech.CreateEngineParams = {
        language: 'zh-CN',
        person: 0,
        online: 1,
        extraParams: extraParam
      };
      this.engine = await textToSpeech.createEngine(createParams);
      return true;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[Speech] createEngine failed: ${err.code} ${err.message}`);
      return false;
    } finally {
      this.creating = false;
    }
  }

  // ★ 立即播报（按钮点击、设置切换等交互反馈）
  speak(text: string): void {
    if (!this.enabled || text.length === 0) return;
    if (SpeechService.isRingerSilent()) return;
    this.doSpeak(text);
  }

  private async doSpeak(text: string): Promise<void> {
    const ok = await this.ensureEngine();
    if (!ok || !this.engine) return;
    // ★ 引擎异步创建期间可能已切静音：发播报前再拦一次
    if (SpeechService.isRingerSilent()) return;
    this.reqSeq++;
    const extraParam: Record<string, Object> = {
      'queueMode': 1,
      'speed': 1,
      'volume': Math.max(0, Math.min(1, this.volumePct / 100)),
      'pitch': 1,
      'languageContext': 'zh-CN',
      'audioType': 'pcm',
      'playType': 1
    };
    const speakParams: textToSpeech.SpeakParams = {
      requestId: `fish${this.reqSeq}`,
      extraParams: extraParam
    };
    try {
      this.engine.speak(text, speakParams);
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[Speech] speak failed: ${err.code} ${err.message}`);
    }
  }

  // ★ 识别结果播报：内容变化才播 + 最短 5 秒间隔（识别循环高频回调，不节流会刷屏）
  announceDetections(text: string): void {
    if (!this.enabled || text.length === 0) return;
    const now = Date.now();
    if (text === this.lastAnnounceText && now - this.lastAnnounceMs < 15000) return;
    if (text !== this.lastAnnounceText && now - this.lastAnnounceMs < 5000) return;
    this.lastAnnounceMs = now;
    this.lastAnnounceText = text;
    this.speak(text);
  }

  stop(): void {
    try { if (this.engine) this.engine.stop(); } catch (_) {}
  }

  shutdown(): void {
    try { if (this.engine) this.engine.shutdown(); } catch (_) {}
    this.engine = null;
  }
}
