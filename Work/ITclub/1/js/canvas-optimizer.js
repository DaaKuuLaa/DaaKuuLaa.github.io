// ==========================================
// Canvas渲染性能优化模块
// Canvas Rendering Performance Optimizer
// ==========================================

/**
 * Canvas性能优化管理器
 * 负责优化Canvas渲染，包括缓存、节流、去抖动等
 */
class CanvasOptimizer {
  constructor() {
    this.canvasCache = new Map();
    this.renderQueue = new Map();
    this.pendingRenders = new Map();
    this.cacheEnabled = true;
    this.maxCacheSize = 100;
    this.renderThrottleMs = 16; // ~60fps
    this.stats = {
      cacheHits: 0,
      cacheMisses: 0,
      renders: 0,
      skippedRenders: 0
    };
    
    // 性能监控
    this.performance = {
      lastFrameTime: 0,
      frameTimes: [],
      fps: 0
    };
    
    // 启动性能监控
    this.startPerformanceMonitoring();
  }
  
  /**
   * 开始性能监控
   */
  startPerformanceMonitoring() {
    // 用 rAF 计帧，setInterval 每秒统计一次，避免每帧都运行额外回调
    let frameCount = 0;
    const countFrame = () => {
      frameCount++;
      this._monitorRafId = requestAnimationFrame(countFrame);
    };
    this._monitorRafId = requestAnimationFrame(countFrame);

    this._monitorIntervalId = setInterval(() => {
      this.performance.fps = frameCount;
      // 保持最近30次采样的帧时间（每次约1000ms/fps）
      if (frameCount > 0) {
        const approxFrameTime = 1000 / frameCount;
        this.performance.frameTimes.push(approxFrameTime);
        if (this.performance.frameTimes.length > 30) {
          this.performance.frameTimes.shift();
        }
      }
      frameCount = 0;
    }, 1000);
  }

  /**
   * 停止性能监控
   */
  stopPerformanceMonitoring() {
    if (this._monitorRafId) {
      cancelAnimationFrame(this._monitorRafId);
      this._monitorRafId = null;
    }
    if (this._monitorIntervalId) {
      clearInterval(this._monitorIntervalId);
      this._monitorIntervalId = null;
    }
  }
  
  /**
   * 获取性能统计
   */
  getPerformanceStats() {
    return {
      fps: this.performance.fps,
      cacheHitRate: this.stats.cacheHits + this.stats.cacheMisses > 0 ? 
        (this.stats.cacheHits / (this.stats.cacheHits + this.stats.cacheMisses) * 100).toFixed(1) + '%' : '0%',
      cacheHits: this.stats.cacheHits,
      cacheMisses: this.stats.cacheMisses,
      renders: this.stats.renders,
      skippedRenders: this.stats.skippedRenders,
      cacheSize: this.canvasCache.size
    };
  }
  
  /**
   * 清空缓存
   */
  clearCache() {
    this.canvasCache.clear();
    this.renderQueue.clear();
    this.pendingRenders.clear();
    this.stats.cacheHits = 0;
    this.stats.cacheMisses = 0;
    this.stats.renders = 0;
    this.stats.skippedRenders = 0;
    console.log('Canvas缓存已清空');
  }
  
  /**
   * 生成Canvas缓存的键
   */
  generateCacheKey(canvasId, params = {}) {
    const sortedParams = Object.keys(params).sort().reduce((obj, key) => {
      obj[key] = params[key];
      return obj;
    }, {});
    
    return `${canvasId}:${JSON.stringify(sortedParams)}`;
  }
  
  /**
   * 从缓存获取Canvas渲染结果
   */
  getFromCache(canvasId, params = {}) {
    if (!this.cacheEnabled) return null;
    
    const cacheKey = this.generateCacheKey(canvasId, params);
    const cached = this.canvasCache.get(cacheKey);
    
    if (cached) {
      this.stats.cacheHits++;
      return cached;
    }
    
    this.stats.cacheMisses++;
    return null;
  }
  
  /**
   * 保存Canvas渲染结果到缓存
   */
  saveToCache(canvasId, params = {}, imageData) {
    if (!this.cacheEnabled) return;
    
    // 检查缓存大小，避免内存泄漏
    if (this.canvasCache.size >= this.maxCacheSize) {
      // 移除最旧的缓存项
      const firstKey = this.canvasCache.keys().next().value;
      if (firstKey) {
        this.canvasCache.delete(firstKey);
      }
    }
    
    const cacheKey = this.generateCacheKey(canvasId, params);
    this.canvasCache.set(cacheKey, imageData);
  }
  
  /**
   * 节流渲染函数
   */
  throttleRender(canvasId, renderFunc, params = {}, immediate = false) {
    if (!this.pendingRenders.has(canvasId)) {
      this.pendingRenders.set(canvasId, null);
    }
    
    // 如果有未执行的渲染，跳过新的渲染
    if (this.pendingRenders.get(canvasId)) {
      this.stats.skippedRenders++;
      return;
    }
    
    const doRender = () => {
      this.pendingRenders.set(canvasId, null);
      renderFunc();
      this.stats.renders++;
    };
    
    if (immediate) {
      doRender();
    } else {
      const timerId = setTimeout(() => {
        if (this.pendingRenders.get(canvasId) === timerId) {
          doRender();
        }
      }, this.renderThrottleMs);
      
      this.pendingRenders.set(canvasId, timerId);
    }
  }
  
  /**
   * 去抖动渲染函数
   */
  debounceRender(canvasId, renderFunc, delay = this.renderThrottleMs) {
    if (!this.pendingRenders.has(canvasId)) {
      this.pendingRenders.set(canvasId, null);
    }
    
    clearTimeout(this.pendingRenders.get(canvasId));
    
    const timerId = setTimeout(() => {
      if (this.pendingRenders.get(canvasId) === timerId) {
        this.pendingRenders.set(canvasId, null);
        renderFunc();
        this.stats.renders++;
      }
    }, delay);
    
    this.pendingRenders.set(canvasId, timerId);
  }
  
  /**
   * 优化渲染循环
   */
  optimizedRenderLoop(canvasId, renderFunc, params = {}) {
    // 首先检查缓存
    const cached = this.getFromCache(canvasId, params);
    if (cached) {
      return cached;
    }
    
    // 使用节流渲染
    this.throttleRender(canvasId, () => {
      const result = renderFunc();
      if (result) {
        this.saveToCache(canvasId, params, result);
      }
    }, params, false);
    
    return null;
  }
  
  /**
   * 创建离屏Canvas进行预渲染
   */
  createOffscreenCanvas(width, height) {
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    return canvas;
  }
  
  /**
   * 批量渲染优化
   */
  batchRender(canvasElements, renderFunc, batchSize = 3) {
    const results = [];
    const total = canvasElements.length;
    
    // 分批渲染，避免阻塞主线程
    const renderBatch = (startIndex) => {
      const endIndex = Math.min(startIndex + batchSize, total);
      
      for (let i = startIndex; i < endIndex; i++) {
        const canvas = canvasElements[i];
        if (canvas) {
          results[i] = renderFunc(canvas, i);
        }
      }
      
      if (endIndex < total) {
        // 使用requestAnimationFrame进行下一批渲染
        requestAnimationFrame(() => {
          renderBatch(endIndex);
        });
      }
    };
    
    renderBatch(0);
    return results;
  }
  
  /**
   * 优化图像数据处理（使用TypedArrays）
   */
  createOptimizedImageData(width, height, fillFunc) {
    // 创建TypedArray以提高性能
    const data = new Uint8ClampedArray(width * height * 4);
    
    // 预分配颜色查找表（如果需要）
    const colorTable = this.createColorTable();
    
    // 逐像素填充（优化版本）
    for (let y = 0; y < height; y++) {
      for (let x = 0; x < width; x++) {
        const index = (y * width + x) * 4;
        const color = fillFunc(x, y, colorTable);
        
        data[index] = color.r;
        data[index + 1] = color.g;
        data[index + 2] = color.b;
        data[index + 3] = 255; // 完全不透明
      }
    }
    
    return new ImageData(data, width, height);
  }
  
  /**
   * 创建颜色查找表（预计算）
   */
  createColorTable() {
    const table = {
      gradient: [],
      terrain: []
    };
    
    // 创建渐变颜色表（256级）
    for (let i = 0; i < 256; i++) {
      const value = i / 255;
      
      // 蓝紫渐变
      table.gradient[i] = {
        r: Math.floor(value * 200),
        g: Math.floor(value * 100),
        b: Math.floor(value * 255)
      };
      
      // 地形颜色
      if (value < 0.3) {
        table.terrain[i] = { r: 30, g: 60, b: 150 }; // 深蓝（水域）
      } else if (value < 0.4) {
        table.terrain[i] = { r: 50, g: 120, b: 180 }; // 浅蓝（浅水）
      } else if (value < 0.5) {
        table.terrain[i] = { r: 210, g: 190, b: 140 }; // 沙滩色
      } else if (value < 0.7) {
        table.terrain[i] = { r: 60, g: 120, b: 50 }; // 绿色（草地）
      } else if (value < 0.85) {
        table.terrain[i] = { r: 100, g: 90, b: 80 }; // 棕色（山地）
      } else {
        table.terrain[i] = { r: 240, g: 245, b: 255 }; // 白色（雪山）
      }
    }
    
    return table;
  }
  
  /**
   * 优化噪声计算
   */
  optimizedPerlinNoise(width, height, frequency = 0.05, octaves = 3) {
    // 预计算网格以提高性能
    const grid = this.precomputePerlinGrid(width, height, frequency);
    
    // 创建优化噪声函数
    const getNoise = (x, y) => {
      // 使用预计算网格
      const gridX = Math.floor(x / grid.cellSize);
      const gridY = Math.floor(y / grid.cellSize);
      
      // 边界检查
      if (gridX < 0 || gridX >= grid.cols || gridY < 0 || gridY >= grid.rows) {
        return 0;
      }
      
      const gradient = grid.gradients[gridY * grid.cols + gridX];
      const distX = x - gridX * grid.cellSize;
      const distY = y - gridY * grid.cellSize;
      
      return gradient.x * distX + gradient.y * distY;
    };
    
    return { getNoise, grid };
  }
  
  /**
   * 预计算柏林噪声网格
   */
  precomputePerlinGrid(width, height, frequency) {
    const cellSize = Math.max(1, Math.floor(1 / frequency));
    const cols = Math.ceil(width / cellSize) + 1;
    const rows = Math.ceil(height / cellSize) + 1;
    const gradients = new Array(cols * rows);
    
    // 预计算随机梯度
    for (let y = 0; y < rows; y++) {
      for (let x = 0; x < cols; x++) {
        const angle = Math.random() * Math.PI * 2;
        gradients[y * cols + x] = {
          x: Math.cos(angle),
          y: Math.sin(angle)
        };
      }
    }
    
    return { cellSize, cols, rows, gradients };
  }
  
  /**
   * 性能建议
   */
  getPerformanceSuggestions() {
    const suggestions = [];
    
    // 根据性能数据提供建议
    if (this.performance.fps < 30) {
      suggestions.push('FPS较低，建议减少同时渲染的Canvas数量');
    }
    
    if (this.stats.cacheMisses > this.stats.cacheHits * 2) {
      suggestions.push('缓存命中率较低，考虑增加缓存策略');
    }
    
    if (this.stats.skippedRenders > this.stats.renders * 0.3) {
      suggestions.push('跳过的渲染过多，可适当降低节流阈值');
    }
    
    if (this.canvasCache.size >= this.maxCacheSize * 0.8) {
      suggestions.push('缓存接近上限，考虑清理旧缓存');
    }
    
    return suggestions.length > 0 ? suggestions : ['性能良好，无需调整'];
  }
  
  /**
   * 启用/禁用缓存
   */
  setCacheEnabled(enabled) {
    this.cacheEnabled = enabled;
    console.log(`Canvas缓存${enabled ? '启用' : '禁用'}`);
  }
  
  /**
   * 设置最大缓存大小
   */
  setMaxCacheSize(size) {
    this.maxCacheSize = size;
    if (this.canvasCache.size > size) {
      // 清理多余缓存
      const keys = Array.from(this.canvasCache.keys());
      for (let i = 0; i < keys.length - size; i++) {
        this.canvasCache.delete(keys[i]);
      }
    }
  }
  
  /**
   * 设置渲染节流时间
   */
  setRenderThrottle(ms) {
    this.renderThrottleMs = ms;
  }
  
  // ==========================================
  // 新增性能管理方法
  // ==========================================
  
  /**
   * 设置目标帧率
   */
  setTargetFPS(fps) {
    const safeFPS = Math.max(15, Math.min(120, fps));
    this.renderThrottleMs = Math.floor(1000 / safeFPS);
    console.log(`目标帧率设置为: ${safeFPS} FPS (间隔: ${this.renderThrottleMs}ms)`);
  }
  
  /**
   * 获取目标帧率
   */
  getTargetFPS() {
    return Math.round(1000 / this.renderThrottleMs);
  }
  
  /**
   * 获取平均帧率
   */
  getAverageFPS() {
    return this.performance.fps;
  }
  
  /**
   * 获取平均渲染时间
   */
  getAverageRenderTime() {
    if (this.performance.frameTimes.length === 0) return 0;
    const avgTime = this.performance.frameTimes.reduce((a, b) => a + b) / this.performance.frameTimes.length;
    return avgTime;
  }
  
  /**
   * 设置质量级别
   */
  setQuality(level) {
    const validLevels = ['low', 'medium', 'high'];
    if (!validLevels.includes(level)) {
      console.warn(`无效的质量级别: ${level}, 使用默认值: medium`);
      level = 'medium';
    }
    
    this.quality = level;
    
    // 根据质量级别调整参数
    switch (level) {
      case 'low':
        this.setTargetFPS(30);
        this.setMaxCacheSize(50);
        this.setRenderThrottle(33); // 30fps
        break;
      case 'medium':
        this.setTargetFPS(45);
        this.setMaxCacheSize(100);
        this.setRenderThrottle(22); // ~45fps
        break;
      case 'high':
        this.setTargetFPS(60);
        this.setMaxCacheSize(150);
        this.setRenderThrottle(16); // 60fps
        break;
    }
    
    console.log(`质量级别设置为: ${level}`);
  }
  
  /**
   * 获取内存使用情况
   */
  getMemoryUsage() {
    let memory = 0;
    
    // 估算缓存内存使用（每个像素4字节）
    for (const [key, imageData] of this.canvasCache) {
      if (imageData && imageData.data) {
        memory += imageData.data.length;
      }
    }
    
    // 转换为MB
    return Math.round(memory / 1024 / 1024 * 100) / 100;
  }
  
  /**
   * 获取性能面板数据
   */
  getPerformancePanelData() {
    const stats = this.getPerformanceStats();
    const fps = this.performance.fps;
    const targetFPS = this.getTargetFPS();
    
    // 计算帧率历史
    this.fpsHistory = this.fpsHistory || [];
    this.fpsHistory.push(fps);
    if (this.fpsHistory.length > 60) this.fpsHistory.shift();
    
    return {
      currentFPS: fps,
      targetFPS: targetFPS,
      avgFPS: fps, // 直接使用当前FPS
      renderTime: this.getAverageRenderTime(),
      cacheHitRate: stats.cacheHitRate,
      memoryUsage: this.getMemoryUsage(),
      qualityLevel: this.quality || 'medium',
      fpsHistory: [...this.fpsHistory],
      timestamp: Date.now(),
      cacheHits: stats.cacheHits,
      cacheMisses: stats.cacheMisses,
      renders: stats.renders,
      skippedRenders: stats.skippedRenders,
      cacheSize: stats.cacheSize
    };
  }
  
  /**
   * 清理内存
   */
  clearMemory() {
    const before = this.getMemoryUsage();
    this.clearCache();
    const after = this.getMemoryUsage();
    console.log(`内存已清理, 释放: ${(before - after).toFixed(2)} MB`);
  }
  
  /**
   * 启用/禁用特定优化
   */
  setOptimization(type, enabled) {
    switch (type) {
      case 'caching':
        this.setCacheEnabled(enabled);
        break;
      case 'throttling':
        if (!enabled) {
          this.renderThrottleMs = 0; // 禁用节流
        } else {
          this.renderThrottleMs = 16; // 默认60fps
        }
        break;
      case 'precomputation':
        this.precomputationEnabled = enabled;
        break;
      default:
        console.warn(`未知的优化类型: ${type}`);
    }
  }
  
  /**
   * 生成性能报告
   */
  generatePerformanceReport() {
    const panelData = this.getPerformancePanelData();
    const suggestions = this.getPerformanceSuggestions();
    
    return {
      summary: {
        performanceScore: this.calculatePerformanceScore(),
        overallStatus: panelData.currentFPS >= 45 ? '优秀' : panelData.currentFPS >= 30 ? '良好' : '需优化'
      },
      metrics: panelData,
      suggestions: suggestions,
      recommendations: this.getDetailedRecommendations(),
      timestamp: new Date().toISOString()
    };
  }
  
  /**
   * 计算性能评分 (0-100)
   */
  calculatePerformanceScore() {
    const fps = this.performance.fps;
    const cacheRate = this.stats.cacheHits / (this.stats.cacheHits + this.stats.cacheMisses) || 0;
    
    let score = 50; // 基础分
    
    // FPS贡献 (0-30分)
    if (fps >= 55) score += 30;
    else if (fps >= 45) score += 20;
    else if (fps >= 30) score += 10;
    else if (fps >= 20) score += 5;
    
    // 缓存效率贡献 (0-20分)
    score += Math.min(20, cacheRate * 20);
    
    return Math.min(100, Math.max(0, score));
  }
  
  /**
   * 获取详细建议
   */
  getDetailedRecommendations() {
    const recommendations = [];
    const fps = this.performance.fps;
    const cacheRate = this.stats.cacheHits / (this.stats.cacheHits + this.stats.cacheMisses) || 0;
    
    if (fps < 30) {
      recommendations.push({
        priority: '高',
        action: '降低渲染质量',
        description: '当前帧率过低，建议降低Canvas渲染分辨率或减少粒子数量',
        impact: '帧率提升40-60%',
        implementation: '调用setQuality(\'low\')或手动降低视觉复杂度'
      });
    }
    
    if (cacheRate < 0.3) {
      recommendations.push({
        priority: '中',
        action: '优化缓存策略',
        description: '缓存命中率较低，说明渲染内容变化频繁',
        impact: '减少重复计算，降低CPU使用率',
        implementation: '检查缓存键生成逻辑，确保相似渲染能被缓存'
      });
    }
    
    if (this.stats.skippedRenders > this.stats.renders * 0.5) {
      recommendations.push({
        priority: '低',
        action: '调整节流策略',
        description: '过多的渲染被跳过，可能导致响应延迟',
        impact: '改善交互响应性',
        implementation: '适当减少节流时间或调整渲染优先级'
      });
    }
    
    if (this.getMemoryUsage() > 50) {
      recommendations.push({
        priority: '中',
        action: '清理缓存内存',
        description: '内存占用较高，可能影响页面性能',
        impact: '释放内存，提高页面稳定性',
        implementation: '调用clearMemory()定期清理或减少maxCacheSize'
      });
    }
    
    // 添加性能优秀的反馈
    if (fps >= 55 && cacheRate >= 0.6 && this.getMemoryUsage() < 30) {
      recommendations.push({
        priority: '信息',
        action: '性能优秀',
        description: '当前性能表现良好，无需额外优化',
        impact: '维持当前配置',
        implementation: '继续监控性能指标'
      });
    }
    
    return recommendations;
  }
  
  /**
   * 自动性能优化
   */
  autoOptimize() {
    const fps = this.performance.fps;
    const memoryUsage = this.getMemoryUsage();
    
    console.log('开始自动性能优化分析...');
    console.log(`当前FPS: ${fps}, 内存使用: ${memoryUsage}MB`);
    
    if (fps < 25) {
      console.log('检测到严重性能问题，应用最大优化');
      this.setQuality('low');
      this.setMaxCacheSize(30);
      this.setRenderThrottle(40); // 25fps
    } else if (fps < 40) {
      console.log('检测到中等性能问题，应用平衡优化');
      this.setQuality('medium');
      this.setMaxCacheSize(80);
    } else if (fps >= 55) {
      console.log('性能优秀，可适当提高质量');
      this.setQuality('high');
    }
    
    if (memoryUsage > 100) {
      console.log('内存使用过高，清理缓存');
      this.clearMemory();
    }
    
    return this.generatePerformanceReport();
  }
}

// ==========================================
// 全局导出
// ==========================================

window.CanvasOptimizer = CanvasOptimizer;

// 快捷访问
window.createCanvasOptimizer = () => new CanvasOptimizer();

console.log('Canvas渲染性能优化模块已加载');

// ==========================================
// 性能监控面板
// ==========================================

/**
 * 创建性能监控面板（可选）
 */
function createPerformancePanel() {
  if (document.getElementById('performance-panel')) return;
  
  const panel = document.createElement('div');
  panel.id = 'performance-panel';
  panel.style.cssText = `
    position: fixed;
    bottom: 20px;
    right: 20px;
    background: rgba(0, 0, 0, 0.8);
    color: #00ff00;
    padding: 10px;
    border-radius: 5px;
    font-family: monospace;
    font-size: 12px;
    z-index: 10000;
    border: 1px solid #00ff00;
    max-width: 300px;
    display: none;
  `;
  
  panel.innerHTML = `
    <div style="display: flex; justify-content: space-between; margin-bottom: 5px;">
      <strong>性能监控</strong>
      <button onclick="togglePerformancePanel()" style="background: none; border: none; color: #00ff00; cursor: pointer;">×</button>
    </div>
    <div id="performance-stats"></div>
    <div id="performance-suggestions" style="margin-top: 10px; font-size: 10px; color: #ffaa00;"></div>
  `;
  
  document.body.appendChild(panel);
  
  // 更新性能数据显示
  const updatePerformanceDisplay = () => {
    if (window.canvasOptimizer) {
      const stats = window.canvasOptimizer.getPerformanceStats();
      const suggestions = window.canvasOptimizer.getPerformanceSuggestions();
      
      const statsEl = document.getElementById('performance-stats');
      const suggestionsEl = document.getElementById('performance-suggestions');
      
      statsEl.innerHTML = `
        FPS: ${stats.fps}<br>
        缓存命中率: ${stats.cacheHitRate}<br>
        缓存命中: ${stats.cacheHits}<br>
        缓存未命中: ${stats.cacheMisses}<br>
        渲染次数: ${stats.renders}<br>
        跳过渲染: ${stats.skippedRenders}<br>
        缓存大小: ${stats.cacheSize}
      `;
      
      suggestionsEl.innerHTML = suggestions.map(s => `• ${s}`).join('<br>');
    }
  };
  
  // 每2秒更新一次
  setInterval(updatePerformanceDisplay, 2000);
}

window.togglePerformancePanel = function() {
  const panel = document.getElementById('performance-panel');
  if (panel) {
    panel.style.display = panel.style.display === 'none' ? 'block' : 'none';
  }
};

// 只在开发模式下显示性能面板
if (window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1') {
  createPerformancePanel();
}