// ==========================================
// 可视化模块
// Visualizations Module
// ==========================================

class VisualizationManager {
  constructor() {
    this.canvases = new Map();
    this.animations = new Map();
    this.currentModule = 1;
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

    // 绘制1D白噪声地形
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

    // 添加文字说明
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '14px Arial';
    ctx.fillText('白噪声地形：太随机，不平滑', 10, 25);
  }

  drawDesiredTerrain() {
    const canvas = this.canvases.get('desiredTerrain');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    ctx.clearRect(0, 0, width, height);

    // 使用Perlin噪声生成地形
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

    // 添加文字说明
    ctx.fillStyle = '#b0b8ff';
    ctx.font = '14px Arial';
    ctx.fillText('柏林噪声地形：随机且平滑，自然真实', 10, 25);
  }

  regenerateWhiteNoise() {
    this.drawWhiteNoise();
  }

  showPerlinSolution() {
    if (window.navigationController) {
      window.navigationController.goToModule(2);
    }
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
    
    tabs.forEach(tab => {
      tab.addEventListener('click', (e) => {
        // 移除所有active类
        tabs.forEach(t => t.classList.remove('active'));
        // 添加active类到当前tab
        e.target.classList.add('active');
        
        // 更新信息文本
        const vizType = e.target.dataset.viz;
        if (infoTexts[vizType]) {
          vizInfo.innerHTML = infoTexts[vizType];
        }
        
        // 根据选择的类型重新绘制
        switch(vizType) {
          case 'grid':
            this.drawGrid();
            break;
          case 'gradient':
            this.drawGradients();
            break;
          case 'dotproduct':
            // 重置点乘演示
            this.drawGrid();
            break;
          case 'interpolation':
            this.drawInterpolationCurve();
            break;
        }
      });
    });
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
    ctx.fillText('点乘运算', 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    ctx.fillText('计算梯度向量与距离向量的点乘', 15, 55);
  }

  // 第4页：插值平滑
  drawStep4Interpolation() {
    this.drawInterpolationToCanvas('step4Canvas', 500, 300);
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
    ctx.fillText('平滑插值', 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    ctx.fillText('使用5阶多项式进行平滑插值', 15, 55);
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

    // 高亮一个单元格作为示例
    const centerX = Math.floor(cols / 2) * gridSize;
    const centerY = Math.floor(rows / 2) * gridSize;
    
    ctx.strokeStyle = '#00c853';
    ctx.lineWidth = 2;
    ctx.strokeRect(centerX, centerY, gridSize, gridSize);
    
    // 添加说明文字
    ctx.fillStyle = '#e0e7ff';
    ctx.font = '14px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    ctx.fillText('规则网格划分', 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    ctx.fillText(`单元格大小: ${gridSize}×${gridSize}像素`, 15, 55);
  }

  // 模块5的主网格绘制（保持原有逻辑但增强视觉效果）
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
    ctx.fillText('随机梯度向量', 15, 30);
    
    ctx.fillStyle = '#b8c2ff';
    ctx.font = '12px "Segoe UI", Tahoma, Geneva, Verdana, sans-serif';
    ctx.fillText('每个网格点有一个随机方向的单位向量', 15, 55);
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

    // 重新绘制网格
    this.drawGrid();

    const gridSize = 50;
    const gridX = Math.floor(mouseX / gridSize);
    const gridY = Math.floor(mouseY / gridSize);

    const points = [
      {x: gridX, y: gridY},
      {x: gridX + 1, y: gridY},
      {x: gridX, y: gridY + 1},
      {x: gridX + 1, y: gridY + 1}
    ];

    // 绘制到鼠标位置的向量
    points.forEach(point => {
      const centerX = point.x * gridSize;
      const centerY = point.y * gridSize;

      ctx.strokeStyle = '#ff5252';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(centerX, centerY);
      ctx.lineTo(mouseX, mouseY);
      ctx.stroke();
    });

    // 绘制鼠标位置
    ctx.fillStyle = '#ffffff';
    ctx.beginPath();
    ctx.arc(mouseX, mouseY, 5, 0, Math.PI * 2);
    ctx.fill();
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

  toggleGridLines() {
    // 切换网格线显示
    this.drawGrid();
  }

  // 模块4：参数控制
  initModule4() {
    this.setupParameterControls();
    this.drawPerlinNoise();
  }

  setupParameterControls() {
    // 参数控制设置
    const controls = [
      { id: 'frequency', param: 'frequency', min: 0.01, max: 0.2, step: 0.01, default: 0.05 },
      { id: 'amplitude', param: 'amplitude', min: 10, max: 100, step: 5, default: 50 },
      { id: 'octaves', param: 'octaves', min: 1, max: 6, step: 1, default: 3 },
      { id: 'persistence', param: 'persistence', min: 0.1, max: 1, step: 0.1, default: 0.5 }
    ];

    controls.forEach(control => {
      const slider = document.getElementById(control.id);
      const display = document.getElementById(control.id + 'Value');
      
      if (slider && display) {
        slider.addEventListener('input', (e) => {
          const value = parseFloat(e.target.value);
          display.textContent = control.param === 'frequency' || control.param === 'persistence' ? 
            value.toFixed(2) : value;
          this.updatePerlinNoise();
        });
      }
    });
  }

  drawPerlinNoise() {
    const canvas = this.canvases.get('perlinCanvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;

    const imageData = ctx.createImageData(width, height);
    const data = imageData.data;

    const frequency = parseFloat(document.getElementById('frequency')?.value || 0.05);
    const amplitude = parseFloat(document.getElementById('amplitude')?.value || 50);
    const octaves = parseInt(document.getElementById('octaves')?.value || 3);
    const persistence = parseFloat(document.getElementById('persistence')?.value || 0.5);

    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const noise = perlinNoise2D(x * frequency, y * frequency, octaves, persistence);
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

  updatePerlinNoise() {
    this.drawPerlinNoise();
  }

  // 模块5：对比分析
  initModule5() {
    this.drawComparison();
  }

  drawComparison() {
    this.drawWhiteNoiseCompare();
    this.drawPerlinNoiseCompare();
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
        const noise = perlinNoise2D(x * 0.05, y * 0.05, 3, 0.5);
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

// ==========================================
// 初始化
// ==========================================

document.addEventListener('DOMContentLoaded', () => {
  window.visualizationManager = new VisualizationManager();
  console.log('Visualization system initialized');
});
