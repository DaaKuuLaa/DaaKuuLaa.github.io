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

    // ESC 键关闭模态框
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') {
        this.closeModal('helpModal');
      }
    });

    // N 键折叠/展开大标题栏
    document.addEventListener('keydown', (e) => {
      if (e.key === 'n' || e.key === 'N') {
        // 如果不在输入框中，才触发折叠
        if (e.target.tagName !== 'INPUT' && e.target.tagName !== 'TEXTAREA') {
          this.toggleNavbarCollapse();
        }
      }
    });

    // 大标题栏折叠功能
    this.initializeNavbarCollapse();

    console.log('Event handlers initialized');
  }

  // 切换大标题栏折叠状态
  toggleNavbarCollapse() {
    const navbar = document.querySelector('.navbar');
    const collapseBtn = document.getElementById('navbarCollapseBtn');
    
    if (navbar && collapseBtn) {
      navbar.classList.toggle('collapsed');
      
      // 更新按钮图标
      const icon = collapseBtn.querySelector('i');
      if (navbar.classList.contains('collapsed')) {
        icon.classList.remove('fa-chevron-up');
        icon.classList.add('fa-chevron-down');
      } else {
        icon.classList.remove('fa-chevron-down');
        icon.classList.add('fa-chevron-up');
      }
      
      // 保存状态到 localStorage
      localStorage.setItem('navbarCollapsed', navbar.classList.contains('collapsed'));
    }
  }

  // 初始化大标题栏折叠功能
  initializeNavbarCollapse() {
    const navbar = document.querySelector('.navbar');
    const collapseBtn = document.getElementById('navbarCollapseBtn');
    
    if (collapseBtn && navbar) {
      collapseBtn.addEventListener('click', () => {
        this.toggleNavbarCollapse();
      });
      
      // 恢复上次状态
      const savedState = localStorage.getItem('navbarCollapsed');
      if (savedState === 'true') {
        navbar.classList.add('collapsed');
        const icon = collapseBtn.querySelector('i');
        icon.classList.remove('fa-chevron-up');
        icon.classList.add('fa-chevron-down');
      }
    }
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
// 银河幕布效果
// ==========================================

class GalaxyCurtain {
  constructor() {
    this.canvas = document.getElementById('starsCanvas');
    this.ctx = this.canvas ? this.canvas.getContext('2d') : null;
    this.stars = [];
    this.animationId = null;
    
    // 性能优化：状态管理
    this.isVisible = false; // 幕布是否可见
    this.isAnimating = false; // 是否正在运行动画
    this.lastFrameTime = 0; // 上一帧时间（用于节流）
    this.targetFPS = 30; // 目标帧率（降低到 30 FPS 以节省资源）
    this.frameInterval = 1000 / this.targetFPS; // 帧间隔
    
    this.initialize();
  }

  initialize() {
    if (!this.canvas || !this.ctx) return;

    // 设置画布尺寸
    this.resizeCanvas();
    
    // 生成星星（延迟生成，节省初始化时间）
    this.generateStars();
    
    // 性能优化：不立即启动动画，只在幕布展开时启动
    // 添加点击事件监听器
    this.addEventListeners();
    
    // 监听窗口大小变化
    window.addEventListener('resize', () => {
      this.resizeCanvas();
      // 如果幕布可见，重新生成星星以适应新尺寸
      if (this.isVisible) {
        this.generateStars();
      }
    });
  }

  resizeCanvas() {
    if (!this.canvas) return;
    this.canvas.width = window.innerWidth;
    this.canvas.height = window.innerHeight;
  }

  generateStars() {
    this.stars = [];
    // 增加星星密度：从 10000 改为 3000，密度增加约 3 倍
    const starCount = Math.floor(window.innerWidth * window.innerHeight / 3000);
    
    // 35°角的单位向量：cos(35°) ≈ 0.819, sin(35°) ≈ 0.574
    // 从右上到左下，所以 x 方向向左（负），y 方向向下（正）
    const angle = 35 * Math.PI / 180;
    const cosAngle = Math.cos(angle); // ≈ 0.819
    const sinAngle = Math.sin(angle); // ≈ 0.574
    
    for (let i = 0; i < starCount; i++) {
      this.stars.push({
        x: Math.random() * window.innerWidth,
        y: Math.random() * window.innerHeight,
        radius: Math.random() * 1.5 + 0.5,
        opacity: Math.random() * 0.8 + 0.2,
        // 降低速度：从 0.5 改为 0.2，使其更缓慢
        speed: Math.random() * 0.15 + 0.05,
        // 保存 35°方向的速度分量
        vx: -cosAngle * (Math.random() * 0.15 + 0.05), // 向左
        vy: sinAngle * (Math.random() * 0.15 + 0.05)   // 向下
      });
    }
  }

  animate(timestamp) {
    // 性能优化：检查幕布是否可见
    if (!this.isVisible || !this.ctx) {
      this.isAnimating = false;
      return;
    }
    
    // 性能优化：FPS 节流，限制帧率
    if (timestamp - this.lastFrameTime < this.frameInterval) {
      this.animationId = requestAnimationFrame((t) => this.animate(t));
      return;
    }
    
    this.lastFrameTime = timestamp;
    
    // 清空画布
    this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    
    // 性能优化：批量绘制星星（避免每颗星星都调用 save/restore）
    const stars = this.stars;
    const len = stars.length;
    
    for (let i = 0; i < len; i++) {
      const star = stars[i];
      
      // 按 35°角移动星星（从右上到左下）
      star.x += star.vx;
      star.y += star.vy;
      
      // 星星移出画布后重新生成（从右上边界）
      if (star.x < 0 || star.y > this.canvas.height) {
        star.x = window.innerWidth + Math.random() * 200; // 右侧外
        star.y = -Math.random() * 200; // 上侧外
      }
      
      // 绘制星星（不使用 save/restore，直接设置属性）
      this.ctx.globalAlpha = star.opacity;
      this.ctx.fillStyle = 'white';
      this.ctx.beginPath();
      this.ctx.arc(star.x, star.y, star.radius, 0, Math.PI * 2);
      this.ctx.fill();
    }
    
    // 重置 globalAlpha（避免影响其他绘制）
    this.ctx.globalAlpha = 1.0;
    
    // 继续动画
    this.animationId = requestAnimationFrame((t) => this.animate(t));
  }

  addEventListeners() {
    const thankYouMessage = document.getElementById('thankYouMessage');
    const galaxyCurtain = document.getElementById('galaxyCurtain');
    const theEndText = document.getElementById('theEndText');
    
    // 点击"谢谢观看"展开幕布
    if (thankYouMessage && galaxyCurtain && theEndText) {
      thankYouMessage.addEventListener('click', () => {
        this.showCurtain(galaxyCurtain, theEndText);
      });
    }
    
    // 添加 ESC 键监听
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' || e.keyCode === 27) {
        if (galaxyCurtain && galaxyCurtain.classList.contains('active')) {
          // 如果幕布已展开，则收起
          this.hideCurtain(galaxyCurtain, theEndText);
        } else {
          // 如果幕布已收起，则可以再次展开（通过触发谢谢观看的点击）
          if (thankYouMessage) {
            this.showCurtain(galaxyCurtain, theEndText);
          }
        }
      }
    });
  }
  
  // 展开幕布
  showCurtain(galaxyCurtain, theEndText) {
    if (!galaxyCurtain) return;
    
    // 展开幕布
    galaxyCurtain.classList.add('active');
    
    // 性能优化：标记幕布为可见状态，并启动动画
    this.isVisible = true;
    if (!this.isAnimating) {
      this.isAnimating = true;
      this.lastFrameTime = 0; // 重置帧时间
      this.animationId = requestAnimationFrame((t) => this.animate(t));
    }
    
    // 幕布展开后显示文字
    setTimeout(() => {
      if (theEndText) {
        theEndText.classList.add('fade-in');
        theEndText.classList.remove('fade-out');
        
        // 文字显示后淡出
        setTimeout(() => {
          theEndText.classList.add('fade-out');
        }, 2000);
      }
    }, 1000);
  }
  
  // 收起幕布
  hideCurtain(galaxyCurtain, theEndText) {
    if (!galaxyCurtain) return;
    
    // 收起幕布
    galaxyCurtain.classList.remove('active');
    
    // 性能优化：标记幕布为不可见，停止动画
    this.isVisible = false;
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
    this.isAnimating = false;
    
    // 重置文字状态
    if (theEndText) {
      theEndText.classList.remove('fade-in');
      theEndText.classList.remove('fade-out');
    }
  }

  destroy() {
    // 停止动画
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
    
    // 性能优化：重置所有状态
    this.isVisible = false;
    this.isAnimating = false;
    this.stars = [];
    
    // 清空画布
    if (this.ctx && this.canvas) {
      this.ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    }
  }
}

// 初始化银河幕布效果
document.addEventListener('DOMContentLoaded', () => {
  window.galaxyCurtain = new GalaxyCurtain();
});

// ==========================================
// 导出模块（如果支持ES6模块）
// ==========================================

if (typeof module !== 'undefined' && module.exports) {
  module.exports = PerlinNoiseDemo;
}

// ==========================================
// 语言切换功能
// ==========================================

// 语言代码数据
const languageCodes = {
    java: `// Java 实现的柏林噪声算法
public class PerlinNoise {
    private int[] permutation;
    private int[] p;
    
    public PerlinNoise() {
        // 初始化置换表
        permutation = new int[256];
        for (int i = 0; i < 256; i++) {
            permutation[i] = i;
        }
        // 打乱置换表
        for (int i = 255; i > 0; i--) {
            int j = (int)(Math.random() * (i + 1));
            int temp = permutation[i];
            permutation[i] = permutation[j];
            permutation[j] = temp;
        }
        // 扩展置换表
        p = new int[512];
        for (int i = 0; i < 512; i++) {
            p[i] = permutation[i & 255];
        }
    }
    
    private double fade(double t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    
    private double lerp(double t, double a, double b) {
        return a + t * (b - a);
    }
    
    private double grad(int hash, double x, double y, double z) {
        int h = hash & 15;
        double u = h < 8 ? x : y;
        double v = h < 4 ? y : h == 12 || h == 14 ? x : z;
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }
    
    public double noise(double x, double y, double z) {
        int X = (int)Math.floor(x) & 255;
        int Y = (int)Math.floor(y) & 255;
        int Z = (int)Math.floor(z) & 255;
        
        x -= Math.floor(x);
        y -= Math.floor(y);
        z -= Math.floor(z);
        
        double u = fade(x);
        double v = fade(y);
        double w = fade(z);
        
        int A = p[X] + Y;
        int AA = p[A] + Z;
        int AB = p[A + 1] + Z;
        int B = p[X + 1] + Y;
        int BA = p[B] + Z;
        int BB = p[B + 1] + Z;
        
        return lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z),
                               grad(p[BA], x-1, y, z)),
                       lerp(u, grad(p[AB], x, y-1, z),
                               grad(p[BB], x-1, y-1, z))),
               lerp(v, lerp(u, grad(p[AA+1], x, y, z-1),
                               grad(p[BA+1], x-1, y, z-1)),
                       lerp(u, grad(p[AB+1], x, y-1, z-1),
                               grad(p[BB+1], x-1, y-1, z-1)));
    }
}`,
    python: `# Python 实现的柏林噪声算法
import math
import random

class PerlinNoise:
    def __init__(self):
        # 初始化置换表
        self.permutation = list(range(256))
        random.shuffle(self.permutation)
        # 扩展置换表
        self.p = self.permutation * 2
    
    def fade(self, t):
        return t * t * t * (t * (t * 6 - 15) + 10)
    
    def lerp(self, t, a, b):
        return a + t * (b - a)
    
    def grad(self, hash_val, x, y, z):
        h = hash_val & 15
        u = x if h < 8 else y
        v = y if h < 4 else (x if h == 12 or h == 14 else z)
        return (u if (h & 1) == 0 else -u) + (v if (h & 2) == 0 else -v)
    
    def noise(self, x, y, z=0):
        X = int(math.floor(x)) & 255
        Y = int(math.floor(y)) & 255
        Z = int(math.floor(z)) & 255
        
        x -= math.floor(x)
        y -= math.floor(y)
        z -= math.floor(z)
        
        u = self.fade(x)
        v = self.fade(y)
        w = self.fade(z)
        
        A = self.p[X] + Y
        AA = self.p[A] + Z
        AB = self.p[A + 1] + Z
        B = self.p[X + 1] + Y
        BA = self.p[B] + Z
        BB = self.p[B + 1] + Z
        
        return self.lerp(w, self.lerp(v, self.lerp(u, self.grad(self.p[AA], x, y, z),
                                               self.grad(self.p[BA], x-1, y, z)),
                               self.lerp(u, self.grad(self.p[AB], x, y-1, z),
                                               self.grad(self.p[BB], x-1, y-1, z))),
               self.lerp(v, self.lerp(u, self.grad(self.p[AA+1], x, y, z-1),
                                               self.grad(self.p[BA+1], x-1, y, z-1)),
                               self.lerp(u, self.grad(self.p[AB+1], x, y-1, z-1),
                                               self.grad(self.p[BB+1], x-1, y-1, z-1)))

# 使用示例
if __name__ == "__main__":
    noise = PerlinNoise()
    # 生成噪声值
    value = noise.noise(0.5, 0.5, 0.0)
    print(f"噪声值: {value}")`,
    cpp: `// C++ 实现的柏林噪声算法
#include <iostream>
#include <vector>
#include <cmath>
#include <random>
#include <algorithm>

class PerlinNoise {
private:
    std::vector<int> permutation;
    std::vector<int> p;
    
public:
    PerlinNoise() {
        // 初始化置换表
        permutation.resize(256);
        for (int i = 0; i < 256; i++) {
            permutation[i] = i;
        }
        // 打乱置换表
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(permutation.begin(), permutation.end(), g);
        // 扩展置换表
        p.resize(512);
        for (int i = 0; i < 512; i++) {
            p[i] = permutation[i & 255];
        }
    }
    
    double fade(double t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    
    double lerp(double t, double a, double b) {
        return a + t * (b - a);
    }
    
    double grad(int hash, double x, double y, double z) {
        int h = hash & 15;
        double u = h < 8 ? x : y;
        double v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
    }
    
    double noise(double x, double y, double z = 0.0) {
        int X = static_cast<int>(std::floor(x)) & 255;
        int Y = static_cast<int>(std::floor(y)) & 255;
        int Z = static_cast<int>(std::floor(z)) & 255;
        
        x -= std::floor(x);
        y -= std::floor(y);
        z -= std::floor(z);
        
        double u = fade(x);
        double v = fade(y);
        double w = fade(z);
        
        int A = p[X] + Y;
        int AA = p[A] + Z;
        int AB = p[A + 1] + Z;
        int B = p[X + 1] + Y;
        int BA = p[B] + Z;
        int BB = p[B + 1] + Z;
        
        return lerp(w, lerp(v, lerp(u, grad(p[AA], x, y, z),
                               grad(p[BA], x-1, y, z)),
                       lerp(u, grad(p[AB], x, y-1, z),
                               grad(p[BB], x-1, y-1, z))),
               lerp(v, lerp(u, grad(p[AA+1], x, y, z-1),
                               grad(p[BA+1], x-1, y, z-1)),
                       lerp(u, grad(p[AB+1], x, y-1, z-1),
                               grad(p[BB+1], x-1, y-1, z-1)));
    }
};

// 使用示例
int main() {
    PerlinNoise noise;
    // 生成噪声值
    double value = noise.noise(0.5, 0.5, 0.0);
    std::cout << "噪声值: " << value << std::endl;
    return 0;
}`
};

// 语法高亮函数
function highlightCode(code, lang) {
    // 转义HTML特殊字符
    code = code.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
    
    // 根据语言进行语法高亮
    if (lang === 'java') {
        // Java语法高亮
        code = code
            // 注释
            .replace(/(\/\/.*$)/gm, '<span class="code-comment">$1</span>')
            // 关键字
            .replace(/\b(public|private|protected|class|int|double|float|boolean|void|return|for|while|if|else|new|static|final)\b/g, '<span class="code-keyword">$1</span>')
            // 字符串
            .replace(/"([^"]*)"/g, '<span class="code-string">"$1"</span>')
            // 数字
            .replace(/\b\d+(\.\d+)?\b/g, '<span class="code-number">$1</span>');
    } else if (lang === 'python') {
        // Python语法高亮
        code = code
            // 注释
            .replace(/(#.*$)/gm, '<span class="code-comment">$1</span>')
            // 关键字
            .replace(/\b(def|class|import|from|if|elif|else|for|while|return|True|False|None|pass|break|continue)\b/g, '<span class="code-keyword">$1</span>')
            // 字符串
            .replace(/"([^"]*)"/g, '<span class="code-string">"$1"</span>')
            .replace(/'([^']*)'/g, '<span class="code-string">\'$1\'</span>')
            // 数字
            .replace(/\b\d+(\.\d+)?\b/g, '<span class="code-number">$1</span>');
    } else if (lang === 'cpp') {
        // C++语法高亮
        code = code
            // 注释
            .replace(/(\/\/.*$)/gm, '<span class="code-comment">$1</span>')
            // 关键字
            .replace(/\b(int|double|float|bool|void|return|for|while|if|else|class|public|private|protected|namespace|using|std|cout|cin)\b/g, '<span class="code-keyword">$1</span>')
            // 字符串
            .replace(/"([^"]*)"/g, '<span class="code-string">"$1"</span>')
            // 数字
            .replace(/\b\d+(\.\d+)?\b/g, '<span class="code-number">$1</span>');
    }
    
    return code;
}

// 初始化语言切换功能
document.addEventListener('DOMContentLoaded', function() {
    const languageOptions = document.querySelectorAll('.language-option');
    const codeContent = document.getElementById('code-content');
    const slider = document.querySelector('.language-slider');
    
    if (languageOptions && codeContent && slider) {
        // 设置初始代码内容和语法高亮
        codeContent.innerHTML = highlightCode(languageCodes['java'], 'java');
        // 设置初始滑块位置
        updateSliderPosition('java');
        
        // 添加点击事件监听器
        languageOptions.forEach(option => {
            option.addEventListener('click', function() {
                const lang = this.dataset.lang;
                
                // 更新激活状态
                languageOptions.forEach(opt => opt.classList.remove('active'));
                this.classList.add('active');
                
                // 更新代码内容并添加语法高亮
                codeContent.innerHTML = highlightCode(languageCodes[lang], lang);
                
                // 更新滑块位置
                updateSliderPosition(lang);
            });
        });
    }
    
    function updateSliderPosition(lang) {
        const option = document.querySelector(`.language-option[data-lang="${lang}"]`);
        if (option && slider) {
            const optionRect = option.getBoundingClientRect();
            const switcherRect = option.parentElement.getBoundingClientRect();
            
            const left = optionRect.left - switcherRect.left;
            const width = optionRect.width;
            
            slider.style.width = width + 'px';
            slider.style.transform = `translateX(${left}px)`;
        }
    }
});
