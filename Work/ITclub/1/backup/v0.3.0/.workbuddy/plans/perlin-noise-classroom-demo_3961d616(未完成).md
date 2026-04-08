---
name: perlin-noise-classroom-demo
overview: 创建一个适合课堂大屏幕演示的柏林噪声算法HTML课件，简洁清晰，具有配合讲解的交互演示功能
design:
  architecture:
    framework: html
  styleKeywords:
    - 简洁清晰
    - 大屏幕优化
    - 高对比度
    - 功能导向
    - 教学演示
  fontSystem:
    fontFamily: "'Segoe UI', Arial, sans-serif"
    heading:
      size: 48px
      weight: 700
    subheading:
      size: 36px
      weight: 600
    body:
      size: 24px
      weight: 400
  colorSystem:
    primary:
      - "#2962ff"
      - "#00c853"
      - "#ff6d00"
    background:
      - "#1a1a2e"
      - "#16213e"
      - "#0f3460"
    text:
      - "#ffffff"
      - "#e0e0e0"
      - "#2962ff"
    functional:
      - "#00c853"
      - "#ff3d00"
      - "#ffab00"
      - "#2962ff"
todos:
  - id: create-project-structure
    content: 创建项目基础目录结构和HTML骨架
    status: pending
  - id: implement-navigation-control
    content: 实现导航控制区和演示进度管理
    status: pending
    dependencies:
      - create-project-structure
  - id: create-context-introduction
    content: 创建情境引入区和问题引出展示
    status: pending
    dependencies:
      - implement-navigation-control
  - id: implement-perlin-algorithm
    content: 实现柏林噪声算法核心模块
    status: pending
    dependencies:
      - create-project-structure
  - id: build-canvas-demo
    content: 构建Canvas可视化演示模块
    status: pending
    dependencies:
      - implement-perlin-algorithm
  - id: create-algorithm-demo
    content: 创建四步算法原理演示区
    status: pending
    dependencies:
      - build-canvas-demo
      - create-context-introduction
  - id: implement-parameter-control
    content: 实现参数调整区和实时预览
    status: pending
    dependencies:
      - build-canvas-demo
  - id: create-comparison-zone
    content: 创建对比展示区和应用案例区
    status: pending
    dependencies:
      - implement-parameter-control
  - id: test-classroom-display
    content: 使用[skill:browser-automation]进行大屏幕显示测试和优化
    status: pending
    dependencies:
      - create-comparison-zone
---

## 产品概述

创建一个用于课堂大屏幕演示的柏林噪声算法HTML课件，供教师在课堂上配合讲解使用。页面包含情境引入、算法原理可视化演示和参数调整功能。

## 核心功能

1. **情境引入区**：通过游戏地形生成的实际问题，引出柏林噪声算法的需求
2. **肯·柏林简介区**：简短介绍算法发明者肯·柏林的背景、成就和荣誉
3. **算法原理演示区**：四步算法流程的可视化展示，支持逐步演示
4. **参数调整演示区**：简单明了的控制面板，教师可调整参数实时观察效果
5. **对比展示区**：柏林噪声与白噪声的直观对比，突出算法优势
6. **应用案例区**：Minecraft地形、云朵生成等实际应用案例展示

## 技术栈选择

- **前端框架**：原生HTML5 + CSS3 + JavaScript（ES6+）
- **可视化库**：Canvas 2D API用于噪声图像绘制
- **交互控制**：简单的滑块和按钮控制
- **响应式设计**：CSS Grid + Flexbox布局，确保大屏幕显示清晰
- **动画效果**：CSS3过渡动画和JavaScript演示控制

## 实现方案

### 整体架构

采用单页面演示结构，包含以下核心模块：

1. **导航控制区**：演示进度控制（前进/后退/重置）
2. **情境引入区**：问题引出和情境展示
3. **人物简介区**：肯·柏林简要介绍
4. **原理演示区**：四步算法流程的Canvas可视化
5. **参数调整区**：频率、振幅、倍频三个核心参数控制
6. **对比展示区**：柏林噪声与白噪声的实时对比
7. **应用案例区**：实际应用效果展示

### 关键实现细节

1. **柏林噪声算法实现**：

- 实现标准的Perlin噪声算法
- 支持一维、二维噪声生成
- 包含梯度向量生成、插值计算等核心步骤

2. **Canvas可视化**：

- 实时绘制噪声图像（256x256像素）
- 支持灰度显示和彩色地形显示
- 算法步骤的可视化分解

3. **演示控制逻辑**：

- 前进/后退按钮控制演示步骤
- 当前步骤高亮显示
- 演示状态自动保存

4. **参数调整系统**：

- 频率控制：0.01-0.2范围
- 振幅控制：0-1范围
- 倍频控制：1-5倍频
- 实时参数更新和Canvas重绘

5. **大屏幕优化**：

- 大字体设计（最小24px）
- 高对比度颜色方案
- 清晰的分区布局
- 操作按钮足够大，便于点击

### 性能优化

- Canvas离屏渲染缓存
- 参数调整防抖处理（300ms延迟）
- 简化绘制算法，避免复杂计算
- 预生成常用参数组合的噪声图

## 目录结构

```
a:/Work/
├── index.html                    # [NEW] 主HTML文件，包含完整的演示页面结构
├── style.css                     # [NEW] 主样式文件，包含大屏幕优化的CSS样式
├── script.js                     # [NEW] 主JavaScript文件，包含页面控制和交互逻辑
├── perlin-noise.js               # [NEW] 柏林噪声算法核心实现
├── canvas-demo.js                # [NEW] Canvas绘图和可视化模块
├── demo-controller.js            # [NEW] 演示控制逻辑模块
├── assets/
│   └── images/
│       ├── ken-perlin.jpg        # [NEW] 肯·柏林照片（占位图）
│       ├── minecraft-terrain.jpg # [NEW] Minecraft地形示例
│       └── cloud-texture.jpg     # [NEW] 云朵纹理示例
└── lib/
    └── simple-slider.js          # [NEW] 简单的滑块组件
```

## 页面详细规划

### 1. 导航控制区 (Navigation Control)

- 左侧：演示标题"柏林噪声算法演示"
- 中间：演示进度指示器（1-4步）
- 右侧：控制按钮（上一步/下一步/重置/暂停）

### 2. 情境引入区 (Context Introduction)

- 标题："问题：如何生成自然的游戏地形？"
- 左侧：传统随机数生成的问题展示（像素化、不连续）
- 右侧：柏林噪声的解决方案预览
- 底部：问题总结和引出算法

### 3. 人物简介区 (Ken Perlin Introduction)

- 左侧：肯·柏林照片和简要介绍
- 右侧：成就时间线（1983发明算法/1997奥斯卡奖）
- 底部：算法命名由来和意义

### 4. 原理演示区 (Algorithm Demonstration)

- 四步Canvas可视化：

1. **网格划分**：显示网格点和坐标
2. **梯度向量**：显示每个网格点的随机向量
3. **点乘计算**：显示计算过程和结果
4. **插值平滑**：显示插值过程和最终噪声值

- 每一步都有详细说明和公式展示

### 5. 参数调整区 (Parameter Adjustment)

- 三个核心参数滑块：

1. **频率(Frequency)**：控制噪声的"粗糙度"
2. **振幅(Amplitude)**：控制噪声的"高度"
3. **倍频(Octaves)**：控制细节层次

- 实时Canvas预览
- 参数值实时显示

### 6. 对比展示区 (Comparison Zone)

- 左侧Canvas：柏林噪声效果
- 右侧Canvas：白噪声效果
- 中间：对比说明和切换按钮
- 底部：优缺点对比表格

### 7. 应用案例区 (Application Cases)

- Minecraft地形生成
- 自然云朵模拟
- 程序化纹理生成
- 每个案例都有简要说明和效果展示

### 8. 总结区 (Conclusion)

- 算法核心思想总结
- 实际应用价值
- 进一步学习资源

## 设计风格

采用简洁明了的大屏幕演示风格，确保在教室投影仪上清晰可见。界面以功能为导向，突出内容展示，避免花哨的装饰效果。

### 整体视觉风格

- **布局结构**：清晰的线性布局，从上到下依次展示内容模块
- **色彩系统**：深色背景（#1a1a2e）搭配浅色文字（#ffffff），确保高对比度
- **字体设计**：大号字体，标题使用36-48px，正文使用24-28px
- **分区标识**：每个模块有明确的标题和边界，便于教师定位

### 核心设计元素

1. **导航栏**：固定在顶部，包含演示标题和进度控制
2. **内容卡片**：每个模块使用卡片式设计，有轻微阴影和圆角
3. **控制面板**：简洁的滑块和按钮，使用明显的视觉反馈
4. **Canvas区域**：占据主要位置，确保图像清晰可见
5. **说明文字**：简洁明了的解释，避免大段文字

### 交互设计原则

1. **操作直观**：所有控制元素都有清晰的标签和反馈
2. **演示流畅**：步骤切换有平滑的过渡动画
3. **状态明确**：当前演示步骤有明显的高亮显示
4. **错误预防**：重要操作有确认提示

### 大屏幕优化

1. **字体大小**：确保教室最后一排也能看清文字
2. **对比度**：使用高对比度颜色组合
3. **间距**：足够的元素间距，避免拥挤
4. **焦点引导**：使用颜色和大小引导观众注意力

## 代理扩展

### SubAgent

- **code-explorer**
- 目的：在项目创建过程中探索目录结构，确保文件组织合理
- 预期成果：确认所有需要的文件都已正确创建，目录结构清晰

### Skill

- **browser-automation**
- 目的：在开发完成后进行大屏幕显示测试，验证页面在投影场景下的可读性
- 预期成果：确保页面在大屏幕上显示清晰，所有交互功能正常工作