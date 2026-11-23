// ★ 公共类型定义：ArkTS 层与 native（libentry.so）之间的"检测结果"数据契约。
//   数据流向：native YOLO 输出裸数组 → VisionController 组装成 Det/Merged
//   → Index.ets/HUD 消费（画框、计数、语音播报、自动录像触发）
// ★ 单个检测框：坐标为 0~1 归一化值（渲染时乘预览尺寸换算成像素）
export interface Det {
  label: number;
  name: string;
  score: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

// ★ Merged：按类别聚合后的统计行（HUD"鱼群统计"列表一行一个类别）
export interface Merged {
  label: number;
  name: string;
  count: number;
  avgScore: number;
}

// ★ CountInfo：聚合过程的中间计数结构（count/total 分开存，便于算平均分）
export interface CountInfo {
  count: number;
  total: number;
  name: string;
}