// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ VoiceCommandEngine（VoiceCommandEngine.ets）—— 离线语音识别会话引擎         ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】封装 CoreSpeechKit 离线中文识别（speechRecognizer，端侧 AI 模型）：  ║
// ║  · createEngine 三阶段回退建引擎（热词→无热词→裸配置），真机兼容性最大化     ║
// ║    （sysGeneralLexicon 热词 >60 条在部分真机抛 1002200001，超限截断）       ║
// ║  · AudioCapturer 16k/单声道/16bit PCM 采集 → 1280 字节分片喂 writeAudio     ║
// ║  · VAD 自动断句（vadEnd→finish）+ timeoutMs 超时兜底 + 中间结果实时回调     ║
// ║  · 会话级创建/释放麦克风（不长期占用，隐私指示灯不常亮）                     ║
// ║  · 引擎跨会话复用（hotwordsKey 语义键缓存）；sid 守卫隔离晚到回调           ║
// ║ 日志：默认仅 error 级 + 建引擎结果（DBG=false）；排查时改 true 看全链路     ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { speechRecognizer } from '@kit.CoreSpeechKit';
import { audio } from '@kit.AudioKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const LOG_DOMAIN: number = 0x00A0;
const LOG_TAG: string = 'VoiceCmd';

// ★ 调试日志总开关：默认关闭（仅保留 error 级 + 建引擎结果），排查时改 true
const DBG: boolean = false;

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
  // ★ stage1（sysGeneralLexicon）曾在该热词集上失败 → 后续同集直接跳过 stage1
  private stage1FailKey: string = '';
  // ★ 最近一次建引擎失败信息（供上层给出可读提示）
  private createFailCode: number = 0;
  private createFailMsg: string = '';
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
  private fedBytes: number = 0;
  private cb: VcSessionCallbacks | null = null;
  private timeoutId: number = -1;
  private finishId: number = -1;
  // ★ 回声抑制：TTS 播报期间置 true → 喂入麦克风音频全部丢弃（会话保活，
  //   识别器只收到静音），防止应用自己的播报被麦克风识别成命令
  private muted: boolean = false;

  // ★ 引擎懒创建（含热词注入）：三阶段回退，最大化真机兼容性
  //   A：locate+mode+纯中文热词（≤60 条，唤醒词优先；sysGeneralLexicon 超限/
  //       混入非汉字词条在部分真机抛 1002200001）
  //   B：locate+mode（无热词）
  //   C：裸配置（仅 language+online，任一 extraParams 都可能不被接受时兜底）
  //   无论落在哪个阶段，hotwordsKey 均按语义热词集缓存 → 引擎跨会话复用不重建
  private async ensureEngine(hotwords: string[]): Promise<boolean> {
    const key = hotwords.join('|');
    if (this.engine !== null && this.hotwordsKey === key) {
      if (DBG) {
        hilog.info(LOG_DOMAIN, LOG_TAG, 'reuse engine, hotwords=%{public}d', hotwords.length);
      }
      return true;
    }
    if (this.creatingEngine) {
      hilog.warn(LOG_DOMAIN, LOG_TAG, 'engine creating, reject parallel create');
      return false;
    }
    this.creatingEngine = true;
    if (this.engine !== null) {
      try { this.engine.shutdown(); } catch (_) {}
      this.engine = null;
    }
    // 阶段 A 的热词集合：仅保留纯汉字词条（2~20 字），唤醒词优先，≤60 条
    const cjk: string[] = [];
    for (const w of hotwords) {
      let pure = true;
      for (let i = 0; i < w.length; i++) {
        const code = w.charCodeAt(i);
        if (code < 0x4E00 || code > 0x9FFF) {
          pure = false;
          break;
        }
      }
      if (pure && w.length >= 2 && cjk.length < 60) {
        cjk.push(w);
      }
    }
    const cjkKey = cjk.join('|');
    if (DBG) {
      hilog.info(LOG_DOMAIN, LOG_TAG, 'createEngine begin, hotwords=%{public}d cjk=%{public}d',
        hotwords.length, cjk.length);
    }
    // ★ 该热词集曾令 stage1 失败 → 直接从 stage2 开始（省一次必败请求 + 700ms）
    let startStage = 1;
    if (this.stage1FailKey === cjkKey) {
      startStage = 2;
    }
    let lastErr: BusinessError | null = null;
    let okEngine: speechRecognizer.SpeechRecognitionEngine | null = null;
    let usedStage = 0;
    for (let stage = startStage; stage <= 3; stage++) {
      try {
        const params: speechRecognizer.CreateEngineParams = {
          language: 'zh-CN',
          online: 1
        };
        if (stage <= 2) {
          const extra: Record<string, Object> = { 'locate': 'CN', 'recognizerMode': 'short' };
          if (stage === 1 && cjk.length > 0) {
            extra['sysGeneralLexicon'] = cjk;
          }
          params.extraParams = extra;
        }
        if (DBG) {
          hilog.info(LOG_DOMAIN, LOG_TAG, 'createEngine stage %{public}d try', stage);
        }
        okEngine = await speechRecognizer.createEngine(params);
        usedStage = stage;
        break;
      } catch (e) {
        const err = e as BusinessError;
        lastErr = err;
        if (stage === 1) {
          this.stage1FailKey = cjkKey;
        }
        hilog.error(LOG_DOMAIN, LOG_TAG,
          'createEngine stage %{public}d failed: %{public}d [%{public}s]',
          stage, err.code, `${err.message}`);
      }
    }
    if (okEngine === null) {
      this.createFailCode = lastErr !== null ? lastErr.code : 0;
      this.createFailMsg = lastErr !== null ? `${lastErr.message}` : 'unknown';
      this.creatingEngine = false;
      return false;
    }
    this.engine = okEngine;
    this.hotwordsKey = key;
    if (usedStage === 1) {
      this.stage1FailKey = '';
    }
    this.createFailCode = 0;
    this.createFailMsg = '';
    hilog.info(LOG_DOMAIN, LOG_TAG, 'createEngine success at stage %{public}d', usedStage);
    this.creatingEngine = false;
    return true;
  }

  // ★ 是否正在聆听（UI 状态用）
  isListening(): boolean {
    return this.listening;
  }

  // ★ 启动一次识别会话：采集→识别→VAD 断句/超时→最终文本回调；
  //   timeoutMs 为无断句兜底时长（唤醒监听长、指令聆听短）
  async startSession(hotwords: string[], cb: VcSessionCallbacks,
    timeoutMs: number = 10000): Promise<boolean> {
    if (this.listening) return false;
    this.cb = cb;
    const ok = await this.ensureEngine(hotwords);
    if (!ok || this.engine === null) {
      const hint = this.createFailCode === 1002200001
        ? '语音识别服务不可用(1002200001)，请稍后重试或重启设备'
        : `语音引擎初始化失败(${this.createFailCode})`;
      cb.onError(hint);
      return false;
    }
    this.sessionSeq++;
    this.sessionId = `vcmd-${this.sessionSeq}`;
    this.finalText = '';
    this.gotFinal = false;
    this.vadEnded = false;
    this.fedBytes = 0;
    this.acc = [];
    if (DBG) {
      hilog.info(LOG_DOMAIN, LOG_TAG, 'startSession %{public}s', this.sessionId);
    }
    const engine = this.engine;
    const myId = this.sessionId;
    engine.setListener({
      onStart: (sid: string, msg: string): void => {
        if (DBG) {
          hilog.info(LOG_DOMAIN, LOG_TAG, 'onStart %{public}s %{public}s', sid, msg);
        }
      },
      onEvent: (sid: string, code: number, msg: string): void => {
        // ★ VAD 事件：1=检测到说话开始 3=说话结束
        if (DBG) {
          hilog.info(LOG_DOMAIN, LOG_TAG, 'onEvent %{public}s code=%{public}d %{public}s', sid, code, msg);
        }
        // ★ 晚到事件守卫：上层已在 onFinal 中开启下一会话时，旧会话的残余事件直接丢弃
        if (sid !== myId) return;
        if (code === 3 && !this.vadEnded) {
          this.vadEnded = true;
          this.scheduleFinish(200);
        }
      },
      onResult: (sid: string, result: speechRecognizer.SpeechRecognitionResult): void => {
        if (DBG) {
          hilog.info(LOG_DOMAIN, LOG_TAG,
            'onResult %{public}s isFinal=%{public}d isLast=%{public}d result=[%{public}s] fedBytes=%{public}d',
            sid, result.isFinal ? 1 : 0, result.isLast ? 1 : 0, result.result, this.fedBytes);
        }
        if (sid !== myId) return;
        if (result.result.length > 0) {
          this.finalText = result.result;
          if (!result.isLast && this.cb !== null) {
            this.cb.onPartial(result.result);
          }
        }
        if (result.isLast) {
          this.gotFinal = true;
          this.completeSession();
        }
      },
      onComplete: (sid: string, msg: string): void => {
        if (DBG) {
          hilog.info(LOG_DOMAIN, LOG_TAG, 'onComplete %{public}s %{public}s', sid, msg);
        }
        if (sid !== myId) return;
        this.completeSession();
      },
      onError: (sid: string, code: number, msg: string): void => {
        hilog.error(LOG_DOMAIN, LOG_TAG, 'onError %{public}s code=%{public}d %{public}s', sid, code, msg);
        if (sid !== myId) return;
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
      if (DBG) {
        hilog.info(LOG_DOMAIN, LOG_TAG, 'startListening ok');
      }
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(LOG_DOMAIN, LOG_TAG, 'startListening throw: %{public}d %{public}s', err.code, err.message);
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
      // ★ 先置位再启动采集，防止首个 readData 回调因 listening=false 被丢弃（首帧丢失竞态）
      this.listening = true;
      await this.capturer.start();
      if (DBG) {
        hilog.info(LOG_DOMAIN, LOG_TAG, 'capturer started');
      }
    } catch (e) {
      const err = e as BusinessError;
      hilog.error(LOG_DOMAIN, LOG_TAG, 'capturer failed: %{public}d %{public}s', err.code, err.message);
      this.failSession(`麦克风启动失败：${err.message}`);
      return false;
    }
    // ★ 无断句兜底：强制 finish 让引擎出最终结果
    this.timeoutId = setTimeout((): void => {
      if (DBG) {
        hilog.info(LOG_DOMAIN, LOG_TAG, 'session timeout, vadEnded=%{public}d fedBytes=%{public}d',
          this.vadEnded ? 1 : 0, this.fedBytes);
      }
      this.scheduleFinish(0);
    }, timeoutMs);
    return true;
  }

  // ★ 回声抑制开关（TTS 播报开始/结束时由上层切换）
  mute(on: boolean): void {
    this.muted = on;
  }

  // ★ PCM 分片缓冲：writeAudio 仅接受 640/1280 字节，攒够 1280 发一片
  private feedChunk(u: Uint8Array): void {
    if (!this.listening || this.muted || this.engine === null) return;
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
        this.fedBytes += 1280;
      } catch (e) {
        // 写入失败（会话已结束等）：静默丢帧，等待 complete 收尾
        if (DBG) {
          const err = e as BusinessError;
          hilog.warn(LOG_DOMAIN, LOG_TAG, 'writeAudio throw: %{public}d %{public}s', err.code, err.message);
        }
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
    if (DBG) {
      hilog.info(LOG_DOMAIN, LOG_TAG, 'session complete, final=[%{public}s]', this.finalText);
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
    if (this.cb !== null) {
      this.cb.onFinal(this.finalText);
    }
  }

  // ★ 会话失败：停麦、清定时器、上抛错误
  private failSession(msg: string): void {
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
    if (DBG) {
      hilog.info(LOG_DOMAIN, LOG_TAG, 'capturer released');
    }
  }

  // ★ 用户主动取消当前会话（再点麦克风/退后台）
  cancel(): void {
    if (!this.listening) return;
    this.listening = false;
    if (DBG) {
      hilog.info(LOG_DOMAIN, LOG_TAG, 'session cancelled');
    }
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
      this.stage1FailKey = '';
      if (DBG) {
        hilog.info(LOG_DOMAIN, LOG_TAG, 'engine destroyed');
      }
    }
  }
}
