// ==========================================
// 优化版噪声Web Worker
// Optimized Noise Web Worker
// ==========================================

// 使用优化版的柏林噪声算法
class OptimizedPerlinNoise {
  constructor(seed = 1) {
    this.perm = new Uint8Array(512);
    this.gradients = new Array(512);
    this.reseed(seed);
  }
  
  reseed(seed) {
    // 使用更快的种子算法
    const p = new Uint8Array(256);
    for (let i = 0; i < 256; i++) p[i] = i;
    
    // Fisher-Yates洗牌算法（优化版）
    let s = seed || 1;
    for (let i = 255; i > 0; i--) {
      s = (s * 16807) & 0x7fffffff; // 更快的伪随机数生成
      const j = s % (i + 1);
      [p[i], p[j]] = [p[j], p[i]];
    }
    
    // 填充permutation表
    for (let i = 0; i < 512; i++) {
      this.perm[i] = p[i & 255];
      // 预计算梯度向量
      const angle = (this.perm[i] / 255) * Math.PI * 2;
      this.gradients[i] = {
        x: Math.cos(angle),
        y: Math.sin(angle)
      };
    }
  }
  
  // 优化版的fade函数（使用近似计算）
  fade(t) {
    // 使用更快的近似计算：6t^5 - 15t^4 + 10t^3
    const t3 = t * t * t;
    const t4 = t3 * t;
    const t5 = t4 * t;
    return 6 * t5 - 15 * t4 + 10 * t3;
  }
  
  // 快速线性插值
  lerp(a, b, t) {
    return a + t * (b - a);
  }
  
  // 优化版噪声计算（2D）
  noise2D(x, y) {
    const X = Math.floor(x) & 255;
    const Y = Math.floor(y) & 255;
    
    x -= Math.floor(x);
    y -= Math.floor(y);
    
    const u = this.fade(x);
    const v = this.fade(y);
    
    const p = this.perm;
    const g = this.gradients;
    
    // 预计算索引
    const A = p[X] + Y;
    const AA = p[A];
    const AB = p[A + 1];
    const B = p[X + 1] + Y;
    const BA = p[B];
    const BB = p[B + 1];
    
    // 计算四个角点的贡献
    const dotAA = g[AA].x * x + g[AA].y * y;
    const dotBA = g[BA].x * (x - 1) + g[BA].y * y;
    const dotAB = g[AB].x * x + g[AB].y * (y - 1);
    const dotBB = g[BB].x * (x - 1) + g[BB].y * (y - 1);
    
    // 双线性插值
    const top = this.lerp(dotAA, dotBA, u);
    const bottom = this.lerp(dotAB, dotBB, u);
    return this.lerp(top, bottom, v);
  }
  
  // 分形布朗运动（优化版）
  fbm2D(x, y, octaves = 4, persistence = 0.5, lacunarity = 2.0) {
    let value = 0;
    let amplitude = 1;
    let frequency = 1;
    let maxValue = 0;
    
    // 使用展开循环以提高性能
    for (let i = 0; i < octaves; i++) {
      value += this.noise2D(x * frequency, y * frequency) * amplitude;
      maxValue += amplitude;
      amplitude *= persistence;
      frequency *= lacunarity;
      
      // 早期退出：如果振幅太小，可以提前结束
      if (amplitude < 0.01) break;
    }
    
    return value / maxValue;
  }
  
  // 批量噪声生成（优化性能）
  generateBatch(width, height, scale = 0.01, octaves = 3) {
    const total = width * height;
    const result = new Float32Array(total);
    
    // 使用预计算的值
    const invWidth = 1 / width;
    const invHeight = 1 / height;
    
    for (let i = 0; i < total; i++) {
      const x = (i % width) * scale;
      const y = Math.floor(i / width) * scale;
      result[i] = this.fbm2D(x, y, octaves);
    }
    
    return result;
  }
}

// 颜色映射函数（预计算查找表）
class ColorMapper {
  constructor() {
    // 预计算颜色查找表
    this.lutSize = 256;
    this.gradientLUT = this.createGradientLUT();
    this.terrainLUT = this.createTerrainLUT();
  }
  
  createGradientLUT() {
    const lut = new Uint8Array(this.lutSize * 3); // RGB
    
    for (let i = 0; i < this.lutSize; i++) {
      const value = i / (this.lutSize - 1);
      
      // 蓝紫渐变
      const index = i * 3;
      lut[index] = Math.floor(value * 200);     // R
      lut[index + 1] = Math.floor(value * 100); // G
      lut[index + 2] = Math.floor(value * 255); // B
    }
    
    return lut;
  }
  
  createTerrainLUT() {
    const lut = new Uint8Array(this.lutSize * 3);
    
    for (let i = 0; i < this.lutSize; i++) {
      const value = i / (this.lutSize - 1);
      const index = i * 3;
      
      let r, g, b;
      
      // 地形颜色映射
      if (value < 0.3) {
        // 深蓝（水域）
        r = 30; g = 60; b = 150;
      } else if (value < 0.4) {
        // 浅蓝（浅水）
        r = 50; g = 120; b = 180;
      } else if (value < 0.5) {
        // 沙滩色
        r = 210; g = 190; b = 140;
      } else if (value < 0.7) {
        // 绿色（草地）
        r = 60; g = 120; b = 50;
      } else if (value < 0.85) {
        // 棕色（山地）
        r = 100; g = 90; b = 80;
      } else {
        // 白色（雪山）
        r = 240; g = 245; b = 255;
      }
      
      lut[index] = r;
      lut[index + 1] = g;
      lut[index + 2] = b;
    }
    
    return lut;
  }
  
  // 快速颜色映射
  mapToColor(value, colorMap = 'gradient') {
    const index = Math.floor((value + 1) * 0.5 * (this.lutSize - 1));
    const clampedIndex = Math.max(0, Math.min(this.lutSize - 1, index));
    const baseIndex = clampedIndex * 3;
    
    if (colorMap === 'terrain') {
      return [
        this.terrainLUT[baseIndex],
        this.terrainLUT[baseIndex + 1],
        this.terrainLUT[baseIndex + 2]
      ];
    } else {
      return [
        this.gradientLUT[baseIndex],
        this.gradientLUT[baseIndex + 1],
        this.gradientLUT[baseIndex + 2]
      ];
    }
  }
  
  // 批量颜色映射
  mapBatchToColor(noiseData, width, height, colorMap = 'gradient') {
    const total = width * height;
    const imageData = new Uint8ClampedArray(total * 4);
    const lut = colorMap === 'terrain' ? this.terrainLUT : this.gradientLUT;
    
    for (let i = 0; i < total; i++) {
      const noiseValue = noiseData[i];
      const index = Math.floor((noiseValue + 1) * 0.5 * (this.lutSize - 1));
      const clampedIndex = Math.max(0, Math.min(this.lutSize - 1, index));
      const lutIndex = clampedIndex * 3;
      const pixelIndex = i * 4;
      
      imageData[pixelIndex] = lut[lutIndex];         // R
      imageData[pixelIndex + 1] = lut[lutIndex + 1]; // G
      imageData[pixelIndex + 2] = lut[lutIndex + 2]; // B
      imageData[pixelIndex + 3] = 255;               // A
    }
    
    return imageData;
  }
}

// Worker主逻辑
let noiseGenerator = null;
let colorMapper = null;

self.onmessage = function(e) {
  const msg = e.data;
  const type = msg.type;
  
  switch (type) {
    case 'init':
      initializeWorker(msg.params);
      break;
      
    // 兼容旧接口：type='custom' 和 type='preset'
    case 'custom':
      handleCustom(msg);
      break;
    
    case 'preset':
      handlePreset(msg);
      break;
      
    case 'generate':
      generateNoise(msg.id, msg.width, msg.height, msg.params);
      break;
      
    case 'batch':
      generateBatch(msg.id, msg.width, msg.height, msg.params);
      break;
      
    case 'terminate':
      self.close();
      break;
  }
};

// 处理旧接口：custom（自定义参数渲染）
function handleCustom(msg) {
  if (!noiseGenerator) {
    noiseGenerator = new OptimizedPerlinNoise(msg.seed || 42);
    colorMapper = new ColorMapper();
  }
  
  const { width, height, frequency, amplitude, octaves, persistence, lacunarity } = msg;
  const scale = (frequency || 1.0) * 0.008;
  
  const noiseData = new Float32Array(width * height);
  const oct = octaves || 4;
  const pers = persistence || 0.5;
  const lac = lacunarity || 2.0;
  const amp = amplitude || 1.0;
  
  for (let y = 0; y < height; y++) {
    const rowIdx = y * width;
    for (let x = 0; x < width; x++) {
      noiseData[rowIdx + x] = noiseGenerator.fbm2D(x * scale, y * scale, oct, pers, lac) * amp;
    }
  }
  
  // 使用蓝紫渐变色
  const total = width * height;
  const data = new Uint8ClampedArray(total * 4);
  for (let i = 0; i < total; i++) {
    const v = Math.floor((noiseData[i] + 1) * 127.5);
    const clamped = Math.max(0, Math.min(255, v));
    const idx = i * 4;
    data[idx]     = clamped;
    data[idx + 1] = Math.min(255, clamped + 60);
    data[idx + 2] = Math.min(255, clamped + 120);
    data[idx + 3] = 255;
  }
  
  self.postMessage({ type: 'custom', data: data.buffer, width, height }, [data.buffer]);
}

// 处理旧接口：preset（预设场景渲染）
function handlePreset(msg) {
  if (!noiseGenerator) {
    noiseGenerator = new OptimizedPerlinNoise(msg.seed || 42);
    colorMapper = new ColorMapper();
  }
  
  const { width, height, octaves, persistence, lacunarity, colorMap, seed } = msg;
  
  if (seed !== undefined) noiseGenerator.reseed(seed);
  
  const scale = 0.012;
  const oct = octaves || 4;
  const pers = persistence || 0.5;
  const lac = lacunarity || 2.0;
  
  const noiseData = new Float32Array(width * height);
  for (let y = 0; y < height; y++) {
    const rowIdx = y * width;
    for (let x = 0; x < width; x++) {
      noiseData[rowIdx + x] = noiseGenerator.fbm2D(x * scale, y * scale, oct, pers, lac);
    }
  }
  
  const total = width * height;
  const data = new Uint8ClampedArray(total * 4);
  
  for (let i = 0; i < total; i++) {
    const normalized = (noiseData[i] + 1) * 0.5;
    const idx = i * 4;
    let r, g, b;
    
    switch (colorMap) {
      case 'terrain':
        if (normalized < 0.3)       { r=30;  g=60;  b=150; }
        else if (normalized < 0.4)  { r=50;  g=120; b=180; }
        else if (normalized < 0.5)  { r=210; g=190; b=140; }
        else if (normalized < 0.7)  { r=60;  g=120; b=50;  }
        else if (normalized < 0.85) { r=100; g=90;  b=80;  }
        else                        { r=240; g=245; b=255; }
        break;
      case 'clouds':
        { const a = Math.max(0, normalized - 0.3) / 0.7;
          r = (200 + a * 55) | 0; g = (220 + a * 35) | 0; b = 255; }
        break;
      case 'marble':
        { const mv = Math.sin(((i % width) * 0.08 + noiseData[i] * 10)) * Math.cos((Math.floor(i / width) * 0.08 + noiseData[i] * 10));
          const mc = ((mv + 1) * 127.5) | 0;
          r = (200 + mc * 0.2) | 0; g = (190 + mc * 0.25) | 0; b = (180 + mc * 0.3) | 0; }
        break;
      case 'wood':
        { const wx = (i % width);
          const wg = Math.sin((wx + noiseData[i] * 20) * 0.1);
          const wv = (wg + 1) * 0.5;
          r = (139 + wv * 40) | 0; g = (90 + wv * 30) | 0; b = (43 + wv * 20) | 0; }
        break;
      case 'fire':
        { const fy = Math.floor(i / width) / height;
          const fireY = 1 - fy;
          if (noiseData[i] * fireY > 0.3) { r = 255; g = (150 * (1 - noiseData[i])) | 0; b = 0; }
          else { r = (50 * noiseData[i] * fireY) | 0; g = (20 * noiseData[i] * fireY) | 0; b = 0; } }
        break;
      case 'water':
        { const wx2 = i % width; const wy2 = Math.floor(i / width);
          const wave = Math.sin(wx2 * 0.15 + noiseData[i] * 5) * Math.cos(wy2 * 0.12 + noiseData[i] * 3);
          r = (20 + wave * 20) | 0; g = (80 + wave * 30) | 0; b = (150 + wave * 40) | 0; }
        break;
      default:
        { const dv = (normalized * 255) | 0;
          r = dv; g = Math.min(255, dv + 60); b = Math.min(255, dv + 120); }
    }
    
    data[idx]     = Math.max(0, Math.min(255, r));
    data[idx + 1] = Math.max(0, Math.min(255, g));
    data[idx + 2] = Math.max(0, Math.min(255, b));
    data[idx + 3] = 255;
  }
  
  self.postMessage({ type: 'preset', data: data.buffer, width, height }, [data.buffer]);
}

function initializeWorker(params) {
  const seed = params?.seed || Date.now();
  noiseGenerator = new OptimizedPerlinNoise(seed);
  colorMapper = new ColorMapper();
  
  self.postMessage({
    type: 'init',
    status: 'ready',
    seed: seed
  });
}

function generateNoise(id, width, height, params) {
  if (!noiseGenerator) {
    noiseGenerator = new OptimizedPerlinNoise(params?.seed);
  }
  
  const startTime = performance.now();
  
  // 生成噪声数据
  const scale = (params?.frequency || 1.0) * 0.01;
  const octaves = params?.octaves || 4;
  const persistence = params?.persistence || 0.5;
  const lacunarity = params?.lacunarity || 2.0;
  
  const noiseData = new Float32Array(width * height);
  
  // 优化循环
  const scaleX = scale;
  const scaleY = scale;
  
  for (let y = 0; y < height; y++) {
    const yCoord = y * scaleY;
    const rowIndex = y * width;
    
    for (let x = 0; x < width; x++) {
      const xCoord = x * scaleX;
      const index = rowIndex + x;
      
      noiseData[index] = noiseGenerator.fbm2D(
        xCoord, 
        yCoord, 
        octaves, 
        persistence, 
        lacunarity
      );
    }
  }
  
  // 映射到颜色
  const colorMapType = params?.colorMap || 'gradient';
  const imageData = colorMapper.mapBatchToColor(noiseData, width, height, colorMapType);
  
  const endTime = performance.now();
  const duration = endTime - startTime;
  
  self.postMessage({
    type: 'generate',
    id: id,
    data: imageData.buffer,
    width: width,
    height: height,
    duration: duration,
    performance: {
      pixelsPerMs: (width * height) / duration,
      totalPixels: width * height
    }
  }, [imageData.buffer]);
}

function generateBatch(id, width, height, params) {
  if (!noiseGenerator) {
    noiseGenerator = new OptimizedPerlinNoise(params?.seed);
  }
  
  const startTime = performance.now();
  
  // 使用批量生成方法
  const scale = (params?.frequency || 1.0) * 0.01;
  const octaves = params?.octaves || 4;
  
  const noiseData = noiseGenerator.generateBatch(width, height, scale, octaves);
  
  // 应用振幅（如果指定）
  const amplitude = params?.amplitude || 1.0;
  if (amplitude !== 1.0) {
    for (let i = 0; i < noiseData.length; i++) {
      noiseData[i] *= amplitude;
    }
  }
  
  // 映射到颜色
  const colorMapType = params?.colorMap || 'gradient';
  const imageData = colorMapper.mapBatchToColor(noiseData, width, height, colorMapType);
  
  const endTime = performance.now();
  const duration = endTime - startTime;
  
  self.postMessage({
    type: 'batch',
    id: id,
    data: imageData.buffer,
    width: width,
    height: height,
    duration: duration,
    performance: {
      pixelsPerMs: (width * height) / duration,
      totalPixels: width * height
    }
  }, [imageData.buffer]);
}

// 错误处理
self.onerror = function(error) {
  console.error('Worker错误:', error);
  self.postMessage({
    type: 'error',
    message: error.message,
    stack: error.stack
  });
};

console.log('优化版噪声Worker已加载');