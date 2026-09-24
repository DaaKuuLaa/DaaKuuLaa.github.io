# DaaKuuLaa 的个人主页

一个基于毛玻璃卡片设计的个人主页网站，包含文件管理和项目管理工作入口。

## 在线访问

访问 [https://daakuulaa.github.io/](https://daakuulaa.github.io/)
或自定义域名 [https://dkl.cc.cd/](https://dkl.cc.cd/)（`CNAME` 已配置，DNS 托管在 Cloudflare）

## 本地预览

### 启动网页服务
```bash
python -m http.server 8000
```
然后在浏览器打开 `http://localhost:8000`

> 提示：`file.html` / `work.html` 的网页上传/删除功能依赖已部署的上传 Worker（`https://upload.dkl.cc.cd`，见下文）。本地预览时若该域名不可达，仅文件浏览/下载可用。

### 使用 PathManager 管理文件/项目
双击 `run_PathManager.bat` 打开图形工具，或：
```bash
pythonw.exe PathManager.py
```

## 项目结构

```
DaaKuuLaa.github.io/
├── index.html           # 主页
├── file.html            # 文件管理器
├── work.html            # 项目管理工作
├── assets/
│   ├── avatar.jpg       # 头像
│   ├── explorer.css     # 文件/项目管理工作共享样式
│   ├── explorer.js      # 文件/项目管理工作共享逻辑
│   └── lunar.js         # 农历库
├── File/                # 文件管理目录
├── Work/                # 项目管理目录
├── file.json            # 文件管理器配置
├── work.json            # 项目管理工作配置
├── PathManager.py       # 文件/项目管理工具（源码）
├── run_http.bat         # 启动网页服务
├── run_PathManager.bat  # 启动 PathManager
├── worker/              # Cloudflare Worker（网页上传后端）
│   ├── worker.js        # Worker 源码（处理上传、调 GitHub API）
│   └── wrangler.toml    # Cloudflare 部署配置
└── email/               # Cloudflare Email Worker（邮箱服务）
    ├── src/index.js     # Worker 源码（收邮件、附件入库、Web 邮箱 API）
    ├── wrangler.toml    # Cloudflare 部署配置（D1 + KV）
    └── schema.sql       # D1 表结构
```

## 工具说明

### PathManager
一个图形化工具，用于管理 `file.json` 和 `work.json` 的内容。

**功能：**
- 扫描 `File/` 或 `Work/` 目录，自动生成/更新 JSON 配置
- 文件/文件夹的添加、删除、拖拽移动
- 合并多个项目为 index 类型
- 切换 work.json / file.json

**快捷键：**
- Ctrl + 点击：多选/取消选择
- Shift + 点击：范围选择
- Delete：删除选中项
- Esc：取消所有选择

**打包：**
```bash
pyinstaller --onefile --windowed --name PathManager PathManager.py
```

### 网页上传与管理功能（Cloudflare Worker）

文件管理器和项目管理器支持在网页上直接上传/删除文件（右键菜单）。
后端使用 Cloudflare Worker，PAT 与密码哈希都保存在 Worker Secret 中，源码不含敏感信息。

**端点：**
- `POST /upload` — 上传文件（multipart/form-data，字段 `file`、`dir`、`subdirs`）
- `POST /delete` — 删除文件（JSON body：`{ path, confirm: true }`）

两个端点成功时都会在 `index` 字段返回最新的 `file.json` / `work.json` 完整对象，
前端直接用其重渲染与缓存，避免等 GitHub Pages 镜像延迟。

**部署域：** 默认 `workers.dev` 在国内不可达，Worker 已绑定自定义域
`https://upload.dkl.cc.cd`（域名需托管到 Cloudflare 才能绑定）。

初次部署 `wrangler deploy` 后将 `file.html` / `work.html` 中 `uploadUrl` 改为你的域名。

**安全设计：**
- PAT 与密码哈希保存在 Cloudflare Secret，不出现在任何源码或前端
- PAT 仅授予 `Contents: read/write`，无 Administration / Workflows 权限
- 同名文件自动加时间戳前缀避免覆盖，无同名则保留原名
- 单文件上限 50MB（超限请用 PathManager 本地添加）
- 仅可往 `File/` 或 `Work/` 写入与删除，路径在服务端强制校验，前端无法绕过
- 删除需输入密码 + 二次确认，Worker 强制要求 `confirm: true` 才执行
- 上传/删除后自动更新对应的 `file.json` / `work.json` 索引（带 sha 重试）

**修改上传密码：**
```bash
echo -n "新密码" | sha256sum   # 拿到新 hash
wrangler secret put UPLOAD_PASSWORD_HASH   # 重新注入
```

**快捷键：**
- `R` — 刷新当前目录（保留路径，重新拉取仓库 JSON）
- 连续点三下 `R` → 强制清缓存，从远端拉取（绕过本地缓存）
- `D` — 切换明暗主题
- `Backspace` — 返回上层
- `Esc` — 返回主页

**本地缓存策略：**
页面加载时优先用 localStorage 缓存渲染，后台用 `fetch` 拉取仓库 JSON；
若仓库更新则覆盖缓存。`R` ×3 清缓存强制同步。缓存仅含 JSON 数据，不含密码或 Token。

### 邮箱服务（Cloudflare Email Worker）

邮箱前后台由 `email/` 目录下的 Cloudflare Email Worker 承载，绑定 `mail.dkl.cc.cd`，
在浏览器即可收发/管理邮件（登录、收件箱、搜索、多选、批量删除、一键清空、详情、附件）。

**依赖资源：**
- D1 数据库 `daakuulaa-email`（表结构见 `schema.sql`，ID 已在 `wrangler.toml` 填好）
- KV 命名空间 `SESSIONS`（登录会话）
- Secret（`wrangler secret put` 注入，不写入源码）：`ADMIN_PASSWORD_HASH`、`GITHUB_PAT`
- 邮件路由：在 Cloudflare 邮箱路由把 `*@dkl.cc.cd` 转发到本 Worker

> ⚠️ 注意：当前设计会把邮件附件提交到公开仓库的 `Email/Attachments/` 目录，附件将公开发布；
> 仅适合存放非重要内容，正式使用前建议改为 R2 私有存储（见上文的改进计划）。

### 自定义域名

- 主页：`dkl.cc.cd`（仓库根 `CNAME` 已声明，DNS 托管在 Cloudflare）
- 上传 Worker：`upload.dkl.cc.cd`
- 邮箱服务：`mail.dkl.cc.cd`

## 下载（GitHub Releases）

较大的二进制文件统一放在 GitHub Releases，仓库内不再保存，下载链接保持稳定：

| 资源 | 页面 |
| --- | --- |
| 工具集（JiYuTool、Rdpwrap） | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/tools |
| 游戏（wolf、demon_release） | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/games |
| PathManager（Windows x64） | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/pathmanager |

文件管理器 / 项目管理工作页对这些条目直接给出 Releases 直链
（`https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/download/<tag>/<file>`），
前端对非 `DaaKuuLaa.github.io/` 前缀的地址按外部直链处理，不做前缀改写与 ghproxy 代理。

## 技术栈

- HTML5 / CSS3 / Vanilla JavaScript
- 无构建工具，直接部署 GitHub Pages
- 农历库：lunar-javascript（本地内置）

## 设计规范

- 毛玻璃卡片风格（backdrop-filter: blur）
- 明暗主题切换（按 D 键，`D` / `d` 均触发）
- 响应式布局（支持移动端）
- 文件列表：键盘导航（Tab 切换项目、Enter/Space 激活、方向键导航）

## 许可证

MIT
