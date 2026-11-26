// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ VoiceNlu（VoiceNlu.ets）—— 语音命令概率式 NLU 引擎（模糊纠错/候选排序）     ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【设计】不做精确关键词匹配，而是类 NLU 的多候选概率打分：                    ║
// ║  1. 归一化：全角转半角、去标点/语气词/客套填充词、英文小写                  ║
// ║  2. 槽位提取：中文/阿拉伯数字解析（"两倍""百分之六十""30秒"）               ║
// ║  3. 极性检测：开/关/停 等意图词决定目标动作方向（无则=切换）                 ║
// ║  4. 模糊打分：命令词库逐条与识别文本做滑动窗口比对，                       ║
// ║     字形编辑距离(0.45) + 拼音编辑距离(0.55) 双通道加权                      ║
// ║     → "录象/鹿象"拼音同 lu xiang 自动高置信纠正为"录像"                    ║
// ║  5. 候选排序：特异性加权（长词优先），输出 top-N 供对话层                   ║
// ║     执行(≥0.75) / 澄清二选一(候选接近) / 确认(0.52~0.75) / 没听懂(<0.52)    ║
// ╚═══════════════════════════════════════════════════════════════════════════╝
import { pinyinOf } from './Pinyin';

// ★ 意图极性：无/开/关/切换
export enum VcPolarity {
  NONE = 0,
  ON = 1,
  OFF = 2,
  TOGGLE = 3
}

// ★ 命令词条：action 目标动作 + phrase 触发词 + param 动态参数（模型 id 等）
export class VcEntry {
  action: string;
  phrase: string;
  param: string;
  constructor(action: string, phrase: string, param: string) {
    this.action = action;
    this.phrase = phrase;
    this.param = param;
  }
}

// ★ 候选意图：NLU 打分后的输出（含数值槽位与动态参数）
export class VcCandidate {
  action: string = '';
  phrase: string = '';
  polarity: VcPolarity = VcPolarity.TOGGLE;
  score: number = 0;
  numParam: number = -1;
  strParam: string = '';
}

// ★ 分析结果：候选列表（按分数降序）+ 全局极性 + 清洗文本 + 数值
export class VcAnalysis {
  candidates: VcCandidate[] = [];
  polarity: VcPolarity = VcPolarity.NONE;
  cleaned: string = '';
  number: number = -1;
}

// ★ 开/关意图词（长词优先匹配，匹配后从文本摘除再比对目标词）
const ON_WORDS: string[] = ['打开一下', '打开', '开启', '启用', '启动', '开始', '开起', '开一下', '开'];
const OFF_WORDS: string[] = ['关闭掉', '关闭', '关掉', '停止', '停用', '取消', '停下', '关了', '停了', '关'];

// ★ 填充词：识别文本中的客套/语气成分，比对前剔除（不影响语义）
const FILLERS: string[] = [
  '麻烦', '请', '帮我', '帮忙', '给我', '我想', '我要', '需要', '能不能',
  '可不可以', '能不能够', '能不能', '能不能帮', '那个', '这个', '一下', '快点',
  '立即', '马上', '嗯', '呃', '然后', '呀', '嘛', '吧', '啦', '呢', '啊', '哦'
];

// ★ 静态命令词库（action ↔ 触发词）；动态词条（模型名）由调用方注入
function buildEntries(): VcEntry[] {
  const e: VcEntry[] = [];
  const add = (action: string, phrases: string[]): void => {
    for (const p of phrases) {
      e.push(new VcEntry(action, p, ''));
    }
  };
  // ── 可开关目标（配合极性：打开/关闭/切换）──
  add('yolo', ['识别', '目标检测', '探鱼', '鱼群识别', '鱼探']);
  add('ocr', ['文字识别', '识别文字', '文字']);
  add('face', ['人脸识别', '人脸', '颜值']);
  add('depth', ['水深检测', '水深', '深度', '深浅']);
  add('motion', ['运动检测', '动态检测', '运动']);
  add('boxes', ['识别框', '检测框', '框线']);
  add('speech', ['语音播报', '播报', '语音']);
  add('detect_speak', ['识别朗读', '朗读']);
  add('pip', ['画中画', '悬浮窗', '小窗']);
  add('auto_record', ['自动录像', '自动记录']);
  add('sound', ['提示音', '触发音效', '音效']);
  add('enhance', ['画质增强', '增强画质', 'AI增强', '高清增强', '超分']);
  add('record', ['录像', '录影', '拍摄']);
  // ── 方向性动作（极性无意义，直接执行）──
  add('snapshot', ['截图', '拍照', '拍一张', '来一张', '拍张照']);
  add('camera_front', ['前置摄像头', '前摄像头', '前摄', '前置']);
  add('camera_back', ['后置摄像头', '后摄像头', '后摄', '后置']);
  add('camera_usb', ['USB摄像头', '外接摄像头', '外接相机', 'USB相机']);
  add('camera_switch', ['切换摄像头', '换个摄像头', '换摄像头', '翻转摄像头', '翻转']);
  add('zoom_in', ['放大', '拉近', '变焦放大']);
  add('zoom_out', ['缩小', '拉远']);
  add('zoom_set', ['变焦', '倍焦']);
  add('rotate', ['旋转画面', '旋转', '转一下', '横竖切换']);
  add('model_next', ['切换模型', '换个模型', '下一个模型', '换模型']);
  add('power_saver', ['省电模式', '节能模式', '省电']);
  add('power_std', ['标准模式', '平衡模式', '默认模式']);
  add('power_perf', ['性能模式', '高性能模式']);
  add('conf_set', ['置信度', '阈值']);
  add('duration_set', ['录像时长', '时长']);
  add('volume_set', ['音量']);
  add('sheet_model', ['识别设置', '识别菜单']);
  add('sheet_picture', ['画面设置', '画质设置', '画面菜单']);
  add('sheet_power', ['能耗设置', '能耗面板', '能耗菜单']);
  add('sheet_record', ['记录设置', '录像设置']);
  add('sheet_update', ['更新面板', '更新设置']);
  add('close_sheet', ['关闭面板', '关闭设置', '关闭菜单', '收起面板', '关闭弹窗']);
  add('update_check', ['检查更新', '看看有没有新版本', '有没有新版本']);
  add('help', ['你能做什么', '你能干什么', '你会什么', '有什么功能', '怎么用', '帮助']);
  add('undo', ['撤销刚才', '取消刚才', '恢复刚才', '撤销', '反悔']);
  add('status', ['现在什么状态', '当前状态', '什么状态', '状态']);
  add('cancel', ['算了', '不用了']);
  return e;
}

// ★ 中文数字字符表（含两=2）
const CN_NUM: Record<string, number> = {
  '零': 0, '一': 1, '二': 2, '两': 2, '三': 3, '四': 4, '五': 5,
  '六': 6, '七': 7, '八': 8, '九': 9, '十': 10, '百': 100
};

// ★ Damerau-Levenshtein 编辑距离（字符级，含相邻交换）
function editDistance(a: string, b: string): number {
  const la = a.length;
  const lb = b.length;
  if (la === 0) return lb;
  if (lb === 0) return la;
  let prev2: number[] = [];
  let prev: number[] = [];
  let cur: number[] = [];
  for (let j = 0; j <= lb; j++) {
    prev.push(j);
  }
  for (let i = 1; i <= la; i++) {
    cur = [];
    cur.push(i);
    for (let j = 1; j <= lb; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      let v = prev[j] + 1;
      if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
      if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
      if (i > 1 && j > 1 && a[i - 1] === b[j - 2] && a[i - 2] === b[j - 1]) {
        if (prev2[j - 2] + 1 < v) v = prev2[j - 2] + 1;
      }
      cur.push(v);
    }
    prev2 = prev;
    prev = cur;
  }
  return cur[lb];
}

// ★ 相似度：1 - 编辑距离/较长串长度（0~1）
function similarity(a: string, b: string): number {
  const m = Math.max(a.length, b.length);
  if (m === 0) return 1;
  return 1 - editDistance(a, b) / m;
}

export class VoiceNlu {
  // ★ 静态词库（进程级一份）
  static readonly ENTRIES: VcEntry[] = buildEntries();

  // ★ 归一化：全角→半角、去空白标点、英文小写、去填充词
  static normalize(text: string): string {
    let t = '';
    for (let i = 0; i < text.length; i++) {
      const code = text.charCodeAt(i);
      let ch = text[i];
      if (code === 0x3000) {
        ch = ' ';
      } else if (code > 0xFF00 && code < 0xFF5F) {
        ch = String.fromCharCode(code - 0xFEE0);
      }
      t += ch;
    }
    t = t.toLowerCase();
    // 去标点/空白（保留字母数字汉字）
    let kept = '';
    for (let i = 0; i < t.length; i++) {
      const c = t[i];
      const code = t.charCodeAt(i);
      const isCjk = code >= 0x4E00 && code <= 0x9FFF;
      const isAlnum = (code >= 48 && code <= 57) || (code >= 97 && code <= 122);
      if (isCjk || isAlnum) {
        kept += c;
      }
    }
    // 去填充词（长词优先，反复剔除直到稳定）
    let changed = true;
    while (changed) {
      changed = false;
      for (const f of FILLERS) {
        const idx = kept.indexOf(f);
        if (idx >= 0) {
          kept = kept.substring(0, idx) + kept.substring(idx + f.length);
          changed = true;
        }
      }
    }
    return kept;
  }

  // ★ 中文数字解析（"六十"→60，"三"→3，"一百二"→120；纯阿拉伯数字直接转）
  static parseCnNumber(s: string): number {
    const trimmed = s.trim();
    if (trimmed.length === 0) return -1;
    let arabic = '';
    let hasArabic = false;
    for (let i = 0; i < trimmed.length; i++) {
      const code = trimmed.charCodeAt(i);
      if (code >= 48 && code <= 57 || trimmed[i] === '.') {
        arabic += trimmed[i];
        hasArabic = true;
      }
    }
    if (hasArabic) {
      const v = parseFloat(arabic);
      return isNaN(v) ? -1 : v;
    }
    let total = 0;
    let cur = 0;
    let any = false;
    for (let i = 0; i < trimmed.length; i++) {
      const v = CN_NUM[trimmed[i]];
      if (v === undefined) continue;
      any = true;
      if (v < 10) {
        cur = v;
      } else if (v === 10) {
        total += (cur === 0 ? 1 : cur) * 10;
        cur = 0;
      } else if (v === 100) {
        total += (cur === 0 ? 1 : cur) * 100;
        cur = 0;
      }
    }
    if (!any) return -1;
    return total + cur;
  }

  // ★ 数值槽位提取：找第一段数字（阿拉伯或中文），"百分之"前缀先剔除
  static extractNumber(norm: string): number {
    let t = norm.replace('百分之', '');
    // 阿拉伯数字段
    for (let i = 0; i < t.length; i++) {
      const code = t.charCodeAt(i);
      if (code >= 48 && code <= 57) {
        let j = i;
        let s = '';
        while (j < t.length) {
          const c2 = t.charCodeAt(j);
          if (c2 >= 48 && c2 <= 57 || t[j] === '.') {
            s += t[j];
            j++;
          } else {
            break;
          }
        }
        const v = parseFloat(s);
        if (!isNaN(v)) return v;
        i = j;
      }
    }
    // 中文数字段
    let start = -1;
    for (let i = 0; i < t.length; i++) {
      const isNum = CN_NUM[t[i]] !== undefined;
      if (isNum && start < 0) {
        start = i;
      } else if (!isNum && start >= 0) {
        return VoiceNlu.parseCnNumber(t.substring(start, i));
      }
    }
    if (start >= 0) {
      return VoiceNlu.parseCnNumber(t.substring(start));
    }
    return -1;
  }

  // ★ 意图极性检测：返回极性并从文本摘除意图词（长词优先）
  static detectPolarity(norm: string, out: VcAnalysis): string {
    let t = norm;
    let pol = VcPolarity.NONE;
    const scan = (words: string[], p: VcPolarity): boolean => {
      const sorted = words.slice().sort((a: string, b: string): number => b.length - a.length);
      for (const w of sorted) {
        const idx = t.indexOf(w);
        if (idx >= 0) {
          t = t.substring(0, idx) + t.substring(idx + w.length);
          pol = p;
          return true;
        }
      }
      return false;
    };
    scan(OFF_WORDS, VcPolarity.OFF);
    scan(ON_WORDS, VcPolarity.ON);
    out.polarity = pol;
    return t;
  }

  // ★ 单词条对文本的模糊得分：精确子串=1，否则滑动窗口取
  //   max(0.45×字形相似 + 0.55×拼音相似)，拼音通道主导同音纠错
  static phraseScore(text: string, phrase: string): number {
    if (phrase.length === 0 || text.length === 0) return 0;
    if (text.includes(phrase)) return 1;
    const pyText = pinyinOf(text);
    const pyPhrase = pinyinOf(phrase);
    const L = phrase.length;
    let best = 0;
    if (text.length <= L) {
      const cs = similarity(phrase, text);
      const ps = similarity(pyPhrase, pyText);
      best = 0.45 * cs + 0.55 * ps;
    } else {
      for (let i = 0; i + L <= text.length; i++) {
        const win = text.substring(i, i + L);
        const cs = similarity(phrase, win);
        const ps = similarity(pyPhrase, pinyinOf(win));
        const s = 0.45 * cs + 0.55 * ps;
        if (s > best) best = s;
      }
    }
    // 特异性加权：长词（更具体的意图）小幅加分，避免"识别"抢走"识别设置"
    const bonus = L >= 5 ? 0.06 : (L >= 4 ? 0.05 : (L >= 3 ? 0.03 : 0.01));
    return Math.min(1, best + bonus);
  }

  // ★ 核心：分析识别文本 → 候选意图排序
  static analyze(text: string, dynamic: VcEntry[]): VcAnalysis {
    const a = new VcAnalysis();
    const norm = VoiceNlu.normalize(text);
    if (norm.length === 0) {
      a.cleaned = '';
      return a;
    }
    a.number = VoiceNlu.extractNumber(norm);
    const stripped = VoiceNlu.detectPolarity(norm, a);
    a.cleaned = stripped;
    const all: VcEntry[] = [];
    for (const e of VoiceNlu.ENTRIES) {
      all.push(e);
    }
    for (const e of dynamic) {
      all.push(e);
    }
    const map: Record<string, VcCandidate> = {};
    const keys: string[] = [];
    const push = (action: string, phrase: string, score: number, param: string): void => {
      const key = `${action}_${param}`;
      const old = map[key];
      if (old === undefined) {
        keys.push(key);
      }
      if (old === undefined || score > old.score) {
        const c = new VcCandidate();
        c.action = action;
        c.phrase = phrase;
        c.score = score;
        c.numParam = a.number;
        c.strParam = param;
        if (a.polarity === VcPolarity.NONE) {
          c.polarity = VcPolarity.TOGGLE;
        } else {
          c.polarity = a.polarity;
        }
        map[key] = c;
      }
    };
    // 1) 意图词摘除后的文本（"打开人脸识别"→"人脸识别"）为主比对
    for (const e of all) {
      const s = VoiceNlu.phraseScore(stripped, e.phrase);
      if (s >= 0.5) {
        push(e.action, e.phrase, s, e.param);
      }
    }
    // 2) 原文兜底（覆盖"开始录像"整词命中，意图词未被摘除干净的场景）
    if (stripped !== norm) {
      for (const e of all) {
        const s = VoiceNlu.phraseScore(norm, e.phrase);
        const key = `${e.action}_${e.param}`;
        const old = map[key];
        if (s >= 0.5 && (old === undefined || s > old.score)) {
          push(e.action, e.phrase, s, e.param);
        }
      }
    }
    const list: VcCandidate[] = [];
    for (const k of keys) {
      const c = map[k];
      if (c !== undefined) {
        list.push(c);
      }
    }
    list.sort((x: VcCandidate, y: VcCandidate): number => y.score - x.score);
    // 数值槽位意图提权：有数字 + 单位关键词 → 直接高置信
    if (a.number >= 0) {
      let numAction = '';
      if (norm.includes('倍')) {
        numAction = 'zoom_set';
      } else if (norm.includes('秒')) {
        numAction = 'duration_set';
      } else if (norm.includes('置信度') || norm.includes('阈值')) {
        numAction = 'conf_set';
      } else if (norm.includes('音量')) {
        numAction = 'volume_set';
      }
      if (numAction.length > 0) {
        const c = new VcCandidate();
        c.action = numAction;
        c.phrase = numAction;
        c.score = 0.95;
        c.numParam = a.number;
        c.polarity = VcPolarity.NONE;
        list.unshift(c);
      }
    }
    a.candidates = list;
    return a;
  }

  // ★ 澄清对话选项解析：返回 -9 未识别 / -1 拒绝 / -3 肯定 / 0..2 选项序号
  static optionIndex(text: string): number {
    const t = VoiceNlu.normalize(text);
    if (t.length === 0) return -9;
    if (t.includes('都') && t.includes('不')) return -2;
    if (t.includes('不是') || t === '不' || t.includes('不对') || t.includes('算了')
      || t.includes('取消') || t.includes('不用') || t.includes('不要')) {
      return -1;
    }
    if (t.includes('第一') || t.includes('1') || t === '一') return 0;
    if (t.includes('第二') || t.includes('2') || t === '二') return 1;
    if (t.includes('第三') || t.includes('3') || t === '三') return 2;
    if (t === '是' || t.includes('是的') || t.includes('对的') || t === '对'
      || t.includes('好的') || t.includes('嗯') || t.includes('好') || t.includes('要')) {
      return -3;
    }
    return -9;
  }

  // ★ 热词收集：静态词库 + 动态词条，喂给离线识别引擎提升命令词命中率
  static collectHotwords(dynamic: VcEntry[]): string[] {
    const seen: Record<string, boolean> = {};
    const out: string[] = [];
    const add = (w: string): void => {
      if (w.length < 2 || w.length > 20) return;
      if (seen[w] !== undefined) return;
      seen[w] = true;
      out.push(w);
    };
    for (const e of VoiceNlu.ENTRIES) {
      add(e.phrase);
    }
    for (const e of dynamic) {
      add(e.phrase);
    }
    if (out.length > 200) {
      return out.slice(0, 200);
    }
    return out;
  }
}
