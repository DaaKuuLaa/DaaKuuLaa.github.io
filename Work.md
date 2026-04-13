# AI 工作规范

## 项目信息
- 项目路径：`a:\DKL\DaaKuuLaa.github.io\`
- 项目类型：个人主页网站
- GitHub 用户：DaaKuuLaa

## 设计规范

### 视觉设计
- 毛玻璃卡片风格（backdrop-filter: blur）
- 背景：纯色乳白 + 渐变
- 卡片圆角：20px
- 响应式布局：支持移动端

### 技术栈
- 纯 HTML/CSS/JavaScript
- 无需构建工具
- 直接部署到 GitHub Pages

### 数据来源
- GitHub API: `https://api.github.com/users/DaaKuuLaa`
- 自动获取：头像、用户名、统计数据

## 功能模块

### 已完成
1. 个人主页卡片（头像、用户名、签名、GitHub 统计）
2. 日期卡片（公历 + 农历）
3. 文件管理入口卡片
4. 其他功能卡片（博客、项目、联系、关于）

### 待开发
- [ ] 文件管理系统
- [ ] 博客系统
- [ ] 项目展示页
- [ ] 联系方式页面
- [ ] 关于页面

## 开发原则
1. 用户提出的大要求、规范必须记录到本文档
2. "要记录"这条要求本身也要记录
3. Work.md 专为 AI 设计，以 AI 效率为主
4. README.md 为用户设计，语言精炼，不标注 AI
5. README.md 不要包含：开发计划、License、卡片说明、本地预览、功能特性

## 快捷键配置
- **Esc**: 从任意界面返回主界面
- **Backspace**: 文件路径返回上一层

## 设计要求
- 顶部控制栏：包含功能按钮（如主题切换）
- 主题切换按钮：圆角矩形、毛玻璃样式
- 卡片布局：控制栏下方，居中显示

## 排版要求
- 全屏无滚动条，内容显示完整
- 自适应任意 16:9 分辨率
- 左侧个人卡片为正方形，宽度适中
- 右侧布局：
  - 上层：日历卡片（横向长条）
  - 下层：文件和工作卡片并排对半
- 排版美观、简洁
- **文件列表滚动条要求**：
  - 文件列表本身高度固定，内部可滚动
  - 外层网页不能有滚动条（硬性要求）
  - 不同分辨率下自适应，保持一致效果

## 配置文件格式

### file.json（文件管理器配置）
```json
{
  "name": "文件管理器",
  "description": "个人文件管理系统",
  "rootPath": "./File",
  "files": [
    {
      "name": "文件名",
      "path": "文件路径",
      "type": "file",
      "description": "文件描述"
    }
  ],
  "folders": [
    {
      "name": "文件夹名",
      "path": "文件夹路径",
      "type": "folder",
      "description": "文件夹描述"
    }
  ]
}
```

### work.json（项目管理工作配置）
```json
{
  "name": "项目管理工作",
  "description": "个人项目管理系统",
  "rootPath": "./Work",
  "projects": [
    {
      "name": "项目名",
      "path": "项目路径",
      "type": "project",
      "description": "项目描述"
    }
  ]
}
```

## 文件目录结构
- 文件管理器：`a:\DKL\DaaKuuLaa.github.io\File\`
- 项目管理工作：`a:\DKL\DaaKuuLaa.github.io\Work\`

## 更新日志
- 2026-04-08: 初始版本，毛玻璃卡片个人主页
- 2026-04-09: 添加文件管理器和项目管理工作页面，支持明暗主题切换
- 2026-04-11: 添加 PathManager 工具，用于快速生成 work.json 和 file.json

## PathManager 工具说明

### 功能特性
- **清空 JSON**：一键清空当前 JSON 文件（🗑️ 图标）
- **添加文件/文件夹**：支持多选，自动包含文件夹内容（➕ 图标）
- **自动扫描**：自动扫描目录生成 JSON（🔄 图标）
- **工作对象切换**：在 work.json 和 file.json 之间切换
- **目录预览**：类似文件资源管理器的树形结构
- **多选功能**：
  - Ctrl+ 点击：切换单个项目的选择状态
  - Shift+ 点击：选择范围内所有项目
- **删除功能**：支持 Delete 键和右键菜单删除
- **拖拽移动**：支持文件和文件夹的拖拽操作
- **合并为 index**：将文件合并为 index 类型

### 快捷键
- **Ctrl+ 点击**：多选/取消选择
- **Shift+ 点击**：范围选择
- **Delete**：删除选中项
- **Esc**：取消所有选择

### 打包说明
- 脚本文件：`PathManager.py`
- 可执行文件：`PathManager.exe`（位于主目录下）
- 打包命令：`pyinstaller --onefile --windowed --name PathManager PathManager.py`
- 生成的 exe 文件会自动复制到主目录下
