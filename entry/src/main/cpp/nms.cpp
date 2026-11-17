// ★ 非极大值抑制（NMS）：YOLO 检测框去重（IoU 阈值过滤重叠框）
#include "nms.h"
#include <algorithm>
std::vector<Detection> NMS(std::vector<Detection> &dets, float iouThresh) {
    std::sort(dets.begin(), dets.end(),
              [](const Detection &a, const Detection &b) { return a.score > b.score; });

    std::vector<Detection> keep;
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

            float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
            float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
            float iw = std::max(0.f, ix2 - ix1), ih = std::max(0.f, iy2 - iy1);
            float inter = iw * ih;

            float iou = inter / (area1 + area2 - inter + 1e-6f);
            float iom = inter / std::min(area1, area2);  // ★ 交集占小框比例

            // IoU 超阈值 或 大框包含小框 80% 以上 → 抑制
            if (iou > iouThresh || iom > 0.80f) {
                suppressed[j] = true;
            }
        }
    }
    return keep;
}