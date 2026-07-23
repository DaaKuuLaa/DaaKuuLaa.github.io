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
└── run_PathManager.bat  # 启动 PathManager
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
