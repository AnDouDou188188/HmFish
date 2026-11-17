// ★ AVRecorder 录像封装：prepare(fd)→start/stop、自动停录定时器、
//   最近录像产物路径保留（自动录像完成后存相册用）
import { media } from '@kit.MediaKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { common } from '@kit.AbilityKit';
import { BusinessError } from '@kit.BasicServicesKit';

export class RecorderService {
  private recorder: media.AVRecorder | null = null;
  private outputFd: number = -1;
  private outputPath: string = '';
  // ★ 最近一段录像产物路径：stop/cleanup 后保留（下次 prepare 覆盖），
  //   供自动录像完成后保存到相册使用
  private videoPath: string = '';
  private isRecording: boolean = false;
  private autoStopTimer: number = -1;
  // ★ 自动停录时长：prepare 记下，start() 成功后再起定时器——
  //   从真正开始录像计时（编码器 flush 耗时不再计入），设 5s 录 5s
  private autoStopDurationMs: number = 0;
  onAutoStop?: () => void;
  // ★ 自动停录前回调：native 先摘掉录像直供 surface、UI 立即停表，再停编码器
  onBeforeAutoStop?: () => void;

  async prepare(context: common.UIAbilityContext,
    durationMs: number,
    withAudio: boolean,
    videoWidth: number = 640,
    videoHeight: number = 480): Promise<string | null> {
    try {
      this.outputPath = context.filesDir + `/record_${Date.now()}.mp4`;
      const file = fs.openSync(this.outputPath,
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      this.outputFd = file.fd;
      this.videoPath = this.outputPath;

      this.recorder = await media.createAVRecorder();
      this.autoStopDurationMs = durationMs > 0 ? durationMs : 0;

      const profile: media.AVRecorderProfile = {
        fileFormat: media.ContainerFormatType.CFT_MPEG_4,
        videoBitrate: 2000000,
        videoCodec: media.CodecMimeType.VIDEO_AVC,
        videoFrameWidth: videoWidth,
        videoFrameHeight: videoHeight,
        videoFrameRate: 30,
        audioBitrate: 48000,
        audioChannels: 1,
        audioCodec: media.CodecMimeType.AUDIO_AAC,
        audioSampleRate: 48000
      };

      const config: media.AVRecorderConfig = {
        videoSourceType: media.VideoSourceType.VIDEO_SOURCE_TYPE_SURFACE_YUV,
        profile: profile,
        url: `fd://${this.outputFd}`
      };
      if (withAudio) {
        config.audioSourceType = media.AudioSourceType.AUDIO_SOURCE_TYPE_MIC;
      }

      console.info(`[Recorder] prepare: audio=${withAudio}, ${videoWidth}x${videoHeight}, path=${this.outputPath}`);
      await this.recorder.prepare(config);
      const surfaceId = await this.recorder.getInputSurface();
      console.info(`[Recorder] surfaceId=${surfaceId}`);
      return surfaceId;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[Recorder] prepare FAILED code=${err.code} msg=${err.message}`);
      this.cleanup();
      return null;
    }
  }

  async start(): Promise<boolean> {
    if (!this.recorder) return false;
    try {
      await this.recorder.start();
      this.isRecording = true;
      // ★ 定时器挪到 start 成功之后：录制时长从首帧真正开始处计算
      if (this.autoStopDurationMs > 0) {
        const durationMs = this.autoStopDurationMs;
        this.autoStopTimer = setTimeout(() => {
          if (this.onBeforeAutoStop) this.onBeforeAutoStop();
          this.stop().then(() => { if (this.onAutoStop) this.onAutoStop(); });
        }, durationMs);
      }
      return true;
    } catch (e) {
      const err = e as BusinessError;
      console.error(`[Recorder] start FAILED code=${err.code} msg=${err.message}`);
      this.cleanup();
      return false;
    }
  }

  async stop(): Promise<string> {
    if (this.autoStopTimer >= 0) {
      clearTimeout(this.autoStopTimer);
      this.autoStopTimer = -1;
    }
    const saved = this.outputPath;
    try {
      if (this.recorder && this.isRecording) await this.recorder.stop();
      if (this.recorder) await this.recorder.release();
    } catch (_) {}
    this.cleanup();
    return saved;
  }

  private cleanup(): void {
    this.recorder = null;
    this.isRecording = false;
    if (this.outputFd >= 0) {
      try { fs.closeSync(this.outputFd); } catch (_) {}
      this.outputFd = -1;
    }
  }

  get recording(): boolean { return this.isRecording; }

  /** 最近一段录像的沙箱路径（stop 后仍有效，直到下一次 prepare） */
  getVideoPath(): string { return this.videoPath; }
}