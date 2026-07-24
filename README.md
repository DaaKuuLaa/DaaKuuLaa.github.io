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

### 网页上传功能（Cloudflare Worker）

文件管理器和项目管理工作都支持在网页上直接上传文件到当前的目录（右键 → 添加文件）。
后端使用 Cloudflare Worker，PAT 与密码哈希都保存在 Worker Secret 中，源码不含敏感信息。

**初次部署：**

1. 安装 Wrangler（一次性）：
```bash
npm install -g wrangler
wrangler login   # 浏览器授权 Cloudflare
```

2. 在 `worker/` 目录下部署：
```bash
cd worker
wrangler deploy
```
部署后得到形如 `https://daakuulaa-upload.<你的子域>.workers.dev` 的地址。

3. 注入两个 Secret：

```bash
# GitHub 细粒度 PAT（仅本仓库 Contents: read/write + Metadata: read）
wrangler secret put GITHUB_PAT

# 上传密码的 SHA-256 hex（小写）
# 生成方式：echo -n "你的密码" | sha256sum
wrangler secret put UPLOAD_PASSWORD_HASH
```

4. 把得到的 Worker URL 填入 `file.html` 和 `work.html` 中的
   `EXPLORER_CONFIG.uploadUrl`。

**安全设计：**
- PAT 与密码哈希保存在 Cloudflare Secret，不出现在任何源码或前端
- PAT 仅授予 `Contents: read/write`，无 Administration / Workflows 权限
- 文件名加时间戳前缀，永不覆盖现有文件
- 单文件上限 50MB（超限请用 PathManager 本地添加）
- 仅可往 `File/` 或 `Work/` 写入，路径在服务端强制，前端无法绕过
- 上传后自动在对应的 `file.json` / `work.json` 追加索引条目（带 sha 重试）

**修改上传密码：**
```bash
echo -n "新密码" | sha256sum   # 拿到新 hash
wrangler secret put UPLOAD_PASSWORD_HASH   # 重新注入
```

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
