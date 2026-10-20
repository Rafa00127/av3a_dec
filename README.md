# av3a_dec

AVS3-P3（Audio Vivid，MP4 中 `av3a` 音轨）解码 DLL，供 MusicPlayer2 等播放器
通过动态加载使用。

- 外壳代码：`src/av3a_dec.c`（MP4 抽帧、12 声道下混、WAV/缓存/流式接口）
- 解码核心：`src/codec`，来自 AVS/UWA 的 `AVS3A_RMV4.0` 参考解码器
- 模型：不随仓库分发，获取方式见下文「模型」

## 构建

MSVC（推荐）：

```
msbuild av3a_dec.vcxproj /p:Configuration=Release /p:Platform=x64
```

产物：`build\x64\Release\av3a_dec.dll`。

w64devkit（MinGW）：

```
make
```

产物：当前目录 `av3a_dec.dll`。

## 模型（需自备，不随仓库分发）

DLL 会在自身目录依次查找 `model.bin`、`vivid_model.bin`，任选其一放入即可。
获取方式二选一：

1. AudioVivid 参考包（推荐）
   - UWA 下载页：https://www.theuwa.com/download/2?page=2
   - 或镜像包：https://github.com/6hzzz/AVS3stream 的 `TS/AudioVivid源码.7z`
   - 解压后位于 `AVS3A_Codec_linux/avs3RM0Decoder/model.bin`
     （或 `AVS3A_RMV4.0/AVS3A_RMV4.0/AVS3_Codec/avs3RM0Decoder/model.bin`）
2. 已安装的网易云客户端
   - `<安装目录>\resource\vivid_model.bin`
     （常见：`C:\Program Files (x86)\NetEase\CloudMusic\resource`、`E:\CloudMusic\resource`）
   - 可直接使用原名，或改名为 `model.bin`

该模型仅供本地使用，请勿再分发。

## 使用

把 `av3a_dec.dll` 和模型文件（`model.bin` 或 `vivid_model.bin`）放到播放器可执行文件同目录即可。导出的 C 接口：

```c
int            av3a_probe(const wchar_t* path);                 // 1 = av3a m4a
const wchar_t* av3a_prepare(const wchar_t* path);               // 返回可播放 WAV 路径
void           av3a_cleanup(void);                              // 清理临时文件
int            av3a_decode_wav(const wchar_t* path, void** data, unsigned* size);
void           av3a_free(void* data);
```

`av3a_prepare` 为流式实现：先返回一个边解码边写入的 WAV，调用方用 BASS 等
播放器打开即可；解码完成后会缓存到 `%LOCALAPPDATA%\av3a_cache`。

## 格式说明

- MP4 sample entry：`av3a`；裸流扩展名 `.av3a`
- 帧长固定 1024 样本，支持 7.1.4 等声道配置
- 文件可带 `dca3` 配置盒，也可只靠 ES 帧头（网易云下载的文件属于后者）

## 许可

`src/codec` 为 AVS/UWA 参考代码，按其头部许可**仅允许用于 AVS 标准开发、
测试与推广**，需保留版权声明；不属于 OSI 开源许可。完整文本见 [LICENSE](LICENSE)。
外壳代码与本文档由本项目作者编写。
