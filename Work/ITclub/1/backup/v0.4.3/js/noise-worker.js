// noise-worker.js - 用于在后台线程中处理噪声生成

// Perlin Noise 类定义
class PerlinNoise {
  constructor(seed = 42) {
    this.seed = seed;
    this.gradientVectors = {};
    this.permutation = this._generatePermutation(seed);
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

  // 获取梯度向量（缓存结果）
  _getGradient(x, y) {
    const key = `${x},${y}`;
    if (!this.gradientVectors[key]) {
      this.gradientVectors[key] = this._generateGradient(x, y);
    }
    return this.gradientVectors[key];
  }

  // 线性插值
  _lerp(a, b, t) {
    return a + t * (b - a);
  }

  // 平滑步函数
  _fade(t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
  }

  // 点积
  _dot2D(grad, x, y) {
    return grad.x * x + grad.y * y;
  }

  // 2D Perlin噪声
  noise2D(x, y) {
    // 获取整数部分
    const X = Math.floor(x);
    const Y = Math.floor(y);
    
    // 获取小数部分
    const x0 = x - X;
    const y0 = y - Y;
    
    // 计算四个角的哈希值
    const X0 = X & 255;
    const Y0 = Y & 255;
    const X1 = (X0 + 1) & 255;
    const Y1 = (Y0 + 1) & 255;
    
    // 获取四个角的梯度向量
    const grad00 = this._getGradient(X0, Y0);
    const grad01 = this._getGradient(X0, Y1);
    const grad10 = this._getGradient(X1, Y0);
    const grad11 = this._getGradient(X1, Y1);
    
    // 计算距离向量
    const dx0 = x0;
    const dy0 = y0;
    const dx1 = x0 - 1;
    const dy1 = y0 - 1;
    
    // 计算点积
    const dot00 = this._dot2D(grad00, dx0, dy0);
    const dot01 = this._dot2D(grad01, dx0, dy1);
    const dot10 = this._dot2D(grad10, dx1, dy0);
    const dot11 = this._dot2D(grad11, dx1, dy1);
    
    // 应用平滑步函数
    const u = this._fade(x0);
    const v = this._fade(y0);
    
    // 双线性插值
    const top = this._lerp(dot00, dot10, u);
    const bottom = this._lerp(dot01, dot11, u);
    const result = this._lerp(top, bottom, v);
    
    return result;
  }

  // 分形噪声（多重八度）
  fractalNoise2D(x, y, octaves = 4, persistence = 0.5, lacunarity = 2.0) {
    let result = 0;
    let amplitude = 1;
    let frequency = 1;
    let maxAmplitude = 0;
    
    for (let i = 0; i < octaves; i++) {
      result += this.noise2D(x * frequency, y * frequency) * amplitude;
      maxAmplitude += amplitude;
      amplitude *= persistence;
      frequency *= lacunarity;
    }
    
    // 归一化
    return result / maxAmplitude;
  }

  // 设置种子
  setSeed(seed) {
    this.seed = seed;
    this.gradientVectors = {};
    this.permutation = this._generatePermutation(seed);
  }
}

// 创建PerlinNoise实例
const perlinNoise = new PerlinNoise(42);

// 处理来自主线程的消息
self.onmessage = function(e) {
  const { width, height, frequency, amplitude, octaves, persistence, lacunarity } = e.data;
  
  // 创建ImageData对象
  const imageData = { width, height, data: new Uint8ClampedArray(width * height * 4) };
  
  const totalPixels = width * height;
  let processedPixels = 0;
  
  // 计算噪声并填充像素数据
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const noise = perlinNoise.fractalNoise2D(
        x * frequency, 
        y * frequency, 
        octaves,
        persistence,
        lacunarity
      ) * amplitude;
      const value = Math.floor((noise + 1) * 127.5);
      const index = (y * width + x) * 4;
      
      imageData.data[index] = value;
      imageData.data[index + 1] = Math.min(255, value + 30);
      imageData.data[index + 2] = Math.min(255, value + 60);
      imageData.data[index + 3] = 255;
      
      processedPixels++;
      
      // 每处理1000个像素发送一次进度更新
      if (processedPixels % 1000 === 0) {
        const progress = Math.min(100, Math.round((processedPixels / totalPixels) * 100));
        self.postMessage({ type: 'progress', progress });
      }
    }
  }
  
  // 发送完成消息
  self.postMessage({ type: 'complete', imageData });
};