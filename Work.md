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
