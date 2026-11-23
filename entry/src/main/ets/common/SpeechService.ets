// ★ 语音播报服务：CoreSpeechKit TTS 引擎封装。
//   设计要点：懒创建（引擎名额全场仅 3 个）、静音/震动模式全拦截、
//   queueMode=1 抢占播报（新打断旧不排队）、识别结果播报节流。
//   被 Index.ets（设置语音开关/音量）与识别结果链路（detectSpeak 开启时
//   朗读鱼群统计）共同使用；与 native（libentry.so）无直接交互
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
// ★ SpeechService：TTS 引擎的进程级封装（一个实例持有一个引擎句柄）
export class SpeechService {
  // ★ TTS 引擎句柄：懒创建，shutdown 后置空允许下次重建
  private engine: textToSpeech.TextToSpeechEngine | null = null;
  // ★ 创建互斥：并发首次播报只触发一次 createEngine，后来的直接放弃
  private creating: boolean = false;
  // ★ 请求序号：拼接 requestId（fish1/fish2...），便于系统侧日志对账
  private reqSeq: number = 0;
  // ★ 节流状态：上次播报时间与文本（同文本 15s / 异文本 5s 内不重播）
  private lastAnnounceMs: number = 0;
  private lastAnnounceText: string = '';

  // ★ 对外开关与音量（0~100）：由设置页 speechOn/speechVolume 同步赋值
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

  // ★ 立即播报（按钮点击、设置切换等交互反馈）：不走节流，
  //   只做开关/静音两道拦截
  speak(text: string): void {
    if (!this.enabled || text.length === 0) return;
    if (SpeechService.isRingerSilent()) return;
    this.doSpeak(text);
  }

  // ★ doSpeak：确保引擎就绪后以抢占模式发播报；
  //   音量做 0~100 → 0~1 线性映射并夹紧，防止设置越界导致引擎报错
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

  // ★ stop：打断当前播报（切换设置/停止播报按钮用），不释放引擎
  stop(): void {
    try { if (this.engine) this.engine.stop(); } catch (_) {}
  }

  // ★ shutdown：彻底释放引擎（Ability 销毁时必须调用——引擎名额有限要还）
  shutdown(): void {
    try { if (this.engine) this.engine.shutdown(); } catch (_) {}
    this.engine = null;
  }
}
