// ==========================================
// 工具函数模块
// Utility Functions Module
// ==========================================

class Utils {
  // 防抖函数
  static debounce(func, wait, immediate = false) {
    let timeout;
    return function executedFunction(...args) {
      const later = () => {
        timeout = null;
        if (!immediate) func.apply(this, args);
      };
      const callNow = immediate && !timeout;
      clearTimeout(timeout);
      timeout = setTimeout(later, wait);
      if (callNow) func.apply(this, args);
    };
  }

  // 节流函数
  static throttle(func, limit) {
    let inThrottle;
    return function() {
      const args = arguments;
      const context = this;
      if (!inThrottle) {
        func.apply(context, args);
        inThrottle = true;
        setTimeout(() => inThrottle = false, limit);
      }
    };
  }

  // 获取随机数（带种子）
  static seededRandom(seed) {
    const x = Math.sin(seed) * 10000;
    return x - Math.floor(x);
  }

  // 生成范围随机数
  static randomInRange(min, max, seed = null) {
    if (seed !== null) {
      const random = this.seededRandom(seed);
      return min + random * (max - min);
    }
    return Math.random() * (max - min) + min;
  }

  // 随机整数
  static randomInt(min, max, seed = null) {
    if (seed !== null) {
      return Math.floor(this.randomInRange(min, max + 1, seed));
    }
    return Math.floor(Math.random() * (max - min + 1)) + min;
  }

  // 限制数值范围
  static clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
  }

  // 线性插值
  static lerp(start, end, factor) {
    return start + (end - start) * factor;
  }

  // 平滑插值（平滑step）
 static smoothstep(edge0, edge1, x) {
    const t = this.clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
  }

  // 更平滑的插值（smootherstep）
  static smootherstep(edge0, edge1, x) {
    const t = this.clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * t * (t * (t * 6 - 15) + 10);
  }

  // 距离计算
  static distance(x1, y1, x2, y2) {
    const dx = x2 - x1;
    const dy = y2 - y1;
    return Math.sqrt(dx * dx + dy * dy);
  }

  // 角度计算
  static angle(x1, y1, x2, y2) {
    return Math.atan2(y2 - y1, x2 - x1);
  }

  // 角度转弧度
  static degToRad(degrees) {
    return degrees * Math.PI / 180;
  }

  // 弧度转角度
  static radToDeg(radians) {
    return radians * 180 / Math.PI;
  }

  // 格式化数字
  static formatNumber(num, decimals = 2) {
    return parseFloat(num).toFixed(decimals);
  }

  // 生成UUID
  static generateUUID() {
    return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c) {
      const r = Math.random() * 16 | 0;
      const v = c == 'x' ? r : (r & 0x3 | 0x8);
      return v.toString(16);
    });
  }

  // 深拷贝对象
  static deepClone(obj) {
    if (obj === null || typeof obj !== 'object') return obj;
    if (obj instanceof Date) return new Date(obj.getTime());
    if (obj instanceof Array) return obj.map(item => this.deepClone(item));
    if (typeof obj === 'object') {
      const clonedObj = {};
      for (const key in obj) {
        if (obj.hasOwnProperty(key)) {
          clonedObj[key] = this.deepClone(obj[key]);
        }
      }
      return clonedObj;
    }
  }

  // 检查是否为数字
  static isNumber(value) {
    return typeof value === 'number' && !isNaN(value);
  }

  // 检查是否在范围内
  static inRange(value, min, max) {
    return value >= min && value <= max;
  }

  // 映射数值范围
  static map(value, inMin, inMax, outMin, outMax) {
    return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
  }

  // 颜色插值（RGB）
  static lerpColor(color1, color2, factor) {
    const r1 = parseInt(color1.slice(1, 3), 16);
    const g1 = parseInt(color1.slice(3, 5), 16);
    const b1 = parseInt(color1.slice(5, 7), 16);
    
    const r2 = parseInt(color2.slice(1, 3), 16);
    const g2 = parseInt(color2.slice(3, 5), 16);
    const b2 = parseInt(color2.slice(5, 7), 16);
    
    const r = Math.round(this.lerp(r1, r2, factor));
    const g = Math.round(this.lerp(g1, g2, factor));
    const b = Math.round(this.lerp(b1, b2, factor));
    
    return `#${r.toString(16).padStart(2, '0')}${g.toString(16).padStart(2, '0')}${b.toString(16).padStart(2, '0')}`;
  }

  // HSL到RGB转换
  static hslToRgb(h, s, l) {
    h = h / 360;
    s = s / 100;
    l = l / 100;
    
    let r, g, b;
    
    if (s === 0) {
      r = g = b = l;
    } else {
      const hue2rgb = (p, q, t) => {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1/6) return p + (q - p) * 6 * t;
        if (t < 1/2) return q;
        if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
        return p;
      };
      
      const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
      const p = 2 * l - q;
      r = hue2rgb(p, q, h + 1/3);
      g = hue2rgb(p, q, h);
      b = hue2rgb(p, q, h - 1/3);
    }
    
    return {
      r: Math.round(r * 255),
      g: Math.round(g * 255),
      b: Math.round(b * 255)
    };
  }

  // RGB到HSL转换
  static rgbToHsl(r, g, b) {
    r /= 255;
    g /= 255;
    b /= 255;
    
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    let h, s, l = (max + min) / 2;
    
    if (max === min) {
      h = s = 0;
    } else {
      const d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
      switch (max) {
        case r: h = (g - b) / d + (g < b ? 6 : 0); break;
        case g: h = (b - r) / d + 2; break;
        case b: h = (r - g) / d + 4; break;
      }
      h /= 6;
    }
    
    return {
      h: Math.round(h * 360),
      s: Math.round(s * 100),
      l: Math.round(l * 100)
    };
  }

  // 本地存储操作
  static saveToLocalStorage(key, value) {
    try {
      localStorage.setItem(key, JSON.stringify(value));
      return true;
    } catch (e) {
      console.warn('LocalStorage保存失败:', e);
      return false;
    }
  }

  static loadFromLocalStorage(key, defaultValue = null) {
    try {
      const item = localStorage.getItem(key);
      return item ? JSON.parse(item) : defaultValue;
    } catch (e) {
      console.warn('LocalStorage读取失败:', e);
      return defaultValue;
    }
  }

  // 下载文件
  static downloadFile(filename, content, type = 'text/plain') {
    const blob = new Blob([content], { type });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  }

  // 性能监控
  static measurePerformance(name, fn) {
    const start = performance.now();
    const result = fn();
    const end = performance.now();
    console.log(`${name} took ${(end - start).toFixed(2)} milliseconds`);
    return result;
  }

  // 记录日志到Log.log文件（追加模式）
  static logModification(modificationData) {
    const currentTime = new Date();
    const timeString = currentTime.toLocaleString('zh-CN', {
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: false
    }).replace(/\//g, '-'); // 将日期格式化为 YYYY-MM-DD HH:MM:SS

    // 构建日志内容
    const logEntry = `================================================================================\n` +
      `修改时间: ${timeString}\n` +
      `修改类型: ${modificationData.type || '修改'}\n` +
      `修改文件: ${modificationData.file || ''}\n` +
      `修改位置: ${modificationData.location || ''}\n` +
      `修改内容: \n` +
      `  - ${(modificationData.content || '').replace(/\n/g, '\n  - ')}\n` +
      `修改原因: ${modificationData.reason || ''}\n` +
      `相关任务: ${modificationData.task || ''}\n` +
      `验证状态: ${modificationData.validationStatus || '已验证'}\n` +
      `验证结果: ${modificationData.validationResult || ''}\n` +
      `修改人: WorkBuddy AI Assistant\n` +
      `================================================================================\n\n`;

    // 创建隐藏的a标签用于下载文件（追加模式）
    const blob = new Blob([logEntry], { type: 'text/plain' });
    
    // 如果已经存在Log.log，我们需要读取并追加内容
    // 这里使用fetch API来读取现有日志文件
    fetch('Log.log')
      .then(response => response.text())
      .then(existingContent => {
        const newContent = existingContent + logEntry;
        const newBlob = new Blob([newContent], { type: 'text/plain' });
        
        // 创建下载链接
        const url = URL.createObjectURL(newBlob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'Log.log';
        a.style.display = 'none';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      })
      .catch(error => {
        // 如果文件不存在，直接创建新文件
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'Log.log';
        a.style.display = 'none';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
      });
  }
}

// ==========================================
// 导出到全局
// ==========================================

window.Utils = Utils;

// 快捷访问函数
window.debounce = Utils.debounce;
window.throttle = Utils.throttle;
window.clamp = Utils.clamp;
window.lerp = Utils.lerp;
window.map = Utils.map;
window.formatNumber = Utils.formatNumber;
