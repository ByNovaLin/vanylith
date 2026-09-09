Vanylith v0.1.0 — Windows x64
================================

简介
----
Vanylith 是一个面向 Windows 的本地 TRON / TRX 靓号地址生成器，支持 CPU、
NVIDIA CUDA GPU、前缀、后缀、前后缀组合、重复字符规则和多设备选择。

地址生成、私钥生成与结果保存均在本机完成。默认无 Telemetry，不上传私钥。

快速使用
--------
1. 将 Vanylith-v0.1.0-win-x64.zip 解压到可写目录。
2. 双击 Vanylith.exe。
3. 配置 Pattern、Position、生成数量与计算设备。
4. 点击“开始搜索”。

系统要求
--------
- Windows 10 / 11 x64
- Microsoft Edge WebView2 Evergreen Runtime
- CUDA 模式需要兼容的 NVIDIA GPU 与 NVIDIA Driver

普通用户无需安装 CUDA Toolkit、nvcc、Visual Studio、CMake、Python、Node.js
或 PHP。

真机验证
--------
- RTX 3090 / sm_86：已测试，约 186 MKeys/s
- RTX 4090 / sm_89：已测试，约 277 MKeys/s
- RTX 5070 / sm_120：已测试，约 206 MKeys/s

以上数据仅供参考；实际性能受驱动、温度、功耗限制、后台负载和匹配规则影响。
程序包含 sm_80、sm_86、sm_89、sm_120 原生目标及 compute_120 PTX。
A100 / sm_80 target 已包含，但 v0.1.0 尚未在 A100 真机上完成正式验证。

正确性
------
v0.1.0 正确性测试：37 / 37 passed，0 failed。
每个 GPU Candidate 保存前必须经过 CPU libsecp256k1 独立复核和 Pattern 二次检查。

结果保存
--------
每次任务会在程序目录下创建独立目录。TRON 地址和 Private Key 默认写入：

  results\task-*\wallets.txt

wallets.txt 是敏感文件。Private Key 丢失后无法恢复，请安全备份，不要公开分享，
也不要将 wallets.txt 或 results 目录提交到 Git 仓库。

安全摘要
--------
- 随机数：Windows BCryptGenRandom
- 私钥范围：1 <= key < secp256k1 curve order
- GPU Candidate：CPU libsecp256k1 独立复核
- Private Key 不进入 Web API、SSE、Dashboard JavaScript 或普通日志
- 默认无 Telemetry、无私钥上传

完整安全说明见 SECURITY.md。

License
-------
Vanylith 使用 Vanylith Community Source License 1.0（VCSL 1.0），属于
Community Source / Source Available，不是 OSI 批准的 Open Source 许可证。
README 摘要不具权威性，如有冲突，以 LICENSE 为准。第三方组件适用各自许可证，
详见 THIRD_PARTY_NOTICES.md。

项目主页与下载
------------
https://github.com/ByNovaLin/vanylith
https://github.com/ByNovaLin/vanylith/releases
