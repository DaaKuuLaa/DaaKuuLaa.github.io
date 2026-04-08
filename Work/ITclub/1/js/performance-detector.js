// ==========================================
// 性能检测与自适应优化模块（安全精简版）
// ==========================================

class PerformanceDetector {
  constructor() {
    this.deviceInfo = this.detectDevice();
    this.optimizationLevel = this.determineLevel();
    this.applyOptimizations();
    console.log('性能检测: ' + this.deviceInfo.type + ', 级别: ' + this.optimizationLevel);
  }

  detectDevice() {
    const ua = navigator.userAgent;
    const isMobile = /Mobi|Android|iPhone|iPad|iPod/i.test(ua);
    const cpuCores = navigator.hardwareConcurrency || 4;
    const hasWebGL = !!document.createElement('canvas').getContext('webgl');
    return { isMobile, cpuCores, hasWebGL, type: isMobile ? 'mobile' : 'desktop' };
  }

  determineLevel() {
    const { isMobile, cpuCores, hasWebGL } = this.deviceInfo;
    if (!hasWebGL || isMobile) return 'low';
    if (cpuCores >= 8) return 'high';
    if (cpuCores >= 4) return 'medium';
    return 'low';
  }

  applyOptimizations() {
    const root = document.documentElement;

    // 设备类型 CSS 类
    root.classList.add(this.deviceInfo.isMobile ? 'device-mobile' : 'device-desktop');
    root.classList.add('performance-' + this.optimizationLevel);
    if (!this.deviceInfo.hasWebGL) root.classList.add('no-webgl');

    // 按性能级别调整 CSS 变量（模糊/阴影）
    if (this.optimizationLevel === 'high') {
      root.style.setProperty('--glass-blur', 'blur(12px)');
    } else if (this.optimizationLevel === 'medium') {
      root.style.setProperty('--glass-blur', 'blur(8px)');
    } else {
      root.style.setProperty('--glass-blur', 'blur(4px)');
    }
  }

  getPerformanceReport() {
    return {
      deviceInfo: this.deviceInfo,
      optimizationLevel: this.optimizationLevel,
      timestamp: new Date().toISOString()
    };
  }
}

// 全局导出
window.PerformanceDetector = PerformanceDetector;

document.addEventListener('DOMContentLoaded', () => {
  window.performanceDetector = new PerformanceDetector();
  window.getPerformanceReport = () => window.performanceDetector.getPerformanceReport();
});
