// ==========================================
// 柏林噪声算法实现
// Perlin Noise Algorithm Implementation
// ==========================================

class PerlinNoise {
  constructor(seed = 42) {
    this.seed = seed;
    this.gradientVectors = {};
    this.permutation = this._generatePermutation(seed);
    // 性能优化：缓存梯度向量查找
    this._gradientCache = new Map();
    this._cacheHits = 0;
    this._cacheMisses = 0;
  }

  // 生成排列数组（用于Simplex噪声）
  _generatePermutation(seed) {
    const p = [];
    for (let i = 0; i < 256; i++) {
      p[i] = i;
    }
    
    // 使用种子进行伪随机打乱
    let n = seed;
    for (let i = 255; i > 0; i--) {
      n = (n * 16807) % 2147483647;
      const j = n % (i + 1);
      [p[i], p[j]] = [p[j], p[i]];
    }
    
    // 复制一份形成512长度的数组
    const perm = [];
    for (let i = 0; i < 512; i++) {
      perm[i] = p[i % 256];
    }
    return perm;
  }

  // 生成伪随机值（基于种子）
  _seededRandom(x, y) {
    const n = x * 37401 + y * 82633 + this.seed * 92821;
    return Math.abs(Math.sin(n) * 10000) % 1;
  }

  // 生成随机梯度向量
  _generateGradient(x, y) {
    const angle = this._seededRandom(x, y) * Math.PI * 2;
    return {
      x: Math.cos(angle),
      y: Math.sin(angle)
    };
  }

  // 获取梯度向量（缓存优化版）
  _getGradient(x, y) {
    const key = `${x},${y}`;
    // 性能优化：使用 Map 缓存
    if (this._gradientCache.has(key)) {
      this._cacheHits++;
      return this._gradientCache.get(key);
    }
    
    this._cacheMisses++;
    const gradient = this._generateGradient(x, y);
    this._gradientCache.set(key, gradient);
    return gradient;
  }

  // 平滑插值函数（5阶多项式）
  _fade(t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
  }

  // 线性插值
  _lerp(t, a, b) {
    return a + t * (b - a);
  }

  // 二维Perlin噪声（核心算法）
  noise2D(x, y) {
    const x0 = Math.floor(x);
    const x1 = x0 + 1;
    const y0 = Math.floor(y);
    const y1 = y0 + 1;
    
    // 获取四个角点的梯度向量
    const g00 = this._getGradient(x0, y0);
    const g10 = this._getGradient(x1, y0);
    const g01 = this._getGradient(x0, y1);
    const g11 = this._getGradient(x1, y1);
    
    // 计算到四个角点的距离向量
    const dx0 = x - x0;
    const dx1 = x - x1;
    const dy0 = y - y0;
    const dy1 = y - y1;
    
    // 计算点乘
    const dot00 = g00.x * dx0 + g00.y * dy0;
    const dot10 = g10.x * dx1 + g10.y * dy0;
    const dot01 = g01.x * dx0 + g01.y * dy1;
    const dot11 = g11.x * dx1 + g11.y * dy1;
    
    // 平滑插值
    const sx = this._fade(dx0);
    const sy = this._fade(dy0);
    
    const n0 = this._lerp(sx, dot00, dot10);
    const n1 = this._lerp(sx, dot01, dot11);
    
    return this._lerp(sy, n0, n1);
  }

  // 分形Perlin噪声（多层叠加）
  fractalNoise2D(x, y, octaves = 4, persistence = 0.5, lacunarity = 2.0) {
    let value = 0;
    let amplitude = 1;
    let frequency = 1;
    let maxValue = 0;
    
    for (let i = 0; i < octaves; i++) {
      value += this.noise2D(x * frequency, y * frequency) * amplitude;
      maxValue += amplitude;
      amplitude *= persistence;
      frequency *= lacunarity;
    }
    
    return value / maxValue;
  }

  // 一维Perlin噪声（用于曲线生成）
  noise1D(x) {
    const x0 = Math.floor(x);
    const x1 = x0 + 1;
    
    const gx0 = this._getGradient(x0, 0).x;
    const gx1 = this._getGradient(x1, 0).x;
    
    const dx0 = x - x0;
    const dx1 = x - x1;
    
    const dot0 = gx0 * dx0;
    const dot1 = gx1 * dx1;
    
    const t = this._fade(dx0);
    return this._lerp(t, dot0, dot1);
  }

  // 清空缓存（用于重新生成）
  clearCache() {
    this.gradientVectors = {};
    this._gradientCache.clear();
    this._cacheHits = 0;
    this._cacheMisses = 0;
  }

  // 设置新种子
  setSeed(seed) {
    this.seed = seed;
    this.clearCache();
    this.permutation = this._generatePermutation(seed);
  }
}

// ==========================================
// 导出全局实例
// ==========================================

const perlinNoise = new PerlinNoise(42);

// 全局访问函数
window.generatePerlinNoise = function(seed = 42) {
  return new PerlinNoise(seed);
};

window.perlinNoise2D = function(x, y, octaves = 1, persistence = 0.5) {
  if (octaves === 1) {
    return perlinNoise.noise2D(x, y);
  }
  return perlinNoise.fractalNoise2D(x, y, octaves, persistence);
};

window.perlinNoise1D = function(x) {
  return perlinNoise.noise1D(x);
};

window.setNoiseSeed = function(seed) {
  perlinNoise.setSeed(seed);
};
