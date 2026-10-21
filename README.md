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
int            av3a_decode_wav(const wchar_t* path, void** data, unsigned* size);
void           av3a_free(void* data);

void*          av3a_stream_open(const wchar_t* path);           // 边解边播
unsigned long long av3a_stream_length(void* h);                 // 虚拟文件长度
int            av3a_stream_read(void* h, void* buf, unsigned len, unsigned long long offset);
void           av3a_stream_close(void* h);
```

`av3a_stream_*` 是推荐用法：对外表现为一个完整的 WAV（44 字节头 + PCM），
内部只保留**播放点前后几秒的窗口**（默认 6 秒），按需解码；任意位置可以
立即 seek（跳到目标帧前 8 帧预滚后继续），与 BASS 的 `BASS_StreamCreateFileUser`
配合即可，无需临时文件、也不再需要整首缓冲。

立体声输出与网易云客户端 `ear::VIVIDRender` 实测一致：12 声道按
`g=0.85 / s=g/√2` 的固定矩阵下混（FL/FR 硬声像、C 与 LFE 满增益进两侧、
环绕与顶部 0.707g、ch4/ch5 以差信号反向进入），随后是 2147 样本前瞻的
峰值限幅器（阈值 ≈32760、释放 227ms、增益平滑 0.6）和硬钳位。

## 格式说明

- MP4 sample entry：`av3a`；裸流扩展名 `.av3a`
- 采样率与帧长从首帧头读取（参考实现帧长恒为 1024）；目前 7.1.4 下混已校准，
  5.1 / HOA 走同构近似
- 只支持普通 MP4（`stsc/stsz/stco|co64`）；fMP4（`moof/traf/trun`）暂不支持
- 文件可带 `dca3` 配置盒，也可只靠 ES 帧头（网易云下载的文件属于后者）

## 许可

`src/codec` 为 AVS/UWA 参考代码，按其头部许可**仅允许用于 AVS 标准开发、
测试与推广**，需保留版权声明；不属于 OSI 开源许可。完整文本见 [LICENSE](LICENSE)。
外壳代码与本文档由本项目作者编写。
