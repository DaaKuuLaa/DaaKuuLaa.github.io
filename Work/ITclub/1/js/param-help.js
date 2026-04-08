/**
 * param-help.js
 * 第四模块参数说明弹窗 - 毛玻璃卡片 + 可视化
 * Author: WorkBuddy AI Assistant
 */

// ===================== 参数数据定义 =====================
const PARAM_HELP_DATA = {
  frequency: {
    title: '频率 (Frequency)',
    subtitle: 'Controls the spatial scale of noise features',
    icon: 'fas fa-wave-square',
    desc: '<strong>频率</strong>决定了噪声纹理的<strong>空间缩放比例</strong>，即噪声图案在空间中重复出现的疏密程度。<br><br>• <strong>低频率（0.1–1）</strong>：图案宽大、平滑，适合模拟宏观地形或大面积云层<br>• <strong>高频率（2–5）</strong>：图案细密、紧凑，适合模拟细节纹理或颗粒感材质<br><br>直观理解：频率越高，"波浪"越密集；频率越低，"波浪"越舒缓。',
    tags: [
      { text: '空间缩放', icon: 'fas fa-compress-arrows-alt', cls: '' },
      { text: '影响密度', icon: 'fas fa-th', cls: 'tag-yellow' },
      { text: '0.1 ~ 5.0', icon: 'fas fa-ruler', cls: 'tag-green' },
    ],
    vizTitle: '不同频率的噪声对比（左低右高）',
    vizType: 'frequency',
  },
  amplitude: {
    title: '振幅 (Amplitude)',
    subtitle: 'Controls the height / intensity of noise values',
    icon: 'fas fa-arrows-alt-v',
    desc: '<strong>振幅</strong>决定了噪声数值的<strong>输出范围</strong>，可以理解为噪声"高度"或"强度"的缩放系数。<br><br>• <strong>低振幅（0.1–0.5）</strong>：输出值范围窄，图像对比度低，平坦柔和<br>• <strong>高振幅（1.0–2.0）</strong>：输出值范围宽，图像明暗对比强烈<br><br>在地形生成中，振幅直接决定山峰的最大高度；在纹理生成中，振幅决定颜色的深浅变化幅度。',
    tags: [
      { text: '强度缩放', icon: 'fas fa-sliders-h', cls: '' },
      { text: '影响对比度', icon: 'fas fa-adjust', cls: 'tag-yellow' },
      { text: '0.1 ~ 2.0', icon: 'fas fa-ruler', cls: 'tag-green' },
    ],
    vizTitle: '不同振幅的噪声波形对比',
    vizType: 'amplitude',
  },
  octaves: {
    title: '倍频 (Octaves)',
    subtitle: 'Number of noise layers stacked together',
    icon: 'fas fa-layer-group',
    desc: '<strong>倍频</strong>（也叫"八度"）是<strong>分形布朗运动（fBm）</strong>的关键参数，表示将多少层不同频率的噪声叠加在一起。<br><br>• <strong>1 倍频</strong>：最基础的单层平滑噪声，无细节<br>• <strong>4 倍频</strong>：细节适中，适合大多数自然场景<br>• <strong>8 倍频</strong>：极丰富的细节层次，像真实地形一样精细<br><br>每增加一倍频，计算量线性增加，但图案的细节层次也随之丰富。',
    tags: [
      { text: '细节层次', icon: 'fas fa-layer-group', cls: '' },
      { text: '分形叠加', icon: 'fas fa-sitemap', cls: 'tag-yellow' },
      { text: '1 ~ 8 层', icon: 'fas fa-ruler', cls: 'tag-green' },
    ],
    vizTitle: '倍频叠加示意（每行为一层，最终叠加结果）',
    vizType: 'octaves',
  },
  persistence: {
    title: '持久度 (Persistence)',
    subtitle: 'Amplitude multiplier per octave',
    icon: 'fas fa-chart-line',
    desc: '<strong>持久度</strong>控制每一层倍频的<strong>振幅衰减比例</strong>。在分形噪声中，每增加一个倍频，其振幅会乘以持久度。<br><br>• <strong>持久度 = 0.3</strong>：高频细节很快衰减，图像整体较平滑<br>• <strong>持久度 = 0.5</strong>：经典设置，高频细节适中<br>• <strong>持久度 = 0.8</strong>：高频细节占比高，图像粗糙感更强<br><br>公式：第 n 层的振幅 = <code>amplitude × persistence<sup>n</sup></code>',
    tags: [
      { text: '振幅衰减', icon: 'fas fa-sort-amount-down', cls: '' },
      { text: '决定粗糙度', icon: 'fas fa-mountain', cls: 'tag-yellow' },
      { text: '0.1 ~ 1.0', icon: 'fas fa-ruler', cls: 'tag-green' },
    ],
    vizTitle: '不同持久度下各倍频振幅的衰减曲线',
    vizType: 'persistence',
  },
  lacunarity: {
    title: '隙度 (Lacunarity)',
    subtitle: 'Frequency multiplier per octave',
    icon: 'fas fa-expand-arrows-alt',
    desc: '<strong>隙度</strong>控制每一层倍频的<strong>频率增长倍数</strong>。在分形噪声中，每增加一个倍频，其频率会乘以隙度。<br><br>• <strong>隙度 = 1.5</strong>：频率增长慢，细节分布密集但层次感弱<br>• <strong>隙度 = 2.0</strong>：经典值，每层频率翻倍，自然感最强<br>• <strong>隙度 = 4.0</strong>：频率增长极快，高频细节非常突出<br><br>公式：第 n 层的频率 = <code>frequency × lacunarity<sup>n</sup></code>',
    tags: [
      { text: '频率增长', icon: 'fas fa-tachometer-alt', cls: '' },
      { text: '控制细节密度', icon: 'fas fa-th-large', cls: 'tag-yellow' },
      { text: '1.0 ~ 4.0', icon: 'fas fa-ruler', cls: 'tag-green' },
    ],
    vizTitle: '不同隙度下各倍频频率的增长对比',
    vizType: 'lacunarity',
  },
};

// ===================== 内置简易 Perlin 噪声 =====================
(function buildMiniPerlin() {
  const perm = [];
  for (let i = 0; i < 256; i++) perm[i] = i;
  for (let i = 255; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [perm[i], perm[j]] = [perm[j], perm[i]];
  }
  const p = new Uint8Array(512);
  for (let i = 0; i < 512; i++) p[i] = perm[i & 255];

  function fade(t) { return t * t * t * (t * (t * 6 - 15) + 10); }
  function lerp(a, b, t) { return a + t * (b - a); }
  function grad(hash, x, y) {
    const h = hash & 3;
    const u = h < 2 ? x : y;
    const v = h < 2 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
  }

  window.miniNoise2D = function(x, y) {
    const X = Math.floor(x) & 255, Y = Math.floor(y) & 255;
    x -= Math.floor(x); y -= Math.floor(y);
    const u = fade(x), v = fade(y);
    const aa = p[p[X] + Y], ab = p[p[X] + Y + 1],
          ba = p[p[X + 1] + Y], bb = p[p[X + 1] + Y + 1];
    return lerp(
      lerp(grad(aa, x, y),     grad(ba, x - 1, y),     u),
      lerp(grad(ab, x, y - 1), grad(bb, x - 1, y - 1), u),
      v
    );
  };

  window.miniFBM = function(x, y, octaves, persistence, lacunarity) {
    let val = 0, amp = 1, freq = 1, maxVal = 0;
    for (let i = 0; i < octaves; i++) {
      val += miniNoise2D(x * freq, y * freq) * amp;
      maxVal += amp;
      amp *= persistence;
      freq *= lacunarity;
    }
    return val / maxVal;
  };
})();

// ===================== 可视化绘制函数 =====================

function drawNoiseCanvas(canvas, freq, octaves, persistence, lacunarity) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  const img = ctx.createImageData(w, h);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      let v = miniFBM(x / w * freq, y / h * freq, octaves, persistence, lacunarity);
      v = Math.max(0, Math.min(1, (v + 0.5)));
      const brightness = Math.floor(v * 255);
      const idx = (y * w + x) * 4;
      img.data[idx]     = brightness;
      img.data[idx + 1] = brightness;
      img.data[idx + 2] = brightness;
      img.data[idx + 3] = 255;
    }
  }
  ctx.putImageData(img, 0, 0);
}

function drawWaveCanvas(canvas, freq, amplitude, label) {
  const ctx = canvas.getContext('2d');
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.fillStyle = 'rgba(10, 13, 26, 0.6)';
  ctx.fillRect(0, 0, w, h);

  // 网格
  ctx.strokeStyle = 'rgba(255,255,255,0.04)';
  ctx.lineWidth = 1;
  for (let gx = 0; gx < w; gx += 20) { ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, h); ctx.stroke(); }
  for (let gy = 0; gy < h; gy += 20) { ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(w, gy); ctx.stroke(); }

  // 基准线
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.lineWidth = 1;
  ctx.setLineDash([3, 3]);
  ctx.beginPath(); ctx.moveTo(0, h / 2); ctx.lineTo(w, h / 2); ctx.stroke();
  ctx.setLineDash([]);

  // 波形
  ctx.strokeStyle = '#5c9dff';
  ctx.lineWidth = 1.5;
  ctx.shadowColor = 'rgba(41, 98, 255, 0.6)';
  ctx.shadowBlur = 6;
  ctx.beginPath();
  for (let px = 0; px < w; px++) {
    const nx = px / w * freq * 3;
    const ny = miniNoise2D(nx, 0.5) * amplitude;
    const vy = h / 2 - ny * (h * 0.38);
    if (px === 0) ctx.moveTo(px, vy); else ctx.lineTo(px, vy);
  }
  ctx.stroke();
  ctx.shadowBlur = 0;

  if (label) {
    ctx.fillStyle = 'rgba(255,255,255,0.4)';
    ctx.font = '10px monospace';
    ctx.fillText(label, 5, h - 5);
  }
}

// ---- 频率可视化 ----
function renderFrequencyViz(container) {
  container.innerHTML = '';
  const freqs = [0.5, 1.5, 3.0];
  const labels = ['低频 (0.5)', '中频 (1.5)', '高频 (3.0)'];
  freqs.forEach((f, i) => {
    const g = document.createElement('div');
    g.className = 'viz-group';
    const c = document.createElement('canvas');
    c.width = 120; c.height = 80;
    drawNoiseCanvas(c, f, 1, 0.5, 2.0);
    const lb = document.createElement('div');
    lb.className = 'viz-label';
    lb.textContent = labels[i];
    g.appendChild(c);
    g.appendChild(lb);
    container.appendChild(g);
  });

  // 演示滑块
  const row = document.createElement('div');
  row.className = 'param-range-demo';
  row.style.width = '100%';
  row.innerHTML = `<label>实时调节频率</label>
    <input type="range" id="helpFreqSlider" min="0.1" max="5" step="0.05" value="1.0">
    <span id="helpFreqVal">1.0</span>`;
  container.parentElement.appendChild(row);

  const slider = row.querySelector('#helpFreqSlider');
  const valSpan = row.querySelector('#helpFreqVal');
  const liveCanvas = document.createElement('canvas');
  liveCanvas.width = 380; liveCanvas.height = 80;
  liveCanvas.style.borderRadius = '8px';
  liveCanvas.style.border = '1px solid rgba(255,255,255,0.08)';
  const liveWrap = document.createElement('div');
  liveWrap.style.width = '100%';
  liveWrap.style.padding = '0 14px 14px';
  liveWrap.appendChild(liveCanvas);
  container.parentElement.appendChild(liveWrap);
  drawNoiseCanvas(liveCanvas, 1.0, 1, 0.5, 2.0);
  slider.addEventListener('input', () => {
    valSpan.textContent = parseFloat(slider.value).toFixed(2);
    drawNoiseCanvas(liveCanvas, parseFloat(slider.value), 1, 0.5, 2.0);
  });
}

// ---- 振幅可视化 ----
function renderAmplitudeViz(container) {
  container.innerHTML = '';
  const amps = [0.2, 0.5, 1.0, 1.5];
  const labels = ['0.2', '0.5', '1.0', '1.5'];
  const wrapDiv = document.createElement('div');
  wrapDiv.style.cssText = 'display:flex;flex-direction:column;gap:6px;width:100%';

  amps.forEach((a, i) => {
    const g = document.createElement('div');
    g.className = 'viz-wave-wrap';
    const c = document.createElement('canvas');
    c.width = 380; c.height = 50;
    drawWaveCanvas(c, 1.5, a, `振幅 = ${labels[i]}`);
    g.appendChild(c);
    wrapDiv.appendChild(g);
  });
  container.appendChild(wrapDiv);
}

// ---- 倍频可视化 ----
function renderOctavesViz(container) {
  container.innerHTML = '';
  const octs = [1, 2, 4, 6];
  const labels = ['1 倍频', '2 倍频', '4 倍频', '6 倍频'];
  octs.forEach((o, i) => {
    const g = document.createElement('div');
    g.className = 'viz-group';
    const c = document.createElement('canvas');
    c.width = 110; c.height = 80;
    drawNoiseCanvas(c, 2.0, o, 0.5, 2.0);
    const lb = document.createElement('div');
    lb.className = 'viz-label';
    lb.textContent = labels[i];
    g.appendChild(c);
    g.appendChild(lb);
    container.appendChild(g);
  });
}

// ---- 持久度可视化 ----
function renderPersistenceViz(container) {
  container.innerHTML = '';
  // 绘制衰减曲线图
  const c = document.createElement('canvas');
  c.width = 370; c.height = 160;
  container.appendChild(c);

  const ctx = c.getContext('2d');
  const w = c.width, h = c.height;
  ctx.fillStyle = 'rgba(10,13,26,0.6)';
  ctx.fillRect(0, 0, w, h);

  const MARGIN = { left: 38, right: 16, top: 14, bottom: 26 };
  const pw = w - MARGIN.left - MARGIN.right;
  const ph = h - MARGIN.top - MARGIN.bottom;

  // 坐标轴
  ctx.strokeStyle = 'rgba(255,255,255,0.15)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(MARGIN.left, MARGIN.top);
  ctx.lineTo(MARGIN.left, MARGIN.top + ph);
  ctx.lineTo(MARGIN.left + pw, MARGIN.top + ph);
  ctx.stroke();

  // X 轴标签（倍频）
  const maxOct = 6;
  ctx.fillStyle = 'rgba(255,255,255,0.35)';
  ctx.font = '9px monospace';
  for (let o = 1; o <= maxOct; o++) {
    const x = MARGIN.left + ((o - 1) / (maxOct - 1)) * pw;
    ctx.fillText(`${o}`, x - 3, MARGIN.top + ph + 14);
  }
  ctx.fillText('倍频', MARGIN.left + pw / 2 - 10, MARGIN.top + ph + 22);
  ctx.save();
  ctx.translate(12, MARGIN.top + ph / 2 + 16);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText('振幅', 0, 0);
  ctx.restore();

  const curves = [
    { p: 0.3, color: '#5c9dff', label: 'p=0.3' },
    { p: 0.5, color: '#00c853', label: 'p=0.5' },
    { p: 0.7, color: '#ffab00', label: 'p=0.7' },
    { p: 0.9, color: '#ff5252', label: 'p=0.9' },
  ];

  curves.forEach(({ p, color, label }) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let o = 1; o <= maxOct; o++) {
      const amp = Math.pow(p, o - 1);
      const x = MARGIN.left + ((o - 1) / (maxOct - 1)) * pw;
      const y = MARGIN.top + ph - amp * ph;
      if (o === 1) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    // 图例
    const legendX = MARGIN.left + pw + 4;
    const legendY = MARGIN.top + ph - Math.pow(p, 0) * ph + 4;
    ctx.fillStyle = color;
    ctx.font = '9px monospace';
    ctx.fillText(label, MARGIN.left + pw - 32, MARGIN.top + ph - Math.pow(p, maxOct - 1) * ph - 3);
  });
}

// ---- 隙度可视化 ----
function renderLacunarityViz(container) {
  container.innerHTML = '';

  const c = document.createElement('canvas');
  c.width = 370; c.height = 160;
  container.appendChild(c);

  const ctx = c.getContext('2d');
  const w = c.width, h = c.height;
  ctx.fillStyle = 'rgba(10,13,26,0.6)';
  ctx.fillRect(0, 0, w, h);

  const MARGIN = { left: 38, right: 16, top: 14, bottom: 26 };
  const pw = w - MARGIN.left - MARGIN.right;
  const ph = h - MARGIN.top - MARGIN.bottom;

  ctx.strokeStyle = 'rgba(255,255,255,0.15)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(MARGIN.left, MARGIN.top);
  ctx.lineTo(MARGIN.left, MARGIN.top + ph);
  ctx.lineTo(MARGIN.left + pw, MARGIN.top + ph);
  ctx.stroke();

  const maxOct = 5;
  ctx.fillStyle = 'rgba(255,255,255,0.35)';
  ctx.font = '9px monospace';
  for (let o = 1; o <= maxOct; o++) {
    const x = MARGIN.left + ((o - 1) / (maxOct - 1)) * pw;
    ctx.fillText(`${o}`, x - 3, MARGIN.top + ph + 14);
  }
  ctx.fillText('倍频', MARGIN.left + pw / 2 - 10, MARGIN.top + ph + 22);
  ctx.save();
  ctx.translate(12, MARGIN.top + ph / 2 + 16);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText('频率', 0, 0);
  ctx.restore();

  const curves = [
    { l: 1.5, color: '#5c9dff', label: 'l=1.5' },
    { l: 2.0, color: '#00c853', label: 'l=2.0' },
    { l: 3.0, color: '#ffab00', label: 'l=3.0' },
  ];

  // 最大频率（用于归一化）
  const maxFreq = Math.pow(3.0, maxOct - 1);

  curves.forEach(({ l, color, label }) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let o = 1; o <= maxOct; o++) {
      const freq = Math.pow(l, o - 1) / maxFreq;
      const x = MARGIN.left + ((o - 1) / (maxOct - 1)) * pw;
      const y = MARGIN.top + ph - Math.min(freq, 1) * ph;
      if (o === 1) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
    const lastFreq = Math.min(Math.pow(l, maxOct - 1) / maxFreq, 1);
    const lx = MARGIN.left + pw - 28;
    const ly = MARGIN.top + ph - lastFreq * ph - 3;
    ctx.fillStyle = color;
    ctx.font = '9px monospace';
    ctx.fillText(label, lx, ly);
  });

  // 不同隙度的噪声对比图
  const lacs = [1.5, 2.0, 4.0];
  const lacLabels = ['隙度=1.5', '隙度=2.0', '隙度=4.0'];
  const row = document.createElement('div');
  row.style.cssText = 'display:flex;gap:10px;padding:10px 0 0;justify-content:center';
  lacs.forEach((l, i) => {
    const g = document.createElement('div');
    g.className = 'viz-group';
    const nc = document.createElement('canvas');
    nc.width = 100; nc.height = 70;
    drawNoiseCanvas(nc, 1.0, 4, 0.5, l);
    const lb = document.createElement('div');
    lb.className = 'viz-label';
    lb.textContent = lacLabels[i];
    g.appendChild(nc);
    g.appendChild(lb);
    row.appendChild(g);
  });
  container.appendChild(row);
}

// ===================== 弹窗开关逻辑 =====================

const VIZ_RENDERERS = {
  frequency: renderFrequencyViz,
  amplitude: renderAmplitudeViz,
  octaves: renderOctavesViz,
  persistence: renderPersistenceViz,
  lacunarity: renderLacunarityViz,
};

window.openParamHelp = function(paramKey) {
  const data = PARAM_HELP_DATA[paramKey];
  if (!data) return;

  // 填充内容
  document.getElementById('paramHelpTitle').textContent = data.title;
  document.getElementById('paramHelpSubtitle').textContent = data.subtitle;
  document.getElementById('paramHelpDesc').innerHTML = data.desc;

  // 图标
  const iconEl = document.querySelector('.param-help-icon i');
  iconEl.className = data.icon;

  // 标签
  const tagsEl = document.getElementById('paramHelpTags');
  tagsEl.innerHTML = data.tags.map(t =>
    `<span class="param-help-tag ${t.cls}"><i class="${t.icon}"></i>${t.text}</span>`
  ).join('');

  // 可视化标题
  document.getElementById('paramHelpVizTitle').textContent = data.vizTitle;

  // 可视化区域
  const vizArea = document.getElementById('paramHelpVizArea');
  vizArea.innerHTML = '';
  const renderer = VIZ_RENDERERS[data.vizType];
  if (renderer) renderer(vizArea);

  // 显示弹窗
  const overlay = document.getElementById('paramHelpOverlay');
  overlay.classList.add('active');
  document.body.style.overflow = 'hidden';
};

window.closeParamHelp = function(event, force) {
  if (!force && event && event.target !== document.getElementById('paramHelpOverlay')) return;
  const overlay = document.getElementById('paramHelpOverlay');
  overlay.classList.remove('active');
  document.body.style.overflow = '';
};

// ESC 键关闭
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') {
    const overlay = document.getElementById('paramHelpOverlay');
    if (overlay && overlay.classList.contains('active')) {
      closeParamHelp(null, true);
    }
  }
});
