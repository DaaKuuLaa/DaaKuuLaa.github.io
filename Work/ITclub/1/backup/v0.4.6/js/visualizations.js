// ==========================================
// 可视化模块
// Visualizations Module
// ==========================================

class VisualizationManager {
  constructor() {
    this.canvases = new Map();
    this.animations = new Map();
    this.currentModule = 1;
    this.currentLanguage = 'zh'; // 默认中文
    this.initPermutation();
    this.init();
  }

  init() {
    this.cacheCanvasElements();
    this.bindEvents();
  }

  cacheCanvasElements() {
    // 缓存所有canvas元素
    const canvasElements = document.querySelectorAll('canvas');
    canvasElements.forEach(canvas => {
      this.canvases.set(canvas.id, canvas);
    });
  }

  bindEvents() {
    // 监听模块切换事件
    document.addEventListener('moduleShown', (e) => {
      this.currentModule = e.detail.moduleId;
      this.initModuleVisualizations(this.currentModule);
    });

    // 监听窗口大小变化
    document.addEventListener('windowResized', (e) => {
      this.handleResize(e.detail.moduleId);
    });
    
    // 监听语言切换事件，重绘 Canvas 文字
    document.addEventListener('languageChanged', (e) => {
      this.currentLanguage = e.detail.language;
      this.redrawCanvasText();
    });
  }

  // 初始化模块可视化
  initModuleVisualizations(moduleId) {
    // 停止所有动画
    this.stopAllAnimations();

    switch (moduleId) {
      case 1:
        this.initModule1();
        break;
      case 2:
        this.initModule2();
        break;
      case 3:
        this.initModule3();
        break;
      case 4:
        this.initModule4();
        break;
      case 5:
        this.initModule5();
        break;
      case 6:
        this.initModule6();
        break;
      case 7:
        this.initModule7();
        break;
    }
  }

  // 模块1：问题引入
  initModule1() {
    this.drawWhiteNoise();
    this.drawDesiredTerrain();
    // 绘制问题分析页的三个可视化
    this.drawProblemVisualizations();
  }

  // 绘制三个问题可视化
  drawProblemVisualizations() {
    this.drawDiscontinuous();
    this.drawUnnatural();
    this.drawUncontrollable();
  }

  // 问题1：缺乏连续性 - 完全离散的点，无关联
  drawDiscontinuous() {
    const canvas = this.canvases.get('vizDiscontinuous');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 绘制完全离散的点，不连接任何线条
    ctx.fillStyle = '#ff5252';
    const points = 20;

    for (let i = 0; i < points; i++) {
      const x = Math.random() * width;
      const y = Math.random() * height;
      const size = Math.random() * 4 + 2;
      
      ctx.beginPath();
      ctx.arc(x, y, size, 0, Math.PI * 2);
      ctx.fill();
      
      // 添加轻微的透明度变化增强离散感
      ctx.globalAlpha = Math.random() * 0.5 + 0.5;
      ctx.fill();
      ctx.globalAlpha = 1;
    }
  }

  // 问题2：不够自然 - 块状像素
  drawUnnatural() {
    const canvas = this.canvases.get('vizUnnatural');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 绘制块状噪声（每个块内颜色一致）
    const blockSize = 12;
    const cols = Math.ceil(width / blockSize);
    const rows = Math.ceil(height / blockSize);

    for (let row = 0; row < rows; row++) {
      for (let col = 0; col < cols; col++) {
        const value = Math.random();
        const color = Math.floor(value * 150 + 50);
        ctx.fillStyle = `rgb(${color}, ${color}, ${color + 30})`;
        ctx.fillRect(col * blockSize, row * blockSize, blockSize - 1, blockSize - 1);
      }
    }
  }

  // 问题3：难以控制 - 无法生成特定地形
  drawUncontrollable() {
    const canvas = this.canvases.get('vizUncontrollable');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 绘制混合的随机地形（无法区分山脉、平原等）
    const blockW = 20;
    const blockH = 15;
    const cols = Math.ceil(width / blockW);
    const rows = Math.ceil(height / blockH);

    const colors = [
      '#4a6fa5', // 水域蓝
      '#6b8e4e', // 草地绿
      '#8b7355', // 土地棕
      '#9e9e9e', // 岩石灰
      '#ffffff'  // 雪山白
    ];

    for (let row = 0; row < rows; row++) {
      for (let col = 0; col < cols; col++) {
        const rand = Math.random();
        let colorIdx;
        if (rand < 0.2) colorIdx = 0;
        else if (rand < 0.5) colorIdx = 1;
        else if (rand < 0.7) colorIdx = 2;
        else if (rand < 0.9) colorIdx = 3;
        else colorIdx = 4;

        ctx.fillStyle = colors[colorIdx];
        ctx.fillRect(col * blockW, row * blockH, blockW - 1, blockH - 1);
      }
    }

    // 添加"?"标识表示无法控制
    ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
    ctx.font = 'bold 24px Arial';
    ctx.textAlign = 'center';
    ctx.fillText('?', width / 2, height / 2 + 8);
  }

  drawWhiteNoise() {
    const canvas = this.canvases.get('whiteNoiseCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制 1D 白噪声地形
    ctx.strokeStyle = '#2962ff';
    ctx.lineWidth = 2;
    ctx.beginPath();

    for (let x = 0; x < width; x++) {
      const noise = Math.random() * height;
      if (x === 0) {
        ctx.moveTo(x, height - noise);
      } else {
        ctx.lineTo(x, height - noise);
      }
    }
    ctx.stroke();

    // 添加文字说明（根据当前语言）
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '14px Arial';
    const text = this.currentLanguage === 'zh' 
      ? '白噪声地形：太随机，不平滑'
      : 'White noise terrain: Too random, not smooth';
    ctx.fillText(text, 10, 25);
  }

  drawDesiredTerrain() {
    const canvas = this.canvases.get('desiredTerrain');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 使用 Perlin 噪声生成地形
    ctx.strokeStyle = '#00c853';
    ctx.lineWidth = 3;
    ctx.beginPath();

    const scale = 0.01;
    for (let x = 0; x < width; x++) {
      const noise = (perlinNoise2D(x * scale, 0) + 1) * height / 2;
      if (x === 0) {
        ctx.moveTo(x, height - noise);
      } else {
        ctx.lineTo(x, height - noise);
      }
    }
    ctx.stroke();

    // 添加文字说明（根据当前语言）
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '14px Arial';
    const text = this.currentLanguage === 'zh'
      ? '柏林噪声地形：随机且平滑，自然真实'
      : 'Perlin noise terrain: Random and smooth, looks natural';
    ctx.fillText(text, 10, 25);
  }

  regenerateWhiteNoise() {
    this.drawWhiteNoise();
  }

  showPerlinSolution() {
    if (window.navigationController) {
      window.navigationController.goToModule(2);
    }
  }
  
  // 重绘所有 Canvas 文字（语言切换时调用）
  redrawCanvasText() {
    // 模块 1 的 Canvas
    this.drawWhiteNoise();
    this.drawDesiredTerrain();
    
    // 模块 3 的 Canvas
    this.drawStep1Grid();
    this.drawStep2Gradients();
    this.drawStep3DotProduct();
    this.drawStep4Interpolation();
  }

  // 模块2：肯·柏林简介
  initModule2() {
    // 无需特殊初始化
  }

  // 模块3：算法原理
  initModule3() {
    this.drawStep1Grid();
    this.drawStep2Gradients();
    this.drawStep3DotProduct();
    this.drawStep4Interpolation();
    this.drawGrid();
    this.setupDotProductCanvas();
    this.drawPropertiesVisualization();
    this.setupAlgorithmTabs();
    this.setupStepInteractions();
  }

  // 设置第1-4页的交互功能
  setupStepInteractions() {
    this.setupStep1Interaction();
    this.setupStep2Interaction();
    this.setupStep3Interaction();
    this.setupStep4Interaction();
  }

  // 第1页：网格划分交互（鼠标悬停高亮单元格）
  setupStep1Interaction() {
    const canvas = this.canvases.get('step1Canvas');
    if (!canvas) return;

    canvas.addEventListener('mousemove', (e) => {
      const rect = canvas.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const y = e.clientY - rect.top;
      
      this.drawGridToCanvas('step1Canvas', 500, 300, 40);
      this.highlightGridCell('step1Canvas', x, y, 40);
    });

    canvas.addEventListener('mouseleave', () => {
      this.drawGridToCanvas('step1Canvas', 500, 300, 40);
    });
  }

  // 高亮网格单元格
  highlightGridCell(canvasId, mouseX, mouseY, gridSize) {
    const canvas = this.canvases.get(canvasId);
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const gridX = Math.floor(mouseX / gridSize);
    const gridY = Math.floor(mouseY / gridSize);
    
    const x = gridX * gridSize;
    const y = gridY * gridSize;

    // 高亮单元格
    ctx.strokeStyle = '#ffab00';
    ctx.lineWidth = 2;
    ctx.strokeRect(x, y, gridSize, gridSize);
    
    // 显示坐标
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 12px Arial';
    ctx.textAlign = 'center';
    ctx.fillText(`(${gridX}, ${gridY})`, x + gridSize / 2, y + gridSize / 2 + 4);
  }

  // 第2页：梯度向量交互（重新生成按钮）
  setupStep2Interaction() {
    // 按钮事件在HTML中已绑定，这里确保画布有初始绘制
    const canvas = this.canvases.get('step2Canvas');
    if (!canvas) return;
    
    // 初始绘制
    this.drawGradientsToCanvas('step2Canvas', 500, 300, 40);
  }

  // 第3页：点乘运算交互（鼠标移动实时计算）
  setupStep3Interaction() {
    const canvas = this.canvases.get('step3Canvas');
    if (!canvas) return;

    // 生成固定的梯度向量（避免每次鼠标移动都变化）
    const gridSize = 40;
    const rows = Math.floor(300 / gridSize);
    const cols = Math.floor(500 / gridSize);
    
    if (!this.step3Gradients) {
      this.step3Gradients = [];
      for (let i = 0; i <= rows; i++) {
        for (let j = 0; j <= cols; j++) {
          this.step3Gradients.push({
            x: j * gridSize,
            y: i * gridSize,
            angle: Math.random() * Math.PI * 2
          });
        }
      }
    }

    // 初始为单点模式
    this.step3ViewMode = 'single';

    canvas.addEventListener('mousemove', (e) => {
      const rect = canvas.getBoundingClientRect();
      const mouseX = e.clientX - rect.left;
      const mouseY = e.clientY - rect.top;
      
      if (this.step3ViewMode === 'single') {
        this.drawStep3DotProductInteractive(mouseX, mouseY);
      }
    });

    // 初始绘制
    this.drawStep3DotProductInteractive(250, 150);
  }

  // 交互式点乘运算绘制 - 鼠标完全自由跟随，无吸附
  drawStep3DotProductInteractive(mouseX, mouseY) {
    const canvas = this.canvases.get('step3Canvas');
    if (!canvas) return;
    
    const ctx = canvas.getContext('2d');
    const width = 500;
    const height = 300;
    const gridSize = 40;
    
    // 绘制网格背景
    ctx.clearRect(0, 0, width, height);
    ctx.strokeStyle = 'rgba(41, 98, 255, 0.2)';
    ctx.lineWidth = 1;

    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 鼠标位置完全自由跟随，不吸附到网格
    const targetX = mouseX;
    const targetY = mouseY;

    // 绘制目标点（红色）
    ctx.fillStyle = '#ff5252';
    ctx.beginPath();
    ctx.arc(targetX, targetY, 6, 0, Math.PI * 2);
    ctx.fill();
    
    ctx.strokeStyle = 'rgba(255, 82, 82, 0.5)';
    ctx.lineWidth = 2;
    ctx.stroke();

    // 计算鼠标所在的网格单元
    const mouseGridX = Math.floor(targetX / gridSize);
    const mouseGridY = Math.floor(targetY / gridSize);
    
    // 只计算和绘制当前格子（鼠标所在格子）的四个顶点
    const corners = [
      { gx: mouseGridX, gy: mouseGridY, name: 'A' },
      { gx: mouseGridX + 1, gy: mouseGridY, name: 'B' },
      { gx: mouseGridX, gy: mouseGridY + 1, name: 'C' },
      { gx: mouseGridX + 1, gy: mouseGridY + 1, name: 'D' }
    ];
    
    corners.forEach(corner => {
      const cornerX = corner.gx * gridSize;
      const cornerY = corner.gy * gridSize;
      
      const gradient = this.step3Gradients.find(g => g.x === cornerX && g.y === cornerY);
      if (!gradient) return;
      
      // 绘制梯度向量（蓝色，较短）
      const gradLength = gridSize * 0.2;
      ctx.strokeStyle = '#2962ff';
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      ctx.moveTo(cornerX, cornerY);
      ctx.lineTo(cornerX + Math.cos(gradient.angle) * gradLength, cornerY + Math.sin(gradient.angle) * gradLength);
      ctx.stroke();
      
      // 绘制距离向量（红色虚线，从格点到鼠标位置）
      ctx.strokeStyle = 'rgba(255, 82, 82, 0.6)';
      ctx.lineWidth = 1;
      ctx.setLineDash([2, 2]);
      ctx.beginPath();
      ctx.moveTo(cornerX, cornerY);
      ctx.lineTo(targetX, targetY);
      ctx.stroke();
      ctx.setLineDash([]);
      
      // 计算点乘
      const dx = (targetX - cornerX) / gridSize;
      const dy = (targetY - cornerY) / gridSize;
      const dotProduct = Math.cos(gradient.angle) * dx + Math.sin(gradient.angle) * dy;
      
      // 绘制格点（绿色）
      ctx.fillStyle = '#00c853';
      ctx.beginPath();
      ctx.arc(cornerX, cornerY, 4, 0, Math.PI * 2);
      ctx.fill();
      
      // 显示顶点名称
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 10px Arial';
      ctx.textAlign = 'center';
      ctx.fillText(corner.name, cornerX, cornerY - 8);
      
      // 显示点乘值
      ctx.fillStyle = 'rgba(255, 171, 0, 0.9)';
      ctx.font = '10px Arial';
      ctx.fillText(dotProduct.toFixed(2), cornerX, cornerY + 15);
    });

    // 显示鼠标位置（左上角）
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '12px Arial';
    ctx.textAlign = 'left';
    ctx.fillText(`鼠标位置: (${mouseX.toFixed(0)}, ${mouseY.toFixed(0)})`, 10, 20);
  }

  // 第4页：插值平滑交互（参数滑块）
  setupStep4Interaction() {
    const canvas = this.canvases.get('step4Canvas');
    if (!canvas) return;

    // 获取滑块元素
    const sliderA = document.getElementById('cornerAValue');
    const sliderB = document.getElementById('cornerBValue');
    const sliderC = document.getElementById('cornerCValue');
    const sliderD = document.getElementById('cornerDValue');
    
    const displayA = document.getElementById('cornerAValueDisplay');
    const displayB = document.getElementById('cornerBValueDisplay');
    const displayC = document.getElementById('cornerCValueDisplay');
    const displayD = document.getElementById('cornerDValueDisplay');

    if (!sliderA || !sliderB || !sliderC || !sliderD) return;

    const updateVisualization = () => {
      const values = {
        A: parseFloat(sliderA.value),
        B: parseFloat(sliderB.value),
        C: parseFloat(sliderC.value),
        D: parseFloat(sliderD.value)
      };
      
      // 更新显示
      displayA.textContent = values.A.toFixed(1);
      displayB.textContent = values.B.toFixed(1);
      displayC.textContent = values.C.toFixed(1);
      displayD.textContent = values.D.toFixed(1);
      
      // 重新绘制（使用合适尺寸）
      this.drawStep4InterpolationInteractive(values, 600, 380);
    };

    sliderA.addEventListener('input', updateVisualization);
    sliderB.addEventListener('input', updateVisualization);
    sliderC.addEventListener('input', updateVisualization);
    sliderD.addEventListener('input', updateVisualization);

    // 初始绘制
    updateVisualization();
  }

  // 交互式插值平滑绘制
  drawStep4InterpolationInteractive(cornerValues, width = 500, height = 300) {
    const canvas = this.canvases.get('step4Canvas');
    if (!canvas) return;
    
    const ctx = canvas.getContext('2d');

    // 绘制背景网格
    ctx.clearRect(0, 0, width, height);
    const gridSize = 40;
    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    ctx.strokeStyle = 'rgba(41, 98, 255, 0.2)';
    ctx.lineWidth = 1;

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 将四个顶点移动到canvas的四个角，与预览颜色区域对齐
    const corners = [
      { x: 0, y: 0, value: cornerValues.A, name: 'A' },
      { x: width, y: 0, value: cornerValues.B, name: 'B' },
      { x: 0, y: height, value: cornerValues.C, name: 'C' },
      { x: width, y: height, value: cornerValues.D, name: 'D' }
    ];

    // 绘制插值结果（放大中心单元格显示）
    // 将中心单元格的插值效果放大到整个canvas
    const cellWidth = width / gridSize;  // 每个单元格的像素宽度
    const cellHeight = height / gridSize; // 每个单元格的像素高度
    
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        // 将canvas坐标映射到中心单元格内的相对坐标 (0-1)
        const u = x / width;  // x方向的插值参数
        const v = y / height; // y方向的插值参数
        
        // 使用平滑插值函数
        const smoothU = this.smoothStep(u);
        const smoothV = this.smoothStep(v);
        
        // 双线性插值计算颜色值
        const top = this.lerp(cornerValues.A, cornerValues.B, smoothU);
        const bottom = this.lerp(cornerValues.C, cornerValues.D, smoothU);
        const value = this.lerp(top, bottom, smoothV);
        
        const intensity = Math.floor(value * 255);
        const index = (y * width + x) * 4;
        
        data[index] = intensity;
        data[index + 1] = intensity;
        data[index + 2] = intensity + 50;
        data[index + 3] = 255; // 完全不透明
      }
    }

    ctx.putImageData(imageData, 0, 0);

    // 绘制角点
    corners.forEach(corner => {
      const intensity = Math.floor(corner.value * 255);
      ctx.fillStyle = `rgb(${intensity}, ${intensity}, ${intensity + 50})`;
      ctx.beginPath();
      ctx.arc(corner.x, corner.y, 8, 0, Math.PI * 2);
      ctx.fill();
      
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 1;
      ctx.stroke();
      
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 12px Arial';
      ctx.textAlign = 'center';
      ctx.fillText(corner.name, corner.x, corner.y - 12);
      ctx.font = '10px Arial';
      ctx.fillText(corner.value.toFixed(1), corner.x, corner.y + 15);
    });

    // 绘制插值函数曲线
    const graphY = height - 60;
    const graphHeight = 40;
    
    ctx.strokeStyle = '#2962ff';
    ctx.lineWidth = 2;
    ctx.beginPath();
    
    for (let x = 0; x < width; x++) {
      const t = x / width;
      const smoothT = this.smoothStep(t);
      const y = graphY + graphHeight - smoothT * graphHeight;
      
      if (x === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
    
    // 坐标轴
    ctx.strokeStyle = '#e0e7ff';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, graphY + graphHeight);
    ctx.lineTo(width, graphY + graphHeight);
    ctx.moveTo(0, graphY);
    ctx.lineTo(0, graphY + graphHeight);
    ctx.stroke();
  }

  // 设置算法演示页面的tab切换
  setupAlgorithmTabs() {
    const tabs = document.querySelectorAll('#module-3 .viz-tab');
    const vizInfo = document.getElementById('vizInfo');
    
    if (!tabs.length || !vizInfo) return;
    
    const infoTexts = {
      grid: '<i class="fas fa-info-circle"></i> 网格划分：将空间分为规则的单元格，便于局部计算',
      gradient: '<i class="fas fa-info-circle"></i> 梯度向量：为每个网格顶点分配随机方向的单位向量',
      dotproduct: '<i class="fas fa-info-circle"></i> 点乘运算：计算梯度向量与距离向量的点积，表示影响程度',
      interpolation: '<i class="fas fa-info-circle"></i> 插值平滑：使用5阶多项式对四个角点的结果进行平滑插值'
    };
    
    // 存储当前tab状态
    this.currentAlgorithmTab = 'grid';
    
    tabs.forEach(tab => {
      tab.addEventListener('click', (e) => {
        // 移除所有active类
        tabs.forEach(t => t.classList.remove('active'));
        // 添加active类到当前tab
        e.target.classList.add('active');
        
        // 更新信息文本
        const vizType = e.target.dataset.viz;
        this.currentAlgorithmTab = vizType;
        
        if (infoTexts[vizType]) {
          vizInfo.innerHTML = infoTexts[vizType];
        }
        
        // 根据选择的类型重新绘制 - 修复：确保每种视图都有正确的绘制函数
        switch(vizType) {
          case 'grid':
            this.drawGrid();
            break;
          case 'gradient':
            // 重新生成梯度以确保每次切换都有新的随机向量
            this.drawGradients();
            break;
          case 'dotproduct':
            // 点乘运算：绘制基础网格，准备鼠标交互
            this.drawGrid();
            // 清除之前的鼠标位置标记
            this.lastMouseX = null;
            this.lastMouseY = null;
            break;
          case 'interpolation':
            this.drawInterpolationCurve();
            break;
        }
      });
    });
    
    // 为algorithmCanvas添加鼠标事件（仅点乘运算需要）
    const algorithmCanvas = this.canvases.get('algorithmCanvas');
    if (algorithmCanvas) {
      algorithmCanvas.addEventListener('mousemove', (e) => {
        if (this.currentAlgorithmTab === 'dotproduct') {
          const rect = algorithmCanvas.getBoundingClientRect();
          const x = e.clientX - rect.left;
          const y = e.clientY - rect.top;
          this.drawDotProductVisualization(x, y);
        }
      });
      
      // 鼠标离开时重绘网格
      algorithmCanvas.addEventListener('mouseleave', () => {
        if (this.currentAlgorithmTab === 'dotproduct') {
          this.drawGrid();
        }
      });
    }
  }

  // 第1页：网格划分
  drawStep1Grid() {
    this.drawGridToCanvas('step1Canvas', 500, 300, 40);
  }

  // 第2页：梯度向量
  drawStep2Gradients() {
    this.drawGradientsToCanvas('step2Canvas', 500, 300, 40);
  }

  // 第3页：点乘运算
  drawStep3DotProduct() {
    this.drawDotProductToCanvas('step3Canvas', 500, 300, 40);
  }

  // 绘制点乘运算到指定canvas
  drawDotProductToCanvas(canvasId, width = 500, height = 300, gridSize = 40) {
    const canvas = this.canvases.get(canvasId);
    if (!canvas) return;
    
    canvas.width = width;
    canvas.height = height;

    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, width, height);

    // 绘制网格背景
    ctx.strokeStyle = 'rgba(41, 98, 255, 0.2)';
    ctx.lineWidth = 1;

    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 选择一个单元格作为示例（居中）
    const centerGridX = Math.floor(cols / 2);
    const centerGridY = Math.floor(rows / 2);
    const targetX = centerGridX * gridSize + gridSize / 2;
    const targetY = centerGridY * gridSize + gridSize / 2;

    // 绘制目标点（红色）
    ctx.fillStyle = '#ff5252';
    ctx.beginPath();
    ctx.arc(targetX, targetY, 6, 0, Math.PI * 2);
    ctx.fill();
    
    // 添加发光效果
    ctx.strokeStyle = 'rgba(255, 82, 82, 0.5)';
    ctx.lineWidth = 2;
    ctx.stroke();

    // 获取四个角点
    const corners = [
      { x: centerGridX * gridSize, y: centerGridY * gridSize, name: 'A' },
      { x: (centerGridX + 1) * gridSize, y: centerGridY * gridSize, name: 'B' },
      { x: centerGridX * gridSize, y: (centerGridY + 1) * gridSize, name: 'C' },
      { x: (centerGridX + 1) * gridSize, y: (centerGridY + 1) * gridSize, name: 'D' }
    ];

    // 为每个角点生成随机梯度向量
    corners.forEach((corner, index) => {
      const angle = Math.random() * Math.PI * 2;
      const length = gridSize * 0.25;
      
      // 绘制梯度向量（蓝色）
      ctx.strokeStyle = '#2962ff';
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(corner.x, corner.y);
      ctx.lineTo(corner.x + Math.cos(angle) * length, corner.y + Math.sin(angle) * length);
      ctx.stroke();

      // 绘制到目标点的距离向量（红色虚线）
      ctx.strokeStyle = 'rgba(255, 82, 82, 0.6)';
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(corner.x, corner.y);
      ctx.lineTo(targetX, targetY);
      ctx.stroke();
      ctx.setLineDash([]);

      // 计算点乘（可视化强度）
      const dx = (targetX - corner.x) / gridSize;
      const dy = (targetY - corner.y) / gridSize;
      const dotProduct = Math.cos(angle) * dx + Math.sin(angle) * dy;
      const intensity = Math.abs(dotProduct);

      // 绘制角点标记
      ctx.fillStyle = '#00c853';
      ctx.beginPath();
      ctx.arc(corner.x, corner.y, 4, 0, Math.PI * 2);
      ctx.fill();
      
      // 标注角点名称
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 12px Arial';
      ctx.textAlign = 'center';
      ctx.fillText(corner.name, corner.x, corner.y - 10);
      
      // 显示点乘值
      if (intensity > 0.1) {
        ctx.fillStyle = 'rgba(255, 171, 0, 0.8)';
        ctx.font = '10px Arial';
        ctx.fillText(dotProduct.toFixed(2), corner.x, corner.y + 15);
      }
    });

    // 添加说明文字
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '14px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const title = this.currentLanguage === 'zh' ? '点乘运算' : 'Dot Product';
    ctx.fillText(title, 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const desc = this.currentLanguage === 'zh' 
      ? '计算梯度向量与距离向量的点乘'
      : 'Calculate dot product of gradient and distance vectors';
    ctx.fillText(desc, 15, 55);
  }

  // 第4页：插值平滑
  drawStep4Interpolation() {
    this.drawInterpolationToCanvas('step4Canvas', 800, 500);
  }

  // 绘制插值平滑到指定canvas
  drawInterpolationToCanvas(canvasId, width = 500, height = 300) {
    const canvas = this.canvases.get(canvasId);
    if (!canvas) return;
    
    canvas.width = width;
    canvas.height = height;

    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, width, height);

    // 绘制背景网格
    const gridSize = 40;
    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    ctx.strokeStyle = 'rgba(41, 98, 255, 0.2)';
    ctx.lineWidth = 1;

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 选择四个角点
    const centerGridX = Math.floor(cols / 2);
    const centerGridY = Math.floor(rows / 2);
    
    const corners = [
      { 
        x: centerGridX * gridSize, 
        y: centerGridY * gridSize, 
        value: Math.random() * 0.8 + 0.1, 
        name: 'A' 
      },
      { 
        x: (centerGridX + 1) * gridSize, 
        y: centerGridY * gridSize, 
        value: Math.random() * 0.8 + 0.1, 
        name: 'B' 
      },
      { 
        x: centerGridX * gridSize, 
        y: (centerGridY + 1) * gridSize, 
        value: Math.random() * 0.8 + 0.1, 
        name: 'C' 
      },
      { 
        x: (centerGridX + 1) * gridSize, 
        y: (centerGridY + 1) * gridSize, 
        value: Math.random() * 0.8 + 0.1, 
        name: 'D' 
      }
    ];

    // 绘制四个角点的值（用颜色表示）
    corners.forEach(corner => {
      // 绘制角点圆圈
      const intensity = Math.floor(corner.value * 255);
      ctx.fillStyle = `rgb(${intensity}, ${intensity}, ${intensity + 50})`;
      ctx.beginPath();
      ctx.arc(corner.x, corner.y, 8, 0, Math.PI * 2);
      ctx.fill();
      
      // 添加边框
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = 1;
      ctx.stroke();
      
      // 标注名称和值
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 12px Arial';
      ctx.textAlign = 'center';
      ctx.fillText(corner.name, corner.x, corner.y - 12);
      ctx.font = '10px Arial';
      ctx.fillText(corner.value.toFixed(2), corner.x, corner.y + 15);
    });

    // 绘制平滑插值后的渐变效果
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const gridX = x / gridSize - centerGridX;
        const gridY = y / gridSize - centerGridY;
        
        if (gridX >= 0 && gridX <= 1 && gridY >= 0 && gridY <= 1) {
          // 使用平滑插值函数
          const u = this.smoothStep(gridX);
          const v = this.smoothStep(gridY);
          
          // 双线性插值
          const top = this.lerp(corners[0].value, corners[1].value, u);
          const bottom = this.lerp(corners[2].value, corners[3].value, u);
          const value = this.lerp(top, bottom, v);
          
          const intensity = Math.floor(value * 255);
          const index = (y * width + x) * 4;
          
          data[index] = intensity;
          data[index + 1] = intensity;
          data[index + 2] = intensity + 50;
          data[index + 3] = 150; // 半透明
        }
      }
    }

    ctx.putImageData(imageData, 0, 0);

    // 添加插值函数可视化（底部）
    const graphY = height - 60;
    const graphHeight = 40;
    
    ctx.strokeStyle = '#2962ff';
    ctx.lineWidth = 2;
    ctx.beginPath();
    
    for (let x = 0; x < width; x++) {
      const t = x / width;
      const smoothT = this.smoothStep(t);
      const y = graphY + graphHeight - smoothT * graphHeight;
      
      if (x === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
    
    // 绘制坐标轴
    ctx.strokeStyle = '#e0e7ff';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, graphY + graphHeight);
    ctx.lineTo(width, graphY + graphHeight);
    ctx.moveTo(0, graphY);
    ctx.lineTo(0, graphY + graphHeight);
    ctx.stroke();
    
    // 标注函数
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '12px Arial';
    ctx.fillText('smooth(t) = 6t⁵ - 15t⁴ + 10t³', 10, graphY - 5);

    // 添加说明文字
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '14px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const title = this.currentLanguage === 'zh' ? '平滑插值' : 'Smooth Interpolation';
    ctx.fillText(title, 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const desc = this.currentLanguage === 'zh'
      ? '使用 5 阶多项式进行平滑插值'
      : 'Use 5th order polynomial for smooth interpolation';
    ctx.fillText(desc, 15, 55);
  }

  // 平滑插值函数
  smoothStep(t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
  }

  // 线性插值
  lerp(a, b, t) {
    return a + (b - a) * t;
  }

  drawInterpolationCurve() {
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    
    // 清除画布
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // 使用通用插值绘制逻辑
    this.drawInterpolationToCanvas('algorithmCanvas', canvas.width, canvas.height);
  }

  // 绘制网格到指定canvas
drawGridToCanvas(canvasId, width = 500, height = 300, gridSize = 40) {
    const canvas = this.canvases.get(canvasId);
    if (!canvas) return;
    
    // 设置canvas尺寸
    canvas.width = width;
    canvas.height = height;

    const ctx = canvas.getContext('2d');

    ctx.clearRect(0, 0, width, height);

    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    // 绘制网格线
    ctx.strokeStyle = 'rgba(41, 98, 255, 0.3)';
    ctx.lineWidth = 1;

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 绘制网格点（使用毛玻璃效果）
    ctx.fillStyle = '#2962ff';
    for (let i = 0; i <= rows; i++) {
      for (let j = 0; j <= cols; j++) {
        ctx.beginPath();
        ctx.arc(j * gridSize, i * gridSize, 4, 0, Math.PI * 2);
        ctx.fill();
        
        // 添加发光效果
        ctx.strokeStyle = 'rgba(92, 157, 255, 0.5)';
        ctx.lineWidth = 1;
        ctx.stroke();
      }
    }
    
    // 添加说明文字（不再默认高亮单元格）
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '14px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const title = this.currentLanguage === 'zh' ? '规则网格划分' : 'Regular Grid';
    ctx.fillText(title, 15, 30);
    
    const cellSizeText = this.currentLanguage === 'zh'
      ? `单元格大小：${gridSize}×${gridSize}像素`
      : `Cell size: ${gridSize}×${gridSize}px`;
    ctx.fillText(cellSizeText, 15, 55);
  }

  // 模块5的主网格绘制（保持原有逻辑但增强视觉效果）
  // 切换点乘运算视图模式（全图/单点）
  toggleDotProductView() {
    const canvas = this.canvases.get('step3Canvas');
    if (!canvas) return;
    
    // 切换模式
    this.step3ViewMode = this.step3ViewMode === 'single' ? 'full' : 'single';
    
    // 更新按钮文本（根据当前语言）
    const btn = document.getElementById('dotProductToggleBtn');
    if (btn) {
      if (this.step3ViewMode === 'single') {
        btn.innerHTML = this.currentLanguage === 'zh' 
          ? '<i class="fas fa-eye"></i> 查看全图点积'
          : '<i class="fas fa-eye"></i> View Full Dot Product';
      } else {
        btn.innerHTML = this.currentLanguage === 'zh'
          ? '<i class="fas fa-crosshairs"></i> 单点查看模式'
          : '<i class="fas fa-crosshairs"></i> Single Point Mode';
      }
    }
    
    // 如果是全图模式，立即绘制
    if (this.step3ViewMode === 'full') {
      this.drawFullDotProductMap();
    } else {
      // 单点模式，绘制默认
      this.drawStep3DotProductInteractive(250, 150);
    }
  }

  // 绘制全图点积结果
  drawFullDotProductMap() {
    const canvas = this.canvases.get('step3Canvas');
    if (!canvas) return;
    
    const ctx = canvas.getContext('2d');
    const width = 500;
    const height = 300;
    const gridSize = 40;
    
    // 创建图像数据，用于逐像素绘制
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;
    
    // 遍历画布上的每个像素点（不只是网格点）
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        // 计算该点到所有格点的点积影响
        let dotSum = 0;
        let weightSum = 0;
        
        // 遍历所有格点
        for (const gradient of this.step3Gradients) {
          const dx = (x - gradient.x) / gridSize;
          const dy = (y - gradient.y) / gridSize;
          const distance = Math.sqrt(dx * dx + dy * dy);
          
          // 使用距离作为权重（距离越近影响越大）
          const weight = Math.max(0, 1 - distance / 5); // 5个网格范围内的影响
          
          if (weight > 0) {
            const dotProduct = Math.cos(gradient.angle) * dx + Math.sin(gradient.angle) * dy;
            dotSum += dotProduct * weight;
            weightSum += weight;
          }
        }
        
        // 归一化结果
        const finalValue = weightSum > 0 ? dotSum / weightSum : 0;
        
        // 将点积值映射到颜色（使用HSL颜色空间）
        // 正值用暖色（红色/橙色），负值用冷色（蓝色）
        const normalizedValue = Math.max(-1, Math.min(1, finalValue)); // 限制在[-1, 1]
        const hue = normalizedValue > 0 ? 30 - normalizedValue * 30 : 240 + normalizedValue * 60; // 正数:黄到红，负数:蓝到紫
        const lightness = 50 - Math.abs(normalizedValue) * 20; // 值越大颜色越深
        
        // 转换HSL到RGB
        const c = (1 - Math.abs(2 * lightness / 100 - 1)) * (Math.abs(normalizedValue) * 0.8 + 0.2);
        const xPrime = (1 - Math.abs((hue / 60) % 2 - 1)) * c;
        const m = lightness / 100 - c / 2;
        
        let r, g, b;
        if (hue >= 0 && hue < 60) {
          r = c; g = xPrime; b = 0;
        } else if (hue >= 60 && hue < 120) {
          r = xPrime; g = c; b = 0;
        } else if (hue >= 120 && hue < 180) {
          r = 0; g = c; b = xPrime;
        } else if (hue >= 180 && hue < 240) {
          r = 0; g = xPrime; b = c;
        } else if (hue >= 240 && hue < 300) {
          r = xPrime; g = 0; b = c;
        } else {
          r = c; g = 0; b = xPrime;
        }
        
        const index = (y * width + x) * 4;
        data[index] = (r + m) * 255;
        data[index + 1] = (g + m) * 255;
        data[index + 2] = (b + m) * 255;
        data[index + 3] = 255; // 不透明
      }
    }
    
    ctx.putImageData(imageData, 0, 0);
    
    // 绘制格点（覆盖在颜色图上）
    ctx.fillStyle = '#ffffff';
    for (const gradient of this.step3Gradients) {
      ctx.beginPath();
      ctx.arc(gradient.x, gradient.y, 2, 0, Math.PI * 2);
      ctx.fill();
    }
    
    // 添加图例
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '12px Arial';
    ctx.fillText('全图点积分布（颜色表示值大小）', 10, 20);
    ctx.fillStyle = '#ffab00';
    ctx.fillText('黄色：正点积', 10, 40);
    ctx.fillStyle = '#2962ff';
    ctx.fillText('蓝色：负点积', 10, 60);
  }

  drawGrid() {
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    const gridSize = 50;
    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    // 绘制网格线
    ctx.strokeStyle = 'rgba(41, 98, 255, 0.3)';
    ctx.lineWidth = 1;

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 绘制网格点
    ctx.fillStyle = '#2962ff';
    for (let i = 0; i <= rows; i++) {
      for (let j = 0; j <= cols; j++) {
        ctx.beginPath();
        ctx.arc(j * gridSize, i * gridSize, 4, 0, Math.PI * 2);
        ctx.fill();
      }
    }

    // 添加标签
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '12px Arial';
    ctx.fillText('网格划分：将空间分为规则的单元格', 10, 20);
  }

  // 绘制梯度向量到指定canvas
  drawGradientsToCanvas(canvasId, width = 500, height = 300, gridSize = 40) {
    const canvas = this.canvases.get(canvasId);
    if (!canvas) return;
    
    canvas.width = width;
    canvas.height = height;

    const ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, width, height);

    const rows = Math.floor(height / gridSize);
    const cols = Math.floor(width / gridSize);

    // 绘制网格背景（淡色）
    ctx.strokeStyle = 'rgba(41, 98, 255, 0.2)';
    ctx.lineWidth = 1;

    for (let i = 0; i <= rows; i++) {
      ctx.beginPath();
      ctx.moveTo(0, i * gridSize);
      ctx.lineTo(width, i * gridSize);
      ctx.stroke();
    }

    for (let j = 0; j <= cols; j++) {
      ctx.beginPath();
      ctx.moveTo(j * gridSize, 0);
      ctx.lineTo(j * gridSize, height);
      ctx.stroke();
    }

    // 为每个网格点生成随机梯度向量
    const gradients = [];
    for (let i = 0; i <= rows; i++) {
      for (let j = 0; j <= cols; j++) {
        const angle = Math.random() * Math.PI * 2;
        const length = gridSize * 0.3;
        
        const centerX = j * gridSize;
        const centerY = i * gridSize;
        const endX = centerX + Math.cos(angle) * length;
        const endY = centerY + Math.sin(angle) * length;

        // 绘制向量（蓝色箭头）
        ctx.strokeStyle = '#2962ff';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(centerX, centerY);
        ctx.lineTo(endX, endY);
        ctx.stroke();

        // 绘制箭头头部
        const arrowLength = 8;
        const arrowAngle = Math.PI / 6;
        const angle1 = angle + Math.PI - arrowAngle;
        const angle2 = angle - Math.PI + arrowAngle;

        ctx.beginPath();
        ctx.moveTo(endX, endY);
        ctx.lineTo(endX + Math.cos(angle1) * arrowLength, endY + Math.sin(angle1) * arrowLength);
        ctx.moveTo(endX, endY);
        ctx.lineTo(endX + Math.cos(angle2) * arrowLength, endY + Math.sin(angle2) * arrowLength);
        ctx.stroke();

        // 绘制中心点（发光效果）
        ctx.fillStyle = '#00c853';
        ctx.beginPath();
        ctx.arc(centerX, centerY, 4, 0, Math.PI * 2);
        ctx.fill();
        
        // 添加发光效果
        ctx.strokeStyle = 'rgba(0, 200, 83, 0.5)';
        ctx.lineWidth = 2;
        ctx.stroke();

        gradients.push({
          x: centerX,
          y: centerY,
          angle: angle,
          length: length
        });
      }
    }

    // 添加说明文字
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '14px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const title = this.currentLanguage === 'zh' ? '随机梯度向量' : 'Random Gradient Vectors';
    ctx.fillText(title, 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    const desc = this.currentLanguage === 'zh'
      ? '每个网格点有一个随机方向的单位向量'
      : 'Each grid point has a unit vector in random direction';
    ctx.fillText(desc, 15, 55);
  }

  drawGradients() {
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    
    // 清除画布
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    
    // 使用通用梯度绘制逻辑
    this.drawGradientsToCanvas('algorithmCanvas', canvas.width, canvas.height);
  }

  setupDotProductCanvas() {
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    // 鼠标移动事件
    canvas.addEventListener('mousemove', (e) => {
      const rect = canvas.getBoundingClientRect();
      const x = e.clientX - rect.left;
      const y = e.clientY - rect.top;
      this.drawDotProductVisualization(x, y);
    });
  }

  drawDotProductVisualization(mouseX, mouseY) {
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // 重新绘制网格（清除之前的绘制）
    this.drawGrid();

    const gridSize = 50;
    
    // 为algorithmCanvas生成梯度向量（如果还没有）
    if (!this.algorithmCanvasGradients) {
      const rows = Math.floor(height / gridSize);
      const cols = Math.floor(width / gridSize);
      this.algorithmCanvasGradients = [];
      for (let i = 0; i <= rows; i++) {
        for (let j = 0; j <= cols; j++) {
          this.algorithmCanvasGradients.push({
            x: j * gridSize,
            y: i * gridSize,
            angle: Math.random() * Math.PI * 2
          });
        }
      }
    }

    // 确定鼠标所在的网格单元
    const gridX = Math.floor(mouseX / gridSize);
    const gridY = Math.floor(mouseY / gridSize);
    
    // 四个角点（确保在画布范围内）
    const corners = [];
    for (let i = 0; i <= 1; i++) {
      for (let j = 0; j <= 1; j++) {
        const x = (gridX + j) * gridSize;
        const y = (gridY + i) * gridSize;
        if (x >= 0 && x <= width && y >= 0 && y <= height) {
          corners.push({x, y, name: i * 2 + j === 0 ? 'A' : i * 2 + j === 1 ? 'B' : i * 2 + j === 2 ? 'C' : 'D'});
        }
      }
    }

    // 绘制到鼠标位置的向量（红色虚线）
    corners.forEach(corner => {
      ctx.strokeStyle = 'rgba(255, 82, 82, 0.6)';
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(corner.x, corner.y);
      ctx.lineTo(mouseX, mouseY);
      ctx.stroke();
      ctx.setLineDash([]);
      
      // 查找对应的梯度向量
      const gradient = this.algorithmCanvasGradients.find(g => 
        Math.abs(g.x - corner.x) < 1 && Math.abs(g.y - corner.y) < 1
      );
      
      if (gradient) {
        // 绘制梯度向量（蓝色）
        const length = gridSize * 0.3;
        ctx.strokeStyle = '#2962ff';
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(corner.x, corner.y);
        ctx.lineTo(corner.x + Math.cos(gradient.angle) * length, corner.y + Math.sin(gradient.angle) * length);
        ctx.stroke();
        
        // 计算点乘
        const dx = (mouseX - corner.x) / gridSize;
        const dy = (mouseY - corner.y) / gridSize;
        const dotProduct = Math.cos(gradient.angle) * dx + Math.sin(gradient.angle) * dy;
        
        // 显示点乘值
        if (Math.abs(dotProduct) > 0.1) {
          ctx.fillStyle = 'rgba(255, 171, 0, 0.9)';
          ctx.font = '10px Arial';
          ctx.textAlign = 'center';
          ctx.fillText(dotProduct.toFixed(2), corner.x, corner.y - 5);
        }
      }
      
      // 绘制角点
      ctx.fillStyle = '#00c853';
      ctx.beginPath();
      ctx.arc(corner.x, corner.y, 4, 0, Math.PI * 2);
      ctx.fill();
      
      // 标注角点名称
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 10px Arial';
      ctx.textAlign = 'center';
      ctx.fillText(corner.name, corner.x, corner.y - 10);
    });

    // 绘制鼠标位置（红色圆点）
    ctx.fillStyle = '#ff5252';
    ctx.beginPath();
    ctx.arc(mouseX, mouseY, 6, 0, Math.PI * 2);
    ctx.fill();
    
    // 添加发光效果
    ctx.strokeStyle = 'rgba(255, 82, 82, 0.5)';
    ctx.lineWidth = 2;
    ctx.stroke();
    
    // 显示鼠标位置
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '12px Arial';
    ctx.textAlign = 'left';
    ctx.fillText(`鼠标位置: (${Math.round(mouseX)}, ${Math.round(mouseY)})`, 10, 20);
    
    // 保存鼠标位置，用于下一次绘制
    this.lastMouseX = mouseX;
    this.lastMouseY = mouseY;
  }

  drawInterpolationCurve() {
    // 插值曲线绘制逻辑
    const canvas = this.canvases.get('algorithmCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    // 绘制逻辑...
  }

  // 绘制算法特点可视化
  drawPropertiesVisualization() {
    this.drawContinuity();
    this.drawRandomness();
    this.drawRepeatability();
    this.drawControllability();
  }

  // 连续性可视化：平滑曲线
  drawContinuity() {
    const canvas = this.canvases.get('vizContinuity');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 绘制平滑连续的曲线
    ctx.strokeStyle = '#00c853';
    ctx.lineWidth = 2;
    ctx.beginPath();

    for (let x = 0; x < width; x++) {
      const noise = (perlinNoise2D(x * 0.05, 0) + 1) * height / 2;
      if (x === 0) {
        ctx.moveTo(x, noise);
      } else {
        ctx.lineTo(x, noise);
      }
    }
    ctx.stroke();

    // 添加文字
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '10px Arial';
    ctx.fillText('平滑连续', 5, 15);
  }

  // 随机性可视化：随机分布的点
  drawRandomness() {
    const canvas = this.canvases.get('vizRandomness');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 绘制随机分布的梯度向量（用短线表示）
    ctx.strokeStyle = '#2962ff';
    ctx.lineWidth = 1.5;

    for (let i = 0; i < 20; i++) {
      const x = Math.random() * width;
      const y = Math.random() * height;
      const angle = Math.random() * Math.PI * 2;
      const length = 8;

      ctx.beginPath();
      ctx.moveTo(x, y);
      ctx.lineTo(x + Math.cos(angle) * length, y + Math.sin(angle) * length);
      ctx.stroke();
    }

    // 添加文字
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '10px Arial';
    ctx.fillText('随机梯度', 5, 15);
  }

  // 可重复性可视化：两次相同结果
  drawRepeatability() {
    const canvas = this.canvases.get('vizRepeatability');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 设置种子，绘制两次相同的模式
    setNoiseSeed(42);

    ctx.strokeStyle = '#ffab00';
    ctx.lineWidth = 2;
    ctx.beginPath();

    for (let x = 0; x < width; x++) {
      const noise = (perlinNoise2D(x * 0.1, 0) + 1) * height / 2;
      if (x === 0) {
        ctx.moveTo(x, noise);
      } else {
        ctx.lineTo(x, noise);
      }
    }
    ctx.stroke();

    // 绘制"="符号表示可重复
    ctx.fillStyle = '#ffab00';
    ctx.font = 'bold 16px Arial';
    ctx.textAlign = 'center';
    ctx.fillText('=', width / 2, height / 2 + 5);

    // 添加文字
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '10px Arial';
    ctx.textAlign = 'left';
    ctx.fillText('可重复', 5, 15);
  }

  // 可控性可视化：不同参数对比
  drawControllability() {
    const canvas = this.canvases.get('vizControllability');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 绘制背景
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, height);

    // 上半部分：高频噪声
    ctx.strokeStyle = '#9c27b0';
    ctx.lineWidth = 1;
    ctx.beginPath();

    for (let x = 0; x < width; x++) {
      const noise = (perlinNoise2D(x * 0.2, 0) + 1) * height / 4;
      if (x === 0) {
        ctx.moveTo(x, noise);
      } else {
        ctx.lineTo(x, noise);
      }
    }
    ctx.stroke();

    // 下半部分：低频噪声
    ctx.strokeStyle = '#00bcd4';
    ctx.beginPath();

    for (let x = 0; x < width; x++) {
      const noise = (perlinNoise2D(x * 0.02, 0) + 1) * height / 4 + height / 2;
      if (x === 0) {
        ctx.moveTo(x, noise);
      } else {
        ctx.lineTo(x, noise);
      }
    }
    ctx.stroke();

    // 添加文字
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '10px Arial';
    ctx.fillText('可控性', 5, 15);
  }

  generateNewGrid() {
    setNoiseSeed(Math.floor(Math.random() * 1000));
    this.drawGrid();
  }

  // 重新生成梯度向量（第2页）
  regenerateGradients() {
    this.drawGradientsToCanvas('step2Canvas', 500, 300, 40);
  }

  toggleGridLines() {
    // 切换网格线显示
    this.drawGrid();
  }

  // 模块4：参数控制与演示
  initModule4() {
    this.setupModule4Parameters();
    this.bindModule4Effects();
    this.bindFlowControls();
    this.initParticles();
    this.renderModule4(); // 静态渲染
    this.startFlowAnimation();
  }

  // 柏林噪声算法实现
  perlinNoise3D(x, y, z) {
    const X = Math.floor(x) & 255;
    const Y = Math.floor(y) & 255;
    const Z = Math.floor(z) & 255;
    x -= Math.floor(x);
    y -= Math.floor(y);
    z -= Math.floor(z);
    const u = this.smoothStep(x);
    const v = this.smoothStep(y);
    const w = this.smoothStep(z);
    const A = this.perm[X] + Y;
    const AA = this.perm[A] + Z;
    const AB = this.perm[A + 1] + Z;
    const B = this.perm[X + 1] + Y;
    const BA = this.perm[B] + Z;
    const BB = this.perm[B + 1] + Z;
    return this.lerp(
      this.lerp(
        this.lerp(this.grad(this.perm[AA], x, y, z), this.grad(this.perm[BA], x - 1, y, z), u),
        this.lerp(this.grad(this.perm[AB], x, y - 1, z), this.grad(this.perm[BB], x - 1, y - 1, z), u), v),
      this.lerp(
        this.lerp(this.grad(this.perm[AA + 1], x, y, z - 1), this.grad(this.perm[BA + 1], x - 1, y, z - 1), u),
        this.lerp(this.grad(this.perm[AB + 1], x, y - 1, z - 1), this.grad(this.perm[BB + 1], x - 1, y - 1, z - 1), u), v), w);
  }

  grad(hash, x, y, z) {
    const h = hash & 15;
    const u = h < 8 ? x : y;
    const v = h < 4 ? y : (h === 12 || h === 14 ? x : z);
    return ((h & 1) === 0 ? u : -u) + ((h & 2) === 0 ? v : -v);
  }

  perm = new Uint8Array(512);

  initPermutation() {
    const p = new Uint8Array(256);
    for (let i = 0; i < 256; i++) {
      p[i] = i;
    }
    for (let i = 255; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [p[i], p[j]] = [p[j], p[i]];
    }
    for (let i = 0; i < 512; i++) {
      this.perm[i] = p[i & 255];
    }
  }

  // FBM (分形布朗运动)
  fbm(x, y, z, octaves = 4, persistence = 0.5, lacunarity = 2) {
    let value = 0;
    let amplitude = 1;
    let frequency = 1;
    let maxValue = 0;
    for (let i = 0; i < octaves; i++) {
      value += amplitude * this.perlinNoise3D(x * frequency, y * frequency, z * frequency);
      maxValue += amplitude;
      amplitude *= persistence;
      frequency *= lacunarity;
    }
    return value / maxValue;
  }

  // 模块 4 参数设置
  setupModule4Parameters() {
    // 初始化参数
    this.module4Params = {
      scale: 0.004,
      octaves: 4,
      persistence: 0.50,
      lacunarity: 2.0,
      speed: 0.5,
      currentEffect: 'noise',
      showFade: true,
      particleCount: 1000
    };
    
    // 初始化动画时间
    this.animTime = 0;

    // 绑定参数控制事件
    this.bindModule4Controls();
  }

  // 绑定模块4的控制事件
  bindModule4Controls() {
    // 缩放控制
    const scaleSlider = document.getElementById('scaleSlider');
    const scaleVal = document.getElementById('scaleVal');
    if (scaleSlider && scaleVal) {
      scaleSlider.addEventListener('input', (e) => {
        const raw = parseFloat(e.target.value);
        this.module4Params.scale = raw / 1000 * 0.049 + 0.001;
        scaleVal.textContent = this.module4Params.scale.toFixed(4);
        this.renderModule4(); // 实时渲染
      });
    }

    // 叠加层数控制
    const octavesSlider = document.getElementById('octavesSlider');
    const octavesVal = document.getElementById('octavesVal');
    if (octavesSlider && octavesVal) {
      octavesSlider.addEventListener('input', (e) => {
        this.module4Params.octaves = parseInt(e.target.value);
        octavesVal.textContent = this.module4Params.octaves;
        this.renderModule4(); // 实时渲染
      });
    }

    // 持续度控制
    const persistenceSlider = document.getElementById('persistenceSlider');
    const persistenceVal = document.getElementById('persistenceVal');
    if (persistenceSlider && persistenceVal) {
      persistenceSlider.addEventListener('input', (e) => {
        this.module4Params.persistence = parseFloat(e.target.value) / 100;
        persistenceVal.textContent = this.module4Params.persistence.toFixed(2);
        this.renderModule4(); // 实时渲染
      });
    }

    // 空隙度控制
    const lacunaritySlider = document.getElementById('lacunaritySlider');
    const lacunarityVal = document.getElementById('lacunarityVal');
    if (lacunaritySlider && lacunarityVal) {
      lacunaritySlider.addEventListener('input', (e) => {
        this.module4Params.lacunarity = parseFloat(e.target.value) / 10;
        lacunarityVal.textContent = this.module4Params.lacunarity.toFixed(2);
        this.renderModule4(); // 实时渲染
      });
    }

    // 随机种子按钮
    const randomSeedBtn = document.querySelector('.control-btn[onclick="randomizeSeed()"]');
    if (randomSeedBtn) {
      randomSeedBtn.addEventListener('click', () => {
        this.initPermutation();
        this.renderModule4(); // 实时渲染
      });
    }
  }

  // 绑定效果选择按钮
  bindModule4Effects() {
    const effectBtns = document.querySelectorAll('.effect-btn');
    effectBtns.forEach(btn => {
      btn.addEventListener('click', () => {
        const effectName = btn.dataset.effect;
        this.selectEffect(effectName);
      });
    });
  }

  // 选择效果
  selectEffect(effectName) {
    // 更新效果按钮状态
    document.querySelectorAll('.effect-btn').forEach(btn => {
      btn.classList.remove('active');
      if (btn.dataset.effect === effectName) {
        btn.classList.add('active');
      }
    });

    // 更新当前效果
    this.module4Params.currentEffect = effectName;

    // 更新效果名称显示
    const currentPreset = document.getElementById('currentPreset');
    if (currentPreset) {
      const effectNames = {
        noise: '噪声图',
        terrain: '地形',
        flow: '流场'
      };
      currentPreset.textContent = effectNames[effectName] || '噪声图';
    }

    // 流场模式需要重新初始化粒子
    if (effectName === 'flow' && (!this.particles || this.particles.length === 0)) {
      this.initParticles();
    }

    // 重新渲染
    this.renderModule4();
  }

  // 绑定流场控制按钮
  bindFlowControls() {
    // 流场箭头控制
    const toggleArrowsBtn = document.getElementById('toggleArrowsBtn');
    if (toggleArrowsBtn) {
      this.showFlowArrows = false;
      toggleArrowsBtn.addEventListener('click', () => {
        this.showFlowArrows = !this.showFlowArrows;
        toggleArrowsBtn.classList.toggle('active', this.showFlowArrows);
        this.renderModule4();
      });
    }

    // 粒子控制
    const toggleParticlesBtn = document.getElementById('toggleParticlesBtn');
    if (toggleParticlesBtn) {
      this.showParticles = false;
      toggleParticlesBtn.addEventListener('click', () => {
        this.showParticles = !this.showParticles;
        toggleParticlesBtn.classList.toggle('active', this.showParticles);
      });
    }

    // 拖尾控制
    const toggleFadeBtn = document.getElementById('toggleFadeBtn');
    if (toggleFadeBtn) {
      this.module4Params.showFade = true;
      toggleFadeBtn.addEventListener('click', () => {
        this.module4Params.showFade = !this.module4Params.showFade;
        toggleFadeBtn.classList.toggle('active', this.module4Params.showFade);
        this.renderModule4();
      });
    }

    // 粒子数量控制
    const particleCountSlider = document.getElementById('particleCountSlider');
    const particleCountVal = document.getElementById('particleCountVal');
    if (particleCountSlider && particleCountVal) {
      particleCountSlider.addEventListener('input', (e) => {
        const count = parseInt(e.target.value);
        this.module4Params.particleCount = count;
        particleCountVal.textContent = count;
        this.initParticles(); // 重新初始化粒子
      });
    }
  }



  // 初始化粒子系统
  initParticles() {
    this.particles = [];
    const particleCount = this.module4Params.particleCount;
    
    // 获取 canvas 尺寸
    const canvas = this.canvases.get('paramPreviewCanvas');
    const width = canvas ? canvas.width : 1000;
    const height = canvas ? canvas.height : 600;
    
    // 初始化粒子
    for (let i = 0; i < particleCount; i++) {
      this.particles.push({
        x: Math.random() * width,
        y: Math.random() * height,
        vx: 0,
        vy: 0,
        life: Math.random() * 200,
        maxLife: 150 + Math.random() * 200
      });
    }
  }

  // 开始流场动画
  startFlowAnimation() {
    let lastTime = 0;
    this.animTime = 0;

    const animate = (ts) => {
      const dt = ts - lastTime;
      lastTime = ts;
      this.animTime += dt;
      
      // 流场模式需要持续渲染
      if (this.module4Params.currentEffect === 'flow') {
        this.renderModule4();
      }
      
      requestAnimationFrame(animate);
    };

    requestAnimationFrame(animate);
  }

  // 渲染模块4
  renderModule4() {
    // 渲染参数预览（根据当前效果）
    this.renderParamPreview();
  }

  // 渲染参数预览
  renderParamPreview() {
    const canvas = this.canvases.get('paramPreviewCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // 清除画布
    ctx.clearRect(0, 0, width, height);

    const effect = this.module4Params.currentEffect || 'noise';
    const scale = this.module4Params.scale;

    switch (effect) {
      case 'noise':
        this.renderNoiseMap(ctx, width, height, scale);
        break;
      case 'terrain':
        this.renderTerrain(ctx, width, height, scale);
        break;
      case 'flow':
        this.renderFlowField(ctx, width, height, scale);
        break;
    }
  }

  // 渲染噪声图
  renderNoiseMap(ctx, width, height, scale) {
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * scale, y * scale, 0, this.module4Params.octaves, this.module4Params.persistence, this.module4Params.lacunarity);
        const value = Math.floor((noise + 1) * 127.5);
        const index = (y * width + x) * 4;
        data[index] = value;
        data[index + 1] = value;
        data[index + 2] = value;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  // 地形颜色映射
  terrainColor(h) {
    if (h < -0.3) return [10, 30, 80]; // 深海
    if (h < -0.05) return [20, 80, 160]; // 海洋
    if (h < 0.02) return [200, 190, 140]; // 沙滩
    if (h < 0.2) return [60, 160, 60]; // 低地
    if (h < 0.45) return [100, 130, 60]; // 高地
    if (h < 0.65) return [130, 100, 80]; // 山地
    return [240, 240, 255]; // 雪地
  }

  // 渲染地形
  renderTerrain(ctx, width, height, scale) {
    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;
    
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const h = this.fbm(x * scale, y * scale, 0, this.module4Params.octaves, this.module4Params.persistence, this.module4Params.lacunarity);
        const [r, g, b] = this.terrainColor(h);
        const idx = (y * width + x) * 4;
        data[idx] = r;
        data[idx + 1] = g;
        data[idx + 2] = b;
        data[idx + 3] = 255;
      }
    }
    
    ctx.putImageData(imageData, 0, 0);
  }

  // 渲染流场
  renderFlowField(ctx, width, height, scale) {
    const t = this.animTime * this.module4Params.speed * 0.0005;
    
    // 拖尾效果
    if (this.module4Params.showFade) {
      ctx.fillStyle = 'rgba(13, 13, 26, 0.18)';
      ctx.fillRect(0, 0, width, height);
    } else {
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = '#0d0d1a';
      ctx.fillRect(0, 0, width, height);
    }

    // 绘制流场箭头（静态，不随时间变化）
    if (this.showFlowArrows) {
      const step = 48;
      ctx.strokeStyle = 'rgba(124, 107, 255, 0.35)';
      ctx.lineWidth = 1;
      
      for (let y = step / 2; y < height; y += step) {
        for (let x = step / 2; x < width; x += step) {
          // 箭头使用 2D 噪声，不加入时间参数
          const v = this.perlinNoise3D(x * scale, y * scale, 0);
          const angle = v * Math.PI * 4;
          const len = 14;
          ctx.beginPath();
          ctx.moveTo(x, y);
          ctx.lineTo(x + Math.cos(angle) * len, y + Math.sin(angle) * len);
          ctx.stroke();
        }
      }
    }

    // 绘制粒子（粒子使用 3D 噪声，随时间变化）
    if (this.showParticles) {
      for (let p of this.particles) {
        const v = this.perlinNoise3D(p.x * scale, p.y * scale, t);
        const angle = v * Math.PI * 4;
        const speed = 2.5;
        
        p.vx = Math.cos(angle) * speed;
        p.vy = Math.sin(angle) * speed;
        p.x += p.vx;
        p.y += p.vy;
        p.life++;

        // 边界检测和重生
        if (p.life > p.maxLife || p.x < 0 || p.x > width || p.y < 0 || p.y > height) {
          p.x = Math.random() * width;
          p.y = Math.random() * height;
          p.life = 0;
        }

        // 计算粒子透明度和颜色
        const alpha = Math.min(p.life / 30, 1) * (1 - p.life / p.maxLife);
        const hue = (v + 1) * 180 + 160;
        
        ctx.beginPath();
        ctx.arc(p.x, p.y, 1.2, 0, Math.PI * 2);
        ctx.fillStyle = `hsla(${hue}, 90%, 70%, ${alpha})`;
        ctx.fill();
      }
    }
  }





  // 模块5：对比分析
  initModule5() {
    this.drawComparison();
  }

  drawComparison() {
    this.drawWhiteNoiseCompare();
    this.drawPerlinNoiseCompare();
    this.drawPerlinNoiseSimplexCompare();
    this.drawSimplexNoiseCompare();
    this.drawNoiseTypes();
    this.drawApplicationScenarios();
  }

  drawWhiteNoiseCompare() {
    const canvas = this.canvases.get('whiteNoiseCompare');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let i = 0; i < data.length; i += 4) {
      const value = Math.random() * 255;
      data[i] = value;
      data[i + 1] = value;
      data[i + 2] = value;
      data[i + 3] = 255;
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawPerlinNoiseCompare() {
    const canvas = this.canvases.get('perlinNoiseCompare');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.05, y * 0.05, 0, 3, 0.5);
        const value = Math.floor((noise + 1) * 127.5);
        const index = (y * width + x) * 4;
        
        data[index] = value;
        data[index + 1] = value;
        data[index + 2] = value;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawPerlinNoiseSimplexCompare() {
    const canvas = this.canvases.get('perlinNoiseSimplexCompare');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.05, y * 0.05, 0, 3, 0.5);
        const value = Math.floor((noise + 1) * 127.5);
        const index = (y * width + x) * 4;
        
        data[index] = value;
        data[index + 1] = value;
        data[index + 2] = value;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawSimplexNoiseCompare() {
    const canvas = this.canvases.get('simplexNoiseCompare');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.05, y * 0.05, 0, 3, 0.5);
        const value = Math.floor((noise + 1) * 127.5);
        const index = (y * width + x) * 4;
        
        data[index] = value;
        data[index + 1] = value + 30;
        data[index + 2] = value + 60;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawNoiseTypes() {
    const canvas = this.canvases.get('noiseTypesCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // 绘制三种噪声类型的对比
    const sectionWidth = width / 3;
    
    // 白噪声
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, sectionWidth, height);
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < sectionWidth; x++) {
        const value = Math.random() * 255;
        ctx.fillStyle = `rgb(${value}, ${value}, ${value})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#ffffff';
    ctx.font = 'bold 14px Arial';
    ctx.textAlign = 'center';
    ctx.fillText('白噪声', sectionWidth / 2, 30);
    
    // 柏林噪声
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(sectionWidth, 0, sectionWidth, height);
    for (let y = 0; y < height; y++) {
      for (let x = sectionWidth; x < sectionWidth * 2; x++) {
        const noise = this.fbm((x - sectionWidth) * 0.05, y * 0.05, 0, 3, 0.5);
        const value = Math.floor((noise + 1) * 127.5);
        ctx.fillStyle = `rgb(${value}, ${value}, ${value})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#ffffff';
    ctx.fillText('柏林噪声', sectionWidth * 1.5, 30);
    
    // Simplex噪声
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(sectionWidth * 2, 0, sectionWidth, height);
    for (let y = 0; y < height; y++) {
      for (let x = sectionWidth * 2; x < width; x++) {
        const noise = this.fbm((x - sectionWidth * 2) * 0.05, y * 0.05, 0, 3, 0.5);
        const value = Math.floor((noise + 1) * 127.5);
        ctx.fillStyle = `rgb(${value}, ${value + 30}, ${value + 60})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#ffffff';
    ctx.fillText('Simplex噪声', sectionWidth * 2.5, 30);
  }

  drawApplicationScenarios() {
    const canvas = this.canvases.get('applicationScenariosCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    // 绘制应用场景对比
    const sectionHeight = height / 3;
    
    // 游戏开发
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, 0, width, sectionHeight);
    for (let y = 0; y < sectionHeight; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.02, y * 0.02, 0, 4, 0.6);
        const normalized = (noise + 1) / 2;
        let r, g, b;
        if (normalized > 0.7) {
          r = g = b = Math.floor(200 + normalized * 55);
        } else if (normalized > 0.5) {
          r = g = b = Math.floor(100 + normalized * 100);
        } else if (normalized > 0.3) {
          r = Math.floor(normalized * 100);
          g = Math.floor(100 + normalized * 155);
          b = Math.floor(normalized * 50);
        } else {
          r = Math.floor(normalized * 50);
          g = Math.floor(normalized * 100);
          b = Math.floor(100 + normalized * 155);
        }
        ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#000000';
    ctx.font = 'bold 14px Arial';
    ctx.textAlign = 'center';
    const gameTitle = this.currentLanguage === 'zh' ? '游戏开发 - 地形生成' : 'Game Dev - Terrain Generation';
    ctx.fillText(gameTitle, width / 2, 30);
    
    // 视觉艺术
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, sectionHeight, width, sectionHeight);
    for (let y = sectionHeight; y < sectionHeight * 2; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.03, y * 0.03, 0, 2, 0.4);
        const value = Math.floor((noise + 1) * 127.5);
        const r = Math.floor(200 + (255 - 200) * (value / 255));
        const g = Math.floor(220 + (255 - 220) * (value / 255));
        const b = 255;
        ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#000000';
    const artTitle = this.currentLanguage === 'zh' ? '视觉艺术 - 纹理生成' : 'Visual Arts - Texture Generation';
    ctx.fillText(artTitle, width / 2, sectionHeight + 30);
    
    // 科学模拟
    ctx.fillStyle = '#1a1d2e';
    ctx.fillRect(0, sectionHeight * 2, width, sectionHeight);
    for (let y = sectionHeight * 2; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = this.fbm(x * 0.03, y * 0.03, 0, 4, 0.5);
        const normalized = (noise + 1) / 2;
        const level = Math.floor(normalized * 10) / 10;
        const r = Math.floor(level * 255);
        const g = Math.floor(level * 200);
        const b = Math.floor(level * 150);
        ctx.fillStyle = `rgb(${r}, ${g}, ${b})`;
        ctx.fillRect(x, y, 1, 1);
      }
    }
    ctx.fillStyle = '#000000';
    const sciTitle = this.currentLanguage === 'zh' ? '科学模拟 - 流体动力学' : 'Scientific Simulation - Fluid Dynamics';
    ctx.fillText(sciTitle, width / 2, sectionHeight * 2 + 30);
  }

  // 模块6：应用案例
  initModule6() {
    this.drawTerrain();
    this.drawCloud();
    this.drawMarbleTexture();
    this.drawContourMap();
  }

  drawTerrain() {
    const canvas = this.canvases.get('terrainCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const baseNoise = perlinNoise2D(x * 0.02, y * 0.02, 4, 0.6);
        const detailNoise = perlinNoise2D(x * 0.08, y * 0.08) * 0.3;
        const noise = baseNoise + detailNoise;
        const normalized = (noise + 1) / 2;

        let r, g, b;
        if (normalized > 0.7) {
          r = g = b = Math.floor(200 + normalized * 55);
        } else if (normalized > 0.5) {
          r = g = b = Math.floor(100 + normalized * 100);
        } else if (normalized > 0.3) {
          r = Math.floor(normalized * 100);
          g = Math.floor(100 + normalized * 155);
          b = Math.floor(normalized * 50);
        } else {
          r = Math.floor(normalized * 50);
          g = Math.floor(normalized * 100);
          b = Math.floor(100 + normalized * 155);
        }

        const index = (y * width + x) * 4;
        data[index] = r;
        data[index + 1] = g;
        data[index + 2] = b;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawCloud() {
    const canvas = this.canvases.get('natureCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = perlinNoise2D(x * 0.03, y * 0.03, 2, 0.4);
        const value = Math.floor((noise + 1) * 127.5);
        
        const r = Math.floor(200 + (255 - 200) * (value / 255));
        const g = Math.floor(220 + (255 - 220) * (value / 255));
        const b = 255;

        const index = (y * width + x) * 4;
        data[index] = r;
        data[index + 1] = g;
        data[index + 2] = b;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawMarbleTexture() {
    const canvas = this.canvases.get('textureCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = perlinNoise2D(x * 0.02, y * 0.02);
        const marble = Math.sin(x * 0.05 + noise * 5) * Math.cos(y * 0.05 + noise * 5);
        const value = Math.floor((marble + 1) * 127.5);
        
        const r = Math.floor(200 + value * 0.2);
        const g = Math.floor(200 + value * 0.3);
        const b = Math.floor(200 + value * 0.4);

        const index = (y * width + x) * 4;
        data[index] = r;
        data[index + 1] = g;
        data[index + 2] = b;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  drawContourMap() {
    const canvas = this.canvases.get('scienceCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = perlinNoise2D(x * 0.03, y * 0.03, 4, 0.5);
        const normalized = (noise + 1) / 2;
        const level = Math.floor(normalized * 10) / 10;
        
        const r = Math.floor(level * 255);
        const g = Math.floor(level * 200);
        const b = Math.floor(level * 150);

        const index = (y * width + x) * 4;
        data[index] = r;
        data[index + 1] = g;
        data[index + 2] = b;
        data[index + 3] = 255;
      }
    }

    ctx.putImageData(imageData, 0, 0);
  }

  // 模块7：总结
  initModule7() {
    // 无需特殊初始化
  }

  // 工具函数
  stopAllAnimations() {
    this.animations.forEach((animationId, key) => {
      cancelAnimationFrame(animationId);
      this.animations.delete(key);
    });
  }

  handleResize(moduleId) {
    // 重新初始化当前模块的可视化
    setTimeout(() => {
      this.initModuleVisualizations(moduleId);
    }, 100);
  }
}

// ==========================================
// 全局函数
// ==========================================

// 重新生成噪声
function regenerateNoise(type) {
  if (window.visualizationManager) {
    if (type === 'white') {
      window.visualizationManager.regenerateWhiteNoise();
    }
  }
}

// 生成新网格
function generateNewGrid() {
  if (window.visualizationManager) {
    window.visualizationManager.generateNewGrid();
  }
}

// 切换网格线
function toggleGridLines() {
  if (window.visualizationManager) {
    window.visualizationManager.toggleGridLines();
  }
}

// 生成地形
function generateTerrain() {
  if (window.visualizationManager) {
    setNoiseSeed(Math.floor(Math.random() * 1000));
    window.visualizationManager.drawTerrain();
  }
}

// 设置网格大小（第1页）
function setGridSize(size) {
  if (window.visualizationManager) {
    window.visualizationManager.setGridSize(size);
  }
}

// 重新生成梯度向量（第2页）
function regenerateGradients() {
  if (window.visualizationManager) {
    window.visualizationManager.regenerateGradients();
  }
}

// 切换点乘运算视图模式（第3页）
function toggleDotProductView() {
  if (window.visualizationManager) {
    window.visualizationManager.toggleDotProductView();
  }
}

// 模块4：更新参数
function updateParam(name, input) {
  // 这个函数由VisualizationManager内部的事件监听器处理
}

// 模块4：随机种子
function randomizeSeed() {
  if (window.visualizationManager) {
    window.visualizationManager.initPermutation();
  }
}



// ==========================================
// 初始化
// ==========================================

document.addEventListener('DOMContentLoaded', () => {
  window.visualizationManager = new VisualizationManager();
  console.log('Visualization system initialized');
});
