// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ Pinyin（Pinyin.ets）—— 汉字→无声调拼音静态表（语音 NLU 同音纠错底座）        ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】为 VoiceNlu 提供逐字拼音转换：识别结果与命令词库分别转成拼音串后     ║
// ║ 再做编辑距离比对，即可把"录象/鹿象"这类同音/近音误识别纠正回"录像"。        ║
// ║ 表外字符退化为原字符（相似度优雅降级为纯字形比对，不致误纠）。               ║
// ╚═══════════════════════════════════════════════════════════════════════════╝

// ★ 汉字→无声调拼音映射（多音字取最常用读音）
const PINYIN_TABLE: Record<string, string> = {
  // ── 功能动作词 ──
  '打': 'da', '开': 'kai', '关': 'guan', '闭': 'bi', '停': 'ting', '止': 'zhi',
  '启': 'qi', '动': 'dong', '取': 'qu', '消': 'xiao', '撤': 'che', '恢': 'hui',
  '复': 'fu', '助': 'zhu', '继': 'ji', '续': 'xu', '暂': 'zan', '结': 'jie',
  '束': 'shu', '完': 'wan', '成': 'cheng', '帮': 'bang', '请': 'qing',
  '识': 'shi', '别': 'bie', '检': 'jian', '测': 'ce', '目': 'mu', '标': 'biao',
  '录': 'lu', '像': 'xiang', '影': 'ying', '拍': 'pai', '摄': 'she', '照': 'zhao',
  '截': 'jie', '图': 'tu', '相': 'xiang', '册': 'ce', '画': 'hua', '面': 'mian',
  '质': 'zhi', '量': 'liang', '增': 'zeng', '强': 'qiang', '超': 'chao',
  '分': 'fen', '辨': 'bian', '率': 'lv', '人': 'ren', '脸': 'lian', '框': 'kuang',
  '线': 'xian', '显': 'xian', '示': 'shi', '语': 'yu', '音': 'yin', '播': 'bo',
  '报': 'bao', '朗': 'lang', '读': 'du', '中': 'zhong', '自': 'zi', '运': 'yun',
  '水': 'shui', '深': 'shen', '度': 'du', '浅': 'qian', '文': 'wen', '字': 'zi',
  '表': 'biao', '格': 'ge', '性': 'xing', '年': 'nian', '龄': 'ling', '值': 'zhi',
  '灯': 'deng', '闪': 'shan', '光': 'guang', '电': 'dian', '手': 'shou', '筒': 'tong',
  '后': 'hou', '置': 'zhi', '头': 'tou', '外': 'wai', '接': 'jie', '设': 'she',
  '能': 'neng', '耗': 'hao', '省': 'sheng', '平': 'ping', '衡': 'heng',
  '准': 'zhun', '模': 'mo', '式': 'shi', '档': 'dang', '位': 'wei', '信': 'xin',
  '阈': 'yu', '型': 'xing', '切': 'qie', '换': 'huan', '使': 'shi', '变': 'bian',
  '焦': 'jiao', '放': 'fang', '大': 'da', '缩': 'suo', '小': 'xiao', '旋': 'xuan',
  '转': 'zhuan', '横': 'heng', '竖': 'shu', '倍': 'bei', '秒': 'miao',
  '更': 'geng', '新': 'xin', '查': 'cha', '跳': 'tiao', '过': 'guo', '稍': 'shao',
  '提': 'ti', '醒': 'xing', '安': 'an', '装': 'zhuang', '下': 'xia', '载': 'zai',
  '发': 'fa', '布': 'bu', '页': 'ye', '声': 'sheng', '效': 'xiao', '果': 'guo',
  '触': 'chu', '试': 'shi', '听': 'ting', '默': 'mo', '认': 'ren', '保': 'bao',
  '存': 'cun', '版': 'ben',
  // ── 调节词 ──
  '调': 'tiao', '到': 'dao', '为': 'wei', '至': 'zhi', '高': 'gao', '低': 'di',
  '多': 'duo', '少': 'shao', '数': 'shu', '清': 'qing', '晰': 'xi', '糊': 'hu',
  '亮': 'liang', '锐': 'rui', '对': 'dui', '饱': 'bao', '和': 'he', '降': 'jiang',
  '噪': 'zao', '彩': 'cai', '色': 'se', '修': 'xiu', '还': 'hai', '原': 'yuan',
  '智': 'zhi', '工': 'gong', '美': 'mei', '颜': 'yan',
  // ── 对话/泛用词 ──
  '的': 'de', '了': 'le', '是': 'shi', '我': 'wo', '你': 'ni', '他': 'ta',
  '给': 'gei', '把': 'ba', '让': 'rang', '那': 'na', '这': 'zhe', '个': 'ge',
  '一': 'yi', '二': 'er', '两': 'liang', '三': 'san', '四': 'si', '五': 'wu',
  '六': 'liu', '七': 'qi', '八': 'ba', '九': 'jiu', '十': 'shi', '百': 'bai',
  '千': 'qian', '万': 'wan', '零': 'ling', '半': 'ban', '点': 'dian', '之': 'zhi',
  '好': 'hao', '想': 'xiang', '要': 'yao', '没': 'mei', '懂': 'dong', '都': 'dou',
  '什': 'shen', '么': 'me', '怎': 'zen', '样': 'yang', '用': 'yong', '现': 'xian',
  '在': 'zai', '当': 'dang', '前': 'qian', '状': 'zhuang', '态': 'tai',
  '说': 'shuo', '话': 'hua', '命': 'ming', '令': 'ling', '控': 'kong', '制': 'zhi',
  '悬': 'xuan', '浮': 'fu', '窗': 'chuang', '全': 'quan', '屏': 'ping',
  '返': 'fan', '回': 'hui', '主': 'zhu', '菜': 'cai', '单': 'dan', '板': 'ban',
  '弹': 'tan', '收': 'shou', '起': 'qi', '来': 'lai', '掉': 'diao', '再': 'zai',
  '次': 'ci', '遍': 'bian', '重': 'chong', '明': 'ming', '白': 'bai',
  '第': 'di', '选': 'xuan', '择': 'ze', '算': 'suan', '嗯': 'en', '呃': 'e',
  '哦': 'o', '呀': 'ya', '吧': 'ba', '啦': 'la', '呢': 'ne', '啊': 'a',
  '麻': 'ma', '烦': 'fan', '拉': 'la', '近': 'jin', '远': 'yuan', '推': 'tui',
  '看': 'kan', '可': 'ke', '以': 'yi', '会': 'hui', '叫': 'jiao',
  '做': 'zuo', '鸟': 'niao', '虫': 'chong', '蝶': 'die', '蜂': 'feng', '蝉': 'chan',
  // ── 鱼类/物种（模型名与识别类别热词）──
  '鱼': 'yu', '草': 'cao', '鲫': 'ji', '鲤': 'li', '鲢': 'lian', '鳙': 'yong',
  '青': 'qing', '黑': 'hei', '鲶': 'nian', '泥': 'ni', '鳅': 'qiu', '鳝': 'shan',
  '甲': 'jia', '龟': 'gui', '鳖': 'bie', '乌': 'wu', '蟹': 'xie', '螃': 'pang',
  '虾': 'xia', '龙': 'long', '螺': 'luo', '蛙': 'wa', '鹰': 'ying', '燕': 'yan',
  '雀': 'que', '鸽': 'ge', '鹊': 'que', '猫': 'mao', '狗': 'gou', '马': 'ma',
  '牛': 'niu', '羊': 'yang', '鹿': 'lu', '兔': 'tu', '虎': 'hu', '狮': 'shi',
  '猴': 'hou', '熊': 'xiong', '鸡': 'ji', '鸭': 'ya', '鹅': 'e', '猪': 'zhu',
  '母': 'mu', '海': 'hai', '豚': 'tun', '鲸': 'jing', '鲨': 'sha', '兽': 'shou'
};

// ★ 方言口音归一化（西南官话/四川话常见系统性偏移，比对双方同时归一后
//   口音差异即被抵消）：
//   · 平翘舌不分：zh→z、ch→c、sh→s
//   · 鼻边音不分：声母 n→l
//   · 前后鼻音不分：ang→an、eng→en、ing→in（iang/uang/iu? 随子串一并归一）
function dialectSyllable(p: string): string {
  let s = p;
  if (s.startsWith('zh')) {
    s = 'z' + s.substring(2);
  } else if (s.startsWith('ch')) {
    s = 'c' + s.substring(2);
  } else if (s.startsWith('sh')) {
    s = 's' + s.substring(2);
  }
  if (s.startsWith('n')) {
    s = 'l' + s.substring(1);
  }
  if (s.includes('ang')) {
    s = s.replace('ang', 'an');
  }
  if (s.includes('eng')) {
    s = s.replace('eng', 'en');
  }
  if (s.includes('ing')) {
    s = s.replace('ing', 'in');
  }
  return s;
}

// ★ 逐字转拼音串（表外字符原样保留 → 相似度退化为字形比对）；
//   dialect=true 时按西南官话归一（口音容错），比对双方需同为 dialect
export function pinyinOf(text: string, dialect: boolean = false): string {
  let out = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    const py = PINYIN_TABLE[ch];
    if (py !== undefined) {
      out += dialect ? dialectSyllable(py) : py;
    } else {
      out += ch.toLowerCase();
    }
  }
  return out;
}
