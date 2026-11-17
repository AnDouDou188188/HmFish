// ★ 公共类型定义：检测框 Det、合并结果 Merged、计数信息 CountInfo
export interface Det {
  label: number;
  name: string;
  score: number;
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface Merged {
  label: number;
  name: string;
  count: number;
  avgScore: number;
}

export interface CountInfo {
  count: number;
  total: number;
  name: string;
}