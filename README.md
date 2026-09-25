# DaaKuuLaa 的个人主页

基于毛玻璃卡片设计的个人主页，包含文件管理与项目管理两个入口。

## 在线访问

- 主站：https://daakuulaa.github.io/
- 自定义域名：https://dkl.cc.cd/

`CNAME` 已配置，DNS 托管在 Cloudflare。

## 本地预览

### 启动网页服务

```bash
python -m http.server 8000
```

浏览器打开 `http://localhost:8000`。

`file.html` 与 `work.html` 的上传、删除功能依赖已部署的上传 Worker。本地预览时若
`https://upload.dkl.cc.cd` 不可达，仅文件浏览与下载可用。

### 启动 PathManager

双击 `run_PathManager.bat`，或执行：

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
│   ├── explorer.css     # 文件管理与项目管理共享样式
│   ├── explorer.js      # 文件管理与项目管理共享逻辑
│   └── lunar.js         # 农历库
├── File/                # 文件管理目录
├── Work/                # 项目管理目录
├── file.json            # 文件管理器索引
├── work.json            # 项目管理工作索引
├── PathManager.py       # 文件与项目管理工具源码
├── run_http.bat         # 启动网页服务
├── run_PathManager.bat  # 启动 PathManager
├── worker/              # Cloudflare Worker，网页上传后端
│   ├── worker.js        # Worker 源码，处理上传并调用 GitHub API
│   └── wrangler.toml    # Cloudflare 部署配置
└── email/               # Cloudflare Email Worker，邮箱服务
    ├── src/index.js     # Worker 源码，收邮件、附件入库、Web 邮箱 API
    ├── wrangler.toml    # Cloudflare 部署配置，使用 D1 与 KV
    └── schema.sql       # D1 表结构
```

## 工具说明

### PathManager

管理 `file.json` 与 `work.json` 的图形化工具。

功能：

- 扫描 `File/` 或 `Work/` 目录，生成或更新索引 JSON
- 文件与目录的添加、删除、拖拽移动
- 合并多个项目为 index 类型
- 切换 work.json 与 file.json
- 发布管理器：上传压缩包发布或更新 GitHub Releases，并同步索引中的下载链接

快捷键：

- Ctrl 加点击：多选或取消选择
- Shift 加点击：范围选择
- Delete：删除选中项
- Esc：取消所有选择

打包：

```bash
pyinstaller --onefile --windowed --name PathManager PathManager.py
```

### 发布管理器

PathManager 工具栏的发布管理器直接对接 GitHub Releases，无需在网页手动上传资产。

- 新建工具：选择压缩包，确定分类与资产名，创建或复用对应 Release，上传资产，
  并把 `https://github.com/.../releases/download/<tag>/<资产名>` 写入索引
- 更新工具：从索引中已有的 Releases 条目选择一个，指定新版本压缩包，删除同名旧资产
  后以同名重新上传，下载链接与索引路径保持不变

分类目录与 Release tag 沿用仓库既有约定，资产名确定后长期不变：

| 索引目录 | Release tag | Release 标题 |
| --- | --- | --- |
| `File/Tool` | `tools` | 工具 Tools |
| `File/Game` | `games` | 游戏 Games |
| `File/Test` | `testdata` | 测试数据 |

Token 配置：需要一个对本仓库具有 `Contents: read/write` 权限的 PAT。读取顺序为环境变量
`GH_TOKEN`、`GITHUB_TOKEN`，其次为仓库根目录的 `.pathmanager_token`。对话框中的
「设置 Token…」会写入该文件，该文件已在 `.gitignore` 中。

发布只写入 Releases 资产与本地索引文件，不会自动提交 git。需要执行
`git add file.json && git commit && git push` 后网页才会更新。

### 网页上传与管理

文件管理器与项目管理工作支持在网页上直接上传、删除文件。后端为 Cloudflare Worker，
PAT 与密码哈希保存在 Worker Secret 中，源码不含敏感信息。

端点：

- `POST /upload` — 上传文件，multipart/form-data，字段为 `file`、`dir`、`subdirs`
- `POST /delete` — 删除文件，JSON body 为 `{ path, confirm: true }`

两个端点成功时都会在 `index` 字段返回最新的 `file.json` 或 `work.json` 完整对象，
前端直接用于重渲染与缓存，避免等待 GitHub Pages 镜像延迟。

部署域：默认 `workers.dev` 在国内不可达，Worker 已绑定自定义域 `https://upload.dkl.cc.cd`。
该域名需托管到 Cloudflare 才能绑定。

初次部署 `wrangler deploy` 后，把 `file.html` 与 `work.html` 中的 `uploadUrl` 改为该域名。

安全设计：

- PAT 与密码哈希保存在 Cloudflare Secret，不出现在源码与前端
- PAT 仅授予 `Contents: read/write`，无 Administration 与 Workflows 权限
- 同名文件自动加时间戳前缀，无同名则保留原名
- 单文件上限 50MB，超限请用 PathManager 本地添加
- 仅允许写入与删除 `File/`、`Work/`，路径在服务端强制校验
- 删除需输入密码并二次确认，Worker 强制要求 `confirm: true`
- 上传与删除后自动更新对应索引 JSON，带 sha 重试

修改上传密码：

```bash
echo -n "新密码" | sha256sum
wrangler secret put UPLOAD_PASSWORD_HASH
```

页面快捷键：

- `R` — 刷新当前目录，保留路径并重新拉取仓库 JSON
- 连续按三次 `R` — 清除本地缓存并从远端拉取
- `D` — 切换明暗主题
- `Backspace` — 返回上层
- `Esc` — 返回主页

本地缓存：页面加载时优先使用 localStorage 缓存渲染，后台用 `fetch` 拉取仓库 JSON，
仓库更新后覆盖缓存。缓存仅含 JSON 数据，不含密码与 Token。

### 邮箱服务

邮箱前后台由 `email/` 下的 Cloudflare Email Worker 承载，绑定 `mail.dkl.cc.cd`，
可在浏览器收发与管理邮件，支持登录、收件箱、搜索、多选、批量删除、一键清空、详情与附件。

依赖资源：

- D1 数据库 `daakuulaa-email`，表结构见 `schema.sql`，ID 已写入 `wrangler.toml`
- KV 命名空间 `SESSIONS`，用于登录会话
- Secret 通过 `wrangler secret put` 注入，不写入源码：`ADMIN_PASSWORD_HASH`、`GITHUB_PAT`
- 邮件路由：在 Cloudflare 邮箱路由把 `*@dkl.cc.cd` 转发到本 Worker

注意：当前设计会把邮件附件提交到公开仓库的 `Email/Attachments/` 目录，附件将公开发布，
仅适合存放非重要内容。正式使用前建议改为 R2 私有存储。

### 自定义域名

- 主页：`dkl.cc.cd`，仓库根 `CNAME` 已声明，DNS 托管在 Cloudflare
- 上传 Worker：`upload.dkl.cc.cd`
- 邮箱服务：`mail.dkl.cc.cd`

## 下载

较大的二进制文件统一放在 GitHub Releases，仓库内不再保存，下载链接保持稳定。

| 资源 | Release 页面 |
| --- | --- |
| 工具集：JiYuTool、Rdpwrap、RoomTool | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/tools |
| 游戏：wolf、demon_release | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/games |
| PathManager，Windows x64 | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/pathmanager |
| 测试数据：F.7z，约 130 MB | https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/tag/testdata |

文件管理器与项目管理工作页对这些条目直接给出 Releases 直链，格式为
`https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/releases/download/<tag>/<文件>`。
前端对非 `DaaKuuLaa.github.io/` 前缀的地址按外部直链处理，不做前缀改写与 ghproxy 代理。

## 技术栈

- HTML5、CSS3、原生 JavaScript
- 无构建工具，直接部署 GitHub Pages
- 农历库：lunar-javascript，本地内置

## 设计规范

- 毛玻璃卡片风格，使用 backdrop-filter: blur
- 明暗主题切换，按 `D` 键触发
- 响应式布局，支持移动端
- 文件列表键盘导航：Tab 切换项目，Enter 或 Space 激活，方向键导航

## 许可证

MIT
