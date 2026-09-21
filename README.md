# Valve Boot

Windows 开机登录前全屏播放视频。使用 **Credential Provider** 方案，在系统 Logo 转圈结束后、登录界面出现前直接播放视频，播完自动进入桌面。

## 功能特性

- 🎬 **登录前全屏播放**：登录界面出现前盖屏播放，无桌面闪现
- 📋 **多视频轮播**：顺序循环（默认）或随机播放，每次开机只播一个
- 🎵 **自动等音频就绪**：开机早期音频设备未就绪时自动等待并重试
- 🔄 **GitHub 自动同步**：定期检查仓库 `video` 文件夹，自动下载新增/变化的视频（按 Git SHA1 差异比对）
- ⚙️ **配置热修改**：编辑 `config.ini` 即可切换模式/间隔，无需重新编译或重装
- 🔇 **零干扰登录**：不提供任何登录凭据 tile，不影响正常登录流程

## 工作原理

| 组件 | 作用 |
|---|---|
| `ValveLogonVideo.dll` | Credential Provider，被 LogonUI 加载，在 Winlogon 桌面全屏播放视频 |
| `Valve Boot.exe` | 安装/卸载器（注册组件、关闭开机锁屏、配置开机自启） |
| `ValveVideoUpdater.exe` | 后台同步器，登录后启动，定期从 GitHub 拉取新视频 |

播放列表 = **[内嵌默认视频（恒为第一号）, `videos` 文件夹视频（按文件名排序）]**。

## 安装

1. 下载 `ValveBoot-Distribution.zip`，解压到任意目录（建议英文路径）
2. 双击 `Valve Boot.exe`：先全屏预览，然后自动提权完成安装
3. 重启电脑生效

卸载：运行 `Valve Boot.exe /uninstall`（恢复锁屏界面、移除组件与自启）。

## 配置（config.ini）

```ini
[Video]
Mode=sequential        ; 顺序循环（默认）| random 随机
VideoFolder=videos     ; 视频文件夹名（相对本目录）
HoldMs=25000           ; 最大阻塞毫秒（1000~60000）

[Update]
Enabled=1                  ; 1=启用 GitHub 自动同步
Repo=GarlicXP/Valve-Boot   ; GitHub 仓库（owner/repo）
RepoFolder=video           ; 仓库中存放视频的文件夹
IntervalMin=30             ; 检查间隔（分钟）
```

修改保存后下次开机/检查生效，无需重装。

## GitHub 视频自动同步

把视频上传到仓库的 `video` 文件夹，所有安装了本软件的机器会在下次检查时自动下载到本地 `videos`（只添加/更新，**从不删除**本地文件），下一轮开机生效。

## 视频格式

推荐 **WMV**（Windows 自带解码器最稳定）；也支持 MP4 / AVI / MKV / MOV / MPG。

## 构建

使用 Visual Studio 打开 `Valve Boot.slnx`，Release / x64 构建即可，产物输出到 `x64\Release\`。

## 注意事项

- 安装时会关闭系统锁屏界面（`NoLockScreen`），避免开机时壁纸时钟闪现；副作用是 Win+L 锁屏直接进登录界面，卸载后恢复
- 运行记录在 `sync.log`，可查看自动同步是否成功
