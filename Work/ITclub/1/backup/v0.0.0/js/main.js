// ==========================================
// 主入口文件
// Main Entry Point
// ==========================================

class PerlinNoiseDemo {
  constructor() {
    this.initialized = false;
    this.modules = [];
    this.currentModule = 1;
    this.init();
  }

  init() {
    // 等待DOM加载完成
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', () => this.initialize());
    } else {
      this.initialize();
    }
  }

  initialize() {
    try {
      // 隐藏加载动画
      this.hideLoader();

      // 初始化各个模块
      this.initializeNavigation();
      this.initializeVisualizations();
      this.initializeEventHandlers();

      // 显示第一模块
      this.showModule(1);

      this.initialized = true;
      console.log('Perlin Noise Demo initialized successfully');

    } catch (error) {
      console.error('Failed to initialize Perlin Noise Demo:', error);
      this.showError('初始化失败，请刷新页面重试');
    }
  }

  // 隐藏加载动画
  hideLoader() {
    const loader = document.getElementById('loader');
    if (loader) {
      setTimeout(() => {
        loader.classList.add('hidden');
        setTimeout(() => {
          loader.style.display = 'none';
        }, 500);
      }, 1000);
    }
  }

  // 初始化导航系统
  initializeNavigation() {
    if (window.navigationController) {
      console.log('Navigation system ready');
    } else {
      console.warn('Navigation system not available');
    }
  }

  // 初始化可视化系统
  initializeVisualizations() {
    if (window.visualizationManager) {
      console.log('Visualization system ready');
    } else {
      console.warn('Visualization system not available');
    }
  }

  // 初始化事件处理器
  initializeEventHandlers() {
    // 帮助模态框
    const helpModal = document.getElementById('helpModal');
    const helpBtn = document.querySelector('[onclick="showHelp()"]');
    const closeModalBtn = document.querySelector('.modal-close');

    if (closeModalBtn) {
      closeModalBtn.addEventListener('click', () => this.closeModal('helpModal'));
    }

    // 点击模态框外部关闭
    if (helpModal) {
      helpModal.addEventListener('click', (e) => {
        if (e.target === helpModal) {
          this.closeModal('helpModal');
        }
      });
    }

    // ESC键关闭模态框
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        this.closeModal('helpModal');
      }
    });

    console.log('Event handlers initialized');
  }

  // 显示指定模块
  showModule(moduleId) {
    if (window.navigationController) {
      window.navigationController.goToModule(moduleId);
    }
    this.currentModule = moduleId;
  }

  // 显示帮助模态框
  showHelp() {
    const modal = document.getElementById('helpModal');
    if (modal) {
      modal.classList.add('active');
    }
  }

  // 关闭模态框
  closeModal(modalId) {
    const modal = document.getElementById(modalId);
    if (modal) {
      modal.classList.remove('active');
    }
  }

  // 显示错误信息
  showError(message) {
    const errorDiv = document.createElement('div');
    errorDiv.className = 'error-message';
    errorDiv.style.cssText = `
      position: fixed;
      top: 20px;
      left: 50%;
      transform: translateX(-50%);
      background: #ff5252;
      color: white;
      padding: 16px 24px;
      border-radius: 8px;
      z-index: 9999;
      box-shadow: 0 4px 16px rgba(255, 82, 82, 0.3);
      font-size: 1rem;
    `;
    errorDiv.textContent = message;
    
    document.body.appendChild(errorDiv);
    
    setTimeout(() => {
      errorDiv.remove();
    }, 5000);
  }

  // 显示成功信息
  showSuccess(message) {
    const successDiv = document.createElement('div');
    successDiv.className = 'success-message';
    successDiv.style.cssText = `
      position: fixed;
      top: 20px;
      left: 50%;
      transform: translateX(-50%);
      background: #00c853;
      color: white;
      padding: 16px 24px;
      border-radius: 8px;
      z-index: 9999;
      box-shadow: 0 4px 16px rgba(0, 200, 83, 0.3);
      font-size: 1rem;
    `;
    successDiv.textContent = message;
    
    document.body.appendChild(successDiv);
    
    setTimeout(() => {
      successDiv.remove();
    }, 3000);
  }

  // 重置所有状态
  reset() {
    // 重置噪声种子
    if (window.setNoiseSeed) {
      window.setNoiseSeed(42);
    }
    
    // 重置导航
    if (window.navigationController) {
      window.navigationController.goToModule(1);
    }
    
    // 重置可视化
    if (window.visualizationManager) {
      window.visualizationManager.stopAllAnimations();
    }
    
    console.log('Demo reset');
  }

  // 获取当前状态
  getState() {
    return {
      currentModule: this.currentModule,
      initialized: this.initialized,
      timestamp: new Date().toISOString()
    };
  }

  // 销毁实例
  destroy() {
    // 清理事件监听器
    // 停止动画
    if (window.visualizationManager) {
      window.visualizationManager.stopAllAnimations();
    }
    
    console.log('Demo destroyed');
  }
}

// ==========================================
// 全局帮助函数
// ==========================================

// 显示帮助
function showHelp() {
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.showHelp();
  }
}

// 关闭模态框
function closeModal(modalId) {
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.closeModal(modalId);
  }
}

// 重新开始演示
function restartDemo() {
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.reset();
  }
}

// 显示成功消息
function showSuccess(message) {
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.showSuccess(message);
  }
}

// 显示错误消息
function showError(message) {
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.showError(message);
  }
}

// ==========================================
// 性能监控
// ==========================================

// 监控页面加载性能
window.addEventListener('load', () => {
  const loadTime = performance.now();
  console.log(`Page loaded in ${loadTime.toFixed(2)}ms`);
  
  // 监控内存使用（如果可用）
  if (performance.memory) {
    console.log('Memory usage:', {
      usedJSHeapSize: Math.round(performance.memory.usedJSHeapSize / 1048576) + ' MB',
      totalJSHeapSize: Math.round(performance.memory.totalJSHeapSize / 1048576) + ' MB'
    });
  }
});

// 监控动画帧率
let frameCount = 0;
let lastTime = performance.now();

function countFPS() {
  frameCount++;
  const currentTime = performance.now();
  
  if (currentTime - lastTime >= 1000) {
    console.log(`FPS: ${frameCount}`);
    frameCount = 0;
    lastTime = currentTime;
  }
  
  requestAnimationFrame(countFPS);
}

// 只在开发模式下启用FPS计数
if (window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1') {
  countFPS();
}

// ==========================================
// 错误处理
// ==========================================

// 全局错误处理
window.addEventListener('error', (event) => {
  console.error('Global error:', event.error);
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.showError('发生错误: ' + event.message);
  }
});

// 未处理的Promise拒绝
window.addEventListener('unhandledrejection', (event) => {
  console.error('Unhandled promise rejection:', event.reason);
  if (window.perlinNoiseDemo) {
    window.perlinNoiseDemo.showError('操作失败: ' + event.reason);
  }
});

// ==========================================
// 初始化应用
// ==========================================

// 创建全局实例
document.addEventListener('DOMContentLoaded', () => {
  window.perlinNoiseDemo = new PerlinNoiseDemo();
  
  // 添加到控制台以便调试
  console.log('%cPerlin Noise Demo Loaded', 'color: #2962ff; font-size: 16px; font-weight: bold;');
  console.log('Available commands:');
  console.log('- perlinNoiseDemo.showHelp() - Show help modal');
  console.log('- perlinNoiseDemo.reset() - Reset demo');
  console.log('- perlinNoiseDemo.getState() - Get current state');
  console.log('- restartDemo() - Restart demo');
});

// ==========================================
// 导出模块（如果支持ES6模块）
// ==========================================

if (typeof module !== 'undefined' && module.exports) {
  module.exports = PerlinNoiseDemo;
}
