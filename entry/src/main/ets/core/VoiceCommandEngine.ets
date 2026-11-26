// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ VoiceCommandEngine（VoiceCommandEngine.ets）—— 离线语音识别会话引擎         ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】封装 CoreSpeechKit 离线中文识别（speechRecognizer，端侧 AI 模型）：  ║
// ║  · createEngine 注入热词表（sysGeneralLexicon）——命令词优先识别，           ║
// ║    从源头提升"录像/截图/画中画/模型名"等专有词的命中率（AI 纠正第一级）      ║
// ║  · AudioCapturer 16k/单声道/16bit PCM 采集 → 1280 字节分片喂 writeAudio     ║
// ║  · VAD 自动断句（vadEnd→finish）+ 10s 超时兜底 + 中间结果实时回调           ║
// ║  · 会话级创建/释放麦克风（不长期占用，隐私指示灯不常亮）                     ║
// ║ 引擎实例跨会话复用（热词变化时重建），destroy 时归还系统引擎名额            ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { speechRecognizer } from '@kit.CoreSpeechKit';
import { audio } from '@kit.AudioKit';
import { BusinessError } from '@kit.BasicServicesKit';

// ★ 会话回调：中间识别文本（实时上屏）/ 最终文本 / 错误
export interface VcSessionCallbacks {
  onPartial: (text: string) => void;
  onFinal: (text: string) => void;
  onError: (msg: string) => void;
}

export class VoiceCommandEngine {
  // ★ 离线识别引擎句柄（跨会话复用；热词变更时 shutdown 重建）
  private engine: speechRecognizer.SpeechRecognitionEngine | null = null;
  private creatingEngine: boolean = false;
  private hotwordsKey: string = '';
  // ★ 麦克风采集器（会话级创建/释放，不长期占用）
  private capturer: audio.AudioCapturer | null = null;
  // ★ 会话状态
  private sessionId: string = '';
  private sessionSeq: number = 0;
  private listening: boolean = false;
  private acc: number[] = [];
  private finalText: string = '';
  private gotFinal: boolean = false;
  private vadEnded: boolean = false;
  private cb: VcSessionCallbacks | null = null;
  private timeoutId: number = -1;
  private finishId: number = -1;

  // ★ 引擎懒创建（含热词注入）：热词集合变化 → 重建引擎让新热词生效
  private async ensureEngine(hotwords: string[]): Promise<boolean> {
    const key = hotwords.join('|');
    if (this.engine !== null && this.hotwordsKey === key) return true;
    if (this.creatingEngine) return false;
    this.creatingEngine = true;
    if (this.engine !== null) {
      try { this.engine.shutdown(); } catch (_) {}
      this.engine = null;
    }
    try {
      const extra: Record<string, Object> = { 'locate': 'CN', 'recognizerMode': 'short' };
      if (hotwords.length > 0) {
        extra['sysGeneralLexicon'] = hotwords;
      }
      const params: speechRecognizer.CreateEngineParams = {
        language: 'zh-CN',
        online: 1,
        extraParams: extra
      };
      this.engine = await speechRecognizer.createEngine(params);
      this.hotwordsKey = key;
      return true;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[VoiceCmd] createEngine failed: ${err.code} ${err.message}`);
      this.engine = null;
      return false;
    } finally {
      this.creatingEngine = false;
    }
  }

  // ★ 是否正在聆听（UI 状态用）
  isListening(): boolean {
    return this.listening;
  }

  // ★ 启动一次识别会话：采集→识别→VAD 断句/超时→最终文本回调
  async startSession(hotwords: string[], cb: VcSessionCallbacks): Promise<boolean> {
    if (this.listening) return false;
    this.cb = cb;
    const ok = await this.ensureEngine(hotwords);
    if (!ok || this.engine === null) {
      cb.onError('离线识别引擎不可用');
      return false;
    }
    this.sessionSeq++;
    this.sessionId = `vcmd-${this.sessionSeq}`;
    this.finalText = '';
    this.gotFinal = false;
    this.vadEnded = false;
    this.acc = [];
    const engine = this.engine;
    engine.setListener({
      onStart: (_sid: string, _msg: string): void => {},
      onEvent: (_sid: string, code: number, _msg: string): void => {
        // ★ VAD 判定说完（code 3）：稍等 200ms 收尾再 finish，掐掉拖尾噪声
        if (code === 3 && !this.vadEnded) {
          this.vadEnded = true;
          this.scheduleFinish(200);
        }
      },
      onResult: (_sid: string, result: speechRecognizer.SpeechRecognitionResult): void => {
        if (result.result.length > 0) {
          this.finalText = result.result;
          if (!result.isLast && this.cb !== null) {
            this.cb.onPartial(result.result);
          }
        }
        if (result.isLast) {
          this.completeSession();
        }
      },
      onComplete: (_sid: string, _msg: string): void => {
        this.completeSession();
      },
      onError: (_sid: string, code: number, msg: string): void => {
        this.failSession(`识别失败(${code})：${msg}`);
      }
    });
    try {
      engine.startListening({
        sessionId: this.sessionId,
        audioInfo: {
          audioType: 'pcm',
          sampleRate: 16000,
          soundChannel: 1,
          sampleBit: 16
        }
      });
    } catch (e) {
      const err = e as BusinessError;
      this.failSession(`启动识别失败：${err.message}`);
      return false;
    }
    // ★ 麦克风采集：16k/单声道/S16LE，readData 回调分片喂识别引擎
    try {
      const opts: audio.AudioCapturerOptions = {
        streamInfo: {
          samplingRate: 16000,
          channels: audio.AudioChannel.CHANNEL_1,
          sampleFormat: audio.AudioSampleFormat.SAMPLE_FORMAT_S16LE,
          encodingType: audio.AudioEncodingType.ENCODING_TYPE_RAW
        },
        capturerInfo: {
          source: audio.SourceType.SOURCE_TYPE_MIC,
          capturerFlags: 0
        }
      };
      this.capturer = await audio.createAudioCapturer(opts);
      this.capturer.on('readData', (buf: ArrayBuffer): void => {
        this.feedChunk(new Uint8Array(buf));
      });
      await this.capturer.start();
    } catch (e) {
      const err = e as BusinessError;
      this.failSession(`麦克风启动失败：${err.message}`);
      return false;
    }
    this.listening = true;
    // ★ 10s 无断句兜底：强制 finish 让引擎出最终结果
    this.timeoutId = setTimeout((): void => {
      this.scheduleFinish(0);
    }, 10000);
    return true;
  }

  // ★ PCM 分片缓冲：writeAudio 仅接受 640/1280 字节，攒够 1280 发一片
  private feedChunk(u: Uint8Array): void {
    if (!this.listening || this.engine === null) return;
    for (let i = 0; i < u.length; i++) {
      this.acc.push(u[i]);
    }
    while (this.acc.length >= 1280) {
      const chunk = new Uint8Array(1280);
      for (let i = 0; i < 1280; i++) {
        chunk[i] = this.acc[i];
      }
      this.acc = this.acc.slice(1280);
      try {
        this.engine.writeAudio(this.sessionId, chunk);
      } catch (_) {
        // 写入失败（会话已结束等）：静默丢帧，等待 complete 收尾
      }
    }
  }

  // ★ 延迟 finish（VAD 断句后 200ms / 超时 0ms），防止重复调用
  private scheduleFinish(delayMs: number): void {
    if (this.finishId !== -1) return;
    this.finishId = setTimeout((): void => {
      this.finishId = -1;
      if (!this.listening || this.engine === null) return;
      try {
        this.engine.finish(this.sessionId);
      } catch (_) {}
    }, delayMs);
  }

  // ★ 会话完成：停麦、清定时器、上抛最终文本
  private completeSession(): void {
    if (!this.listening) return;
    this.listening = false;
    this.releaseCapturer();
    if (this.timeoutId !== -1) {
      clearTimeout(this.timeoutId);
      this.timeoutId = -1;
    }
    if (this.finishId !== -1) {
      clearTimeout(this.finishId);
      this.finishId = -1;
    }
    if (this.cb !== null) {
      this.cb.onFinal(this.finalText);
    }
  }

  // ★ 会话失败：停麦、清定时器、上抛错误
  private failSession(msg: string): void {
    if (!this.listening) {
      // 引擎 startListening 阶段就失败（listening 还未置 true）
      this.releaseCapturer();
    } else {
      this.listening = false;
      this.releaseCapturer();
    }
    if (this.timeoutId !== -1) {
      clearTimeout(this.timeoutId);
      this.timeoutId = -1;
    }
    if (this.finishId !== -1) {
      clearTimeout(this.finishId);
      this.finishId = -1;
    }
    if (this.cb !== null) {
      this.cb.onError(msg);
    }
  }

  // ★ 释放麦克风（会话结束即释放，不长期占用）
  private releaseCapturer(): void {
    const c = this.capturer;
    this.capturer = null;
    if (c === null) return;
    try { c.stop(); } catch (_) {}
    try { c.release(); } catch (_) {}
  }

  // ★ 用户主动取消当前会话（再点麦克风/退后台）
  cancel(): void {
    if (!this.listening) return;
    this.listening = false;
    if (this.engine !== null) {
      try { this.engine.cancel(this.sessionId); } catch (_) {}
    }
    this.releaseCapturer();
    if (this.timeoutId !== -1) {
      clearTimeout(this.timeoutId);
      this.timeoutId = -1;
    }
    if (this.finishId !== -1) {
      clearTimeout(this.finishId);
      this.finishId = -1;
    }
  }

  // ★ 彻底销毁（页面卸载）：取消会话 + 归还系统引擎名额
  destroy(): void {
    this.cancel();
    if (this.engine !== null) {
      try { this.engine.shutdown(); } catch (_) {}
      this.engine = null;
      this.hotwordsKey = '';
    }
  }
}
