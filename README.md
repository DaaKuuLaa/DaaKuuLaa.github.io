# DaaKuuLaa 的个人主页

一个基于毛玻璃卡片设计的个人主页网站，包含文件管理和项目管理工作入口。

## 在线访问

访问 [https://daakuulaa.github.io/](https://daakuulaa.github.io/)

## 本地预览

### 启动网页服务
```bash
python -m http.server 8000
```
然后在浏览器打开 `http://localhost:8000`

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
├── PathManager.exe      # 文件/项目管理工具（可执行文件）
├── run_http.bat         # 启动网页服务
├── run_PathManager.bat  # 启动 PathManager
└── worker/              # Cloudflare Worker（网页上传后端）
    ├── worker.js        # Worker 源码（处理上传、调 GitHub API）
    └── wrangler.toml    # Cloudflare 部署配置
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

## 技术栈

- HTML5 / CSS3 / Vanilla JavaScript
- 无构建工具，直接部署 GitHub Pages
- 农历库：lunar-javascript（本地内置）

## 设计规范

- 毛玻璃卡片风格（backdrop-filter: blur）
- 明暗主题切换（D 或 D 键切换）
- 响应式布局（支持移动端）
- 文件列表：键盘导航（Tab 切换项目、Enter/Space 激活、方向键导航）

## 许可证

MIT
