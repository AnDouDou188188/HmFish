// ★ NMS 头文件：Detection 去重接口声明
#ifndef NMS_H
#define NMS_H

#include <vector>

struct Detection {
    float x, y, w, h;   // 归一化到 [0,1]
    float score;
    int   label;
};

// 声明 NMS 函数
std::vector<Detection> NMS(std::vector<Detection> &dets, float iouThresh);

#endif // NMS_H