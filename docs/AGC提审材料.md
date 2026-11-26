# 华为应用市场（AGC）提审材料 — 我爱钓鱼

> 包名 `com.fish.ayf` · 应用名「我爱钓鱼」· 版本 v2.2（versionCode 2020000）
> 发布签名配置 `default_release` 已就绪（证书已生成，等发布 Profile `.p7b`）

## 一、AGC 控制台操作（需要你自己做，浏览器登录华为开发者账号）

1. **创建应用**：[AppGallery Connect](https://developer.huawei.com/consumer/cn/service/josp/agc/index.html) → 「我的项目」→ 新建项目（或选已有项目）→「添加应用」：
   - 平台：**HarmonyOS**
   - 应用名称：**我爱钓鱼**
   - 软件包名：**com.fish.ayf**（必须完全一致，否则 Profile 申请不了）
   - 设备：手机 / 平板
2. **申请发布 Profile**：「用户与访问」→「证书、APP ID 与 Profile」→「Profile」页签 →「新增」：
   - 类型：**发布（release）**
   - 应用：我爱钓鱼（com.fish.ayf）
   - 证书：选择已自动生成的发布证书（`..._release=.cer` 对应的那张，工具已帮你上传申请）
3. Profile 受理通过后**下载 `.p7b`**，保存到（文件名必须一致，路径已写入 build-profile.json5）：
   ```
   C:\Users\18882\.ohos\config\default_HmFish_lWdgIrvpCfNZdjbt7WVCDNWru7xwKJlPe5qhZY_release=.p7b
   ```
4. 把 `.p7b` 放好后告诉 AI，执行上架包构建：
   ```
   devecocli build --build-mode release --assemble-type assembleApp --signing-config default_release
   ```
   产物：`entry/build/default/outputs/default/entry-default-signed.app`（AGC 上传的就是它）
5. AGC「我的项目 → 应用 → 版本管理」创建版本 → 上传 `.app` → 填写下方材料 → 提交审核。

## 二、开启 GitHub Pages（隐私政策 URL 必须）

仓库 Settings → Pages → Source 选 **Deploy from a branch** → 分支 `main` / 目录 **/docs** → Save。
约 1 分钟后生效，隐私政策 URL（提审时填这个）：

```
https://andoudou188188.github.io/HmFish/privacy.html
```

## 三、提审填写材料（直接复制）

### 基本信息
| 字段 | 填写内容 |
|---|---|
| 应用名称 | 我爱钓鱼 |
| 一句话简介 | AI 智能探鱼相机：鱼种识别、人脸分析、语音控制 |
| 分类 | 拍摄美化（备选：实用工具） |
| 是否免费 | 免费 |
| 年龄分级 | 4+（无不良内容；含人脸分析但仅本地展示） |
| 隐私政策 URL | `https://andoudou188188.github.io/HmFish/privacy.html` |
| 应用截图 | `docs/screenshots/` 下 4 张任选（main_hud / yolo_detect / face_wireframe / face_detail），要求 2–5 张 |
| 应用图标 | 已内置于应用（$media:ic_logo），无需另传 |

### 应用介绍（版本介绍/详细描述）
我爱钓鱼是一款 AI 智能探鱼相机，专为钓鱼爱好者打造，全部 AI 能力离线运行、无需联网、不收集任何个人数据。

主要功能：
- AI 鱼种识别：内置 21 种淡水鱼检测模型与 80 类通用目标检测模型，一键切换，实时识别框 + 计数 + 语音播报"识别到草鱼3条"；
- 多相机接入：支持 USB 探鱼器摄像头与手机前后置，捏合变焦，断连自动恢复；
- 语音命令：说"开始录像""打开人脸识别""变焦两倍"即可控制全部功能，识别不准会自动纠正并多轮追问确认；
- 人脸识别分析：检测框 + 性别/年龄/颜值实时展示（全部本地处理，不保存不上传）；
- OCR 文字/表格识别：拍照识别文字与表格结构，一键复制；
- AI 画质增强：超分辨率、降噪、水下去浊、色彩还原四模块独立调节；
- 画中画后台识别：退到桌面后预览与识别在悬浮小窗继续，录像播报不中断；
- 完整拍摄能力：录像、拍照、定时录像、运动检测自动录像、识别到目标自动录像、自定义触发音效；
- 更多：水深估算、运动检测、能耗档位、应用内自动更新。

钓鱼记录好帮手，探鱼、分析、拍摄一次搞定。

### 新版本特性（v2.2）
- 画中画后台识别：Home 键退出后，预览 + 识别在悬浮小窗继续，录像/播报/自动录像不中断；
- 应用内自动更新：每小时检查新版本，断点续传下载，一键安装；
- 离线语音命令：全功能语音控制，同音字自动纠正，支持多轮对话澄清确认；
- 修复与稳定性优化。

### 权限用途说明（审核员问询口径）
| 权限 | 用途 |
|---|---|
| 相机 | 实时预览与 AI 目标检测的视频源（核心功能） |
| 麦克风 | ① 录像时录制环境声音（用户可关闭声音录制）② 语音命令离线识别（需用户在设置中主动开启） |
| 位置 | HUD 显示当前地名、为照片/录像标记拍摄位置 |
| 网络 | 仅用于检查/下载 GitHub 上的应用更新 |

### 审核注意事项
- 人脸识别属敏感能力：隐私政策第五条已明确"端侧推理、不保存、不上传、不做身份识别"，审核通常关注这点；
- 麦克风是 user_grant 权限，module.json5 已配置 reason 与 usedScene；
- 首次提审版本号 v2.2 / versionCode 2020000 没有问题（应用市场只要求新版本高于旧版本）；
- 若审核被拒，常见原因：隐私政策 URL 无法访问（先确认 GitHub Pages 已生效）、截图含真实人脸（用 yolo_detect / main_hud 更稳妥）。
