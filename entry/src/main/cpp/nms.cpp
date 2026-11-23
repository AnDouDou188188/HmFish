// ╔═══════════════════════════════════════════════════════════════════════════╗
// ║ NMS 实现（nms.cpp）—— 非极大值抑制                                            ║
// ╠═══════════════════════════════════════════════════════════════════════════╣
// ║ 【职责】YOLO 解码后的候选框去重：同一目标会被多个相邻 anchor 重复检出，        ║
// ║   保留分数最高者、抑制与它重叠的其余框，避免"一条鱼三四层框"。                ║
// ║                                                                                ║
// ║ 【算法】贪心：① 按分数降序排序 → ② 依次取未被抑制的最高分框入 keep →          ║
// ║   ③ 与其后同类别框算重叠度：IoU > iouThresh（常规重叠）或                     ║
// ║   IoM > 0.80（交集占小框比例，大框几乎包住小框）→ 抑制。                       ║
// ║   IoM 补丁解决纯 IoU 盲区：小框被大框包住时 IoU 分母是两者并集，               ║
// ║   数值可能仍低于阈值；鱼群贴近/遮挡时必须按小框自身面积占比判包含。            ║
// ║                                                                                ║
// ║ 【约束】只对同类别（label 相同）互相抑制——不同鱼种本就可能重叠共处。           ║
// ║ 【复杂度】O(n²)，n 为过阈值候选数（YOLOv8 top-1 解码后通常几十个，可忽略）     ║
// ╚═════════════════════════════════════════════════════════════════════════════╝
// ★ 非极大值抑制（NMS）：YOLO 检测框去重（IoU 阈值过滤重叠框）
#include "nms.h"
#include <algorithm>
std::vector<Detection> NMS(std::vector<Detection> &dets, float iouThresh) {
    // ① 按分数降序：NMS 的贪心基础——永远先确认"最像目标"的框，再让它压制次优框
    std::sort(dets.begin(), dets.end(),
              [](const Detection &a, const Detection &b) { return a.score > b.score; });

    std::vector<Detection> keep;
    // suppressed[i]=true 表示该框已被更优框压制；用标记数组而非边删边遍历，
    // 避免在 O(n²) 双重循环中反复搬移 vector
    std::vector<bool> suppressed(dets.size(), false);

    for (size_t i = 0; i < dets.size(); ++i) {
        if (suppressed[i]) continue;
        keep.push_back(dets[i]);

        float ax1 = dets[i].x, ay1 = dets[i].y;
        float ax2 = dets[i].x + dets[i].w, ay2 = dets[i].y + dets[i].h;
        float area1 = dets[i].w * dets[i].h;

        for (size_t j = i + 1; j < dets.size(); ++j) {
            if (suppressed[j]) continue;
            if (dets[i].label != dets[j].label) continue;

            float bx1 = dets[j].x, by1 = dets[j].y;
            float bx2 = dets[j].x + dets[j].w, by2 = dets[j].y + dets[j].h;
            float area2 = dets[j].w * dets[j].h;

            // 交集矩形：左上取两框 max、右下取两框 min；
            // 宽高再与 0 取 max，两框不相交时交集面积自然为 0
            float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
            float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
            float iw = std::max(0.f, ix2 - ix1), ih = std::max(0.f, iy2 - iy1);
            float inter = iw * ih;

            // IoU = 交/并（分母 +1e-6 防全等小框除零）；IoM = 交/小框面积，
            // 专治"小框被大框包住"——此时 IoU 分母是大并集、数值偏低压不掉
            float iou = inter / (area1 + area2 - inter + 1e-6f);
            float iom = inter / std::min(area1, area2);  // ★ 交集占小框比例

            // IoU 超阈值 或 大框包含小框 80% 以上 → 抑制；
            // 两条路径互补：常规重叠走 IoU，包含/遮挡走 IoM（鱼群贴近场景的关键）
            if (iou > iouThresh || iom > 0.80f) {
                suppressed[j] = true;
            }
        }
    }
    return keep;
}