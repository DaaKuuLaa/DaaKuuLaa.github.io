// ==========================================
// 第五页：噪声计算可视化（重制版）
// Noise Calculation Visualization - Rewrite
// 修复：去除 initialize() 内的双重 DOMContentLoaded 监听
//       MutationObserver 只监听最小范围两个元素
//       render() 不再内嵌 rAF（由 _tick / idle loop 统一管理）
// ==========================================

class NoiseCalculationVisualization {
    constructor(canvas) {
        this.canvas = canvas;
        this.ctx    = canvas.getContext('2d');

        // ---- 动画状态 ----
        this.animationId  = null;   // 主动画 rAF id
        this._idleLoopId  = null;   // 待机 rAF id
        this.isAnimating  = false;
        this.phase        = 0;      // 0=待机 1~5=各阶段
        this.phaseProgress = 0;     // 0~1
        this.speed        = 5;      // 1~10

        // ---- Perlin 梯度 ----
        // 四个角：00(左上), 10(右上), 01(左下), 11(右下)
        this.gradients = this._makeGradients();

        // ---- 查询点 P（单元格内相对坐标，0~1）----
        this.pU = 0.35;
        this.pV = 0.42;

        // ---- 拖拽状态 ----
        this._dragging = false;

        // ---- 布局缓存 ----
        this.layout = null;

        // ---- 时间轴（idle 脉冲动效用） ----
        this._t0 = performance.now();

        // ---- 计算缓存 ----
        this.calc = {};

        // ---- 全图热力图 ----
        this.showHeatmap  = false;
        this.heatmapAlpha = 0;
        this._heatmapCanvas = null;
        this._heatmapDirty  = true;

        // ---- easing ----
        this._ease    = t => t < 0.5 ? 2 * t * t : -1 + (4 - 2 * t) * t;
        this._easeOut = t => 1 - Math.pow(1 - t, 3);

        // 初始化
        this._fitCanvas();
        this._recompute();
        this._setupEventListeners();
        this._startIdleLoop();
    }

    // ====================================================
    //  核心计算
    // ====================================================

    _makeGradients() {
        return [0, 1, 2, 3].map(() => {
            const a = Math.random() * Math.PI * 2;
            return { x: Math.cos(a), y: Math.sin(a) };
        });
    }

    /** Smoothstep 5次多项式 */
    _smoothstep(t) { return t * t * t * (t * (t * 6 - 15) + 10); }

    /** 线性插值 */
    _lerp(a, b, t) { return a + (b - a) * t; }

    /**
     * 根据当前 pU/pV 重算所有中间值，缓存到 this.calc
     * 角点顺序：g[0]=00(左上), g[1]=10(右上), g[2]=01(左下), g[3]=11(右下)
     */
    _recompute() {
        const u = this.pU, v = this.pV;
        const g = this.gradients;

        // 四角点到 P 的距离向量，再做点积
        const corners = [
            { cx: 0, cy: 0, gi: 0 },
            { cx: 1, cy: 0, gi: 1 },
            { cx: 0, cy: 1, gi: 2 },
            { cx: 1, cy: 1, gi: 3 },
        ];
        const dots = corners.map(c => {
            const dx = u - c.cx, dy = v - c.cy;
            return dx * g[c.gi].x + dy * g[c.gi].y;
        });

        const su = this._smoothstep(u);
        const sv = this._smoothstep(v);

        // E = lerp(n00, n10, su) 上边插值
        // F = lerp(n01, n11, su) 下边插值
        const E = this._lerp(dots[0], dots[1], su);
        const F = this._lerp(dots[2], dots[3], su);
        const noise = this._lerp(E, F, sv);

        this.calc = { u, v, su, sv, dots, E, F, noise };
    }

    // ====================================================
    //  布局 & Canvas 适配
    // ====================================================

    _fitCanvas() {
        const container = this.canvas.parentElement;
        if (!container) return;
        const W = container.clientWidth  || 640;
        const H = container.clientHeight || 400;
        this.canvas.width  = W;
        this.canvas.height = H;

        const pad      = Math.round(Math.min(W, H) * 0.05);
        const gridSize = Math.min(H - pad * 2, (W - pad * 2) * 0.52);
        const gx       = pad;
        const gy       = Math.round((H - gridSize) / 2);
        const formulaW = W - gridSize - pad * 3;

        this.layout = {
            W, H, pad, gridSize,
            gx, gy,
            formulaX: gx + gridSize + pad,
            formulaW,
        };
        this._heatmapDirty = true;
    }

    /** 将单元格相对坐标 (u,v) ∈ [0,1] 转换为 canvas 像素坐标 */
    _toCanvas(u, v) {
        const { gx, gy, gridSize } = this.layout;
        return { x: gx + u * gridSize, y: gy + v * gridSize };
    }

    // ====================================================
    //  事件绑定
    // ====================================================

    _setupEventListeners() {
        // Canvas 拖拽 P 点
        this.canvas.addEventListener('mousedown', e => this._onMouseDown(e));
        this.canvas.addEventListener('mousemove', e => this._onMouseMove(e));
        this.canvas.addEventListener('mouseup',   () => { this._dragging = false; });
        this.canvas.addEventListener('mouseleave',() => { this._dragging = false; });

        // 触摸支持
        this.canvas.addEventListener('touchstart', e => { e.preventDefault(); this._onMouseDown(e.touches[0]); }, { passive: false });
        this.canvas.addEventListener('touchmove',  e => { e.preventDefault(); this._onMouseMove(e.touches[0]); }, { passive: false });
        this.canvas.addEventListener('touchend',   () => { this._dragging = false; });

        // 响应式
        window.addEventListener('resize', () => {
            this._fitCanvas();
            this._recompute();
            this.render();
        });

        // 速度滑块
        const slider = document.getElementById('calculationSpeed');
        if (slider) {
            slider.addEventListener('input', e => { this.speed = parseInt(e.target.value); });
        }

        // 全图噪声值切换按钮
        const heatmapBtn = document.getElementById('toggleHeatmapBtn');
        if (heatmapBtn) {
            heatmapBtn.addEventListener('click', () => this.toggleHeatmap());
        }
    }

    _onMouseDown(e) {
        const rect = this.canvas.getBoundingClientRect();
        const mx = (e.clientX - rect.left) * (this.canvas.width  / rect.width);
        const my = (e.clientY - rect.top)  * (this.canvas.height / rect.height);
        const P  = this._toCanvas(this.pU, this.pV);
        const dist = Math.hypot(mx - P.x, my - P.y);
        if (dist < 18) { this._dragging = true; }
    }

    _onMouseMove(e) {
        if (!this._dragging) return;
        const rect = this.canvas.getBoundingClientRect();
        const mx = (e.clientX - rect.left) * (this.canvas.width  / rect.width);
        const my = (e.clientY - rect.top)  * (this.canvas.height / rect.height);
        const { gx, gy, gridSize } = this.layout;
        this.pU = Math.max(0.05, Math.min(0.95, (mx - gx) / gridSize));
        this.pV = Math.max(0.05, Math.min(0.95, (my - gy) / gridSize));
        this._recompute();
        if (!this.isAnimating && !this._idleLoopId) this._startIdleLoop();
    }

    // ====================================================
    //  公共控制 API
    // ====================================================

    animateCalculation() {
        if (this.isAnimating) return;
        this._stopIdleLoop();
        this.isAnimating   = true;
        this.phase         = 1;
        this.phaseProgress = 0;
        this._recompute();
        this.animationId = requestAnimationFrame(() => this._tick());
    }

    resetVisualization() {
        this._stopIdleLoop();
        if (this.animationId) { cancelAnimationFrame(this.animationId); this.animationId = null; }
        this.isAnimating   = false;
        this.phase         = 0;
        this.phaseProgress = 0;
        this._syncStepUI(0);
        this._recompute();
        this.render();
        this._startIdleLoop();
    }

    randomizeGradients() {
        this.gradients      = this._makeGradients();
        this._heatmapDirty  = true;
        this._recompute();
        this.render();
    }

    toggleHeatmap() {
        this.showHeatmap = !this.showHeatmap;
        const btn = document.getElementById('toggleHeatmapBtn');
        if (btn) {
            if (this.showHeatmap) {
                btn.innerHTML = '<i class="fas fa-eye-slash"></i> 隐藏噪声图';
                btn.classList.add('active');
            } else {
                btn.innerHTML = '<i class="fas fa-th"></i> 全图噪声值';
                btn.classList.remove('active');
            }
        }
        if (!this.isAnimating && !this._idleLoopId) this._startIdleLoop();
    }

    // ====================================================
    //  动画循环
    // ====================================================

    /** 主动画 tick */
    _tick() {
        this.animationId = null;
        if (!this.isAnimating) return;

        const phaseDuration = { 1: 40, 2: 35, 3: 55, 4: 55, 5: 60 };
        const dur   = phaseDuration[this.phase] || 50;
        const delta = this.speed / (dur * 3);

        this.phaseProgress = Math.min(1, this.phaseProgress + delta);

        const stepMap = { 1: 0, 2: 0, 3: 1, 4: 2, 5: 2 };
        this._syncStepUI(stepMap[this.phase] ?? 0);

        this._updateHeatmapAlpha();
        this.render();

        if (this.phaseProgress >= 1) {
            this.phase++;
            this.phaseProgress = 0;
            if (this.phase > 5) {
                this.isAnimating = false;
                this._syncStepUI(3);
                this._startIdleLoop();
                return;
            }
        }

        this.animationId = requestAnimationFrame(() => this._tick());
    }

    /** 待机渲染循环：保持 P 点脉冲动效 & 热力图淡入淡出 */
    _startIdleLoop() {
        if (this.isAnimating) return;
        if (this._idleLoopId)  return;
        const loop = () => {
            if (this.isAnimating) { this._idleLoopId = null; return; }
            this._updateHeatmapAlpha();
            this.render();
            // 如果 alpha 已稳定且没有拖拽，每秒约30帧即可
            this._idleLoopId = requestAnimationFrame(loop);
        };
        this._idleLoopId = requestAnimationFrame(loop);
    }

    _stopIdleLoop() {
        if (this._idleLoopId) { cancelAnimationFrame(this._idleLoopId); this._idleLoopId = null; }
    }

    _syncStepUI(active) {
        const steps = document.querySelectorAll('#noiseCalcSteps .info-step');
        steps.forEach((s, i) => s.classList.toggle('active', i === active));
    }

    // ====================================================
    //  全图热力图
    // ====================================================

    _updateHeatmapAlpha() {
        const target = this.showHeatmap ? 0.82 : 0;
        const speed  = 0.05;
        if (Math.abs(this.heatmapAlpha - target) < 0.01) {
            this.heatmapAlpha = target;
        } else {
            this.heatmapAlpha += (target - this.heatmapAlpha) * speed * 3;
        }
    }

    _buildHeatmapCache(size) {
        const res  = Math.ceil(size);
        const oc   = document.createElement('canvas');
        oc.width   = res;
        oc.height  = res;
        const octx = oc.getContext('2d');
        const img  = octx.createImageData(res, res);
        const data = img.data;
        for (let py = 0; py < res; py++) {
            for (let px = 0; px < res; px++) {
                const u = px / (res - 1);
                const v = py / (res - 1);
                const n = this._noiseAt(u, v);
                const t = (n + 1) / 2;
                let r, g, b;
                if (t < 0.5) {
                    const s = t / 0.5;
                    r = Math.round(10  + (18  - 10) * s);
                    g = Math.round(30  + (20  - 30) * s);
                    b = Math.round(80  + (40  - 80) * s);
                } else {
                    const s = (t - 0.5) / 0.5;
                    r = Math.round(18  + (255 - 18) * s);
                    g = Math.round(20  + (160 - 20) * s);
                    b = Math.round(40  + (30  - 40) * s);
                }
                const idx = (py * res + px) * 4;
                data[idx]     = r;
                data[idx + 1] = g;
                data[idx + 2] = b;
                data[idx + 3] = 230;
            }
        }
        octx.putImageData(img, 0, 0);
        return oc;
    }

    _drawHeatmap(ctx, alpha) {
        if (!this.layout) return;
        const { gx, gy, gridSize } = this.layout;
        const size = Math.ceil(gridSize);
        if (this._heatmapDirty || !this._heatmapCanvas || this._heatmapCanvas.width !== size) {
            this._heatmapCanvas = this._buildHeatmapCache(size);
            this._heatmapDirty  = false;
        }
        ctx.save();
        ctx.globalAlpha = alpha;
        ctx.beginPath();
        ctx.rect(gx, gy, gridSize, gridSize);
        ctx.clip();
        ctx.drawImage(this._heatmapCanvas, gx, gy, gridSize, gridSize);
        ctx.globalAlpha = alpha * 0.15;
        ctx.strokeStyle = 'rgba(255,255,255,0.5)';
        ctx.lineWidth   = 0.5;
        const step = gridSize / 8;
        for (let i = 1; i < 8; i++) {
            ctx.beginPath(); ctx.moveTo(gx + i * step, gy); ctx.lineTo(gx + i * step, gy + gridSize); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(gx, gy + i * step); ctx.lineTo(gx + gridSize, gy + i * step); ctx.stroke();
        }
        ctx.restore();
    }

    _noiseAt(u, v) {
        const g = this.gradients;
        const corners = [
            { cx: 0, cy: 0, gi: 0 }, { cx: 1, cy: 0, gi: 1 },
            { cx: 0, cy: 1, gi: 2 }, { cx: 1, cy: 1, gi: 3 },
        ];
        const dots = corners.map(c => (u - c.cx) * g[c.gi].x + (v - c.cy) * g[c.gi].y);
        const su = this._smoothstep(u), sv = this._smoothstep(v);
        return this._lerp(this._lerp(dots[0], dots[1], su), this._lerp(dots[2], dots[3], su), sv);
    }

    // ====================================================
    //  主渲染
    // ====================================================

    render() {
        if (!this.ctx || !this.layout) return;
        const ctx  = this.ctx;
        const { W, H } = this.layout;

        ctx.clearRect(0, 0, W, H);

        const p = this.phase, pp = this.phaseProgress;

        // 各阶段透明度
        const gradAlpha   = p === 0 ? 1 : (p === 1 ? this._easeOut(pp) : 1);
        const pvecAlpha   = p <= 1 ? (p === 1 ? this._easeOut(pp) : 0) : 1;
        const distAlpha   = p <= 2 ? (p === 2 ? this._easeOut(pp) : 0) : 1;
        const xIntAlpha   = p <= 3 ? (p === 3 ? this._easeOut(pp) : 0) : 1;
        const yIntAlpha   = p <= 4 ? (p === 4 ? this._easeOut(pp) : 0) : 1;
        this._drawGrid(ctx);
        if (this.heatmapAlpha > 0.01) this._drawHeatmap(ctx, this.heatmapAlpha);
        this._drawGradients(ctx, gradAlpha, pvecAlpha);
        if (p >= 2) this._drawDistanceVectors(ctx, distAlpha);
        if (p >= 3) this._drawXInterpolation(ctx, xIntAlpha, p === 3 ? pp : 1);
        if (p >= 4) this._drawYInterpolation(ctx, yIntAlpha, p === 4 ? pp : 1);
        // noise 最终值仅在右侧公式面板中展示，左侧不重复显示
        this._drawQueryPoint(ctx);
        this._drawFormulas(ctx);
        if (p === 0) this._drawHint(ctx);
    }

    // ====================================================
    //  绘制：网格
    // ====================================================

    _drawGrid(ctx) {
        const { gx, gy, gridSize } = this.layout;
        // 单元格背景
        ctx.fillStyle = 'rgba(10,14,30,0.85)';
        ctx.fillRect(gx, gy, gridSize, gridSize);

        // 边框
        ctx.strokeStyle = 'rgba(92,157,255,0.45)';
        ctx.lineWidth   = 1.5;
        ctx.strokeRect(gx, gy, gridSize, gridSize);

        // 细分网格线
        ctx.strokeStyle = 'rgba(92,157,255,0.1)';
        ctx.lineWidth   = 0.5;
        const div = 4;
        for (let i = 1; i < div; i++) {
            const d = (gridSize / div) * i;
            ctx.beginPath(); ctx.moveTo(gx + d, gy); ctx.lineTo(gx + d, gy + gridSize); ctx.stroke();
            ctx.beginPath(); ctx.moveTo(gx, gy + d); ctx.lineTo(gx + gridSize, gy + d); ctx.stroke();
        }

        // 角点标签
        const labels = [
            { u: 0, v: 0, name: '(0,0)' }, { u: 1, v: 0, name: '(1,0)' },
            { u: 0, v: 1, name: '(0,1)' }, { u: 1, v: 1, name: '(1,1)' },
        ];
        ctx.fillStyle  = 'rgba(180,200,255,0.55)';
        ctx.font       = `${Math.max(9, gridSize * 0.038)}px monospace`;
        ctx.textAlign  = 'center';
        labels.forEach(l => {
            const p = this._toCanvas(l.u, l.v);
            const ox = l.u === 0 ? 18 : -18;
            const oy = l.v === 0 ? -10 : 14;
            ctx.fillText(l.name, p.x + ox, p.y + oy);
        });
    }

    // ====================================================
    //  绘制：角点 + 梯度箭头
    // ====================================================

    _drawGradients(ctx, cornerAlpha, arrowAlpha) {
        const g      = this.gradients;
        const colors = ['#00c853', '#ffab00', '#ff5252', '#5c9dff'];
        const corners = [
            { u: 0, v: 0, gi: 0 }, { u: 1, v: 0, gi: 1 },
            { u: 0, v: 1, gi: 2 }, { u: 1, v: 1, gi: 3 },
        ];
        const arrowLen = this.layout.gridSize * 0.16;
        const r        = Math.max(5, this.layout.gridSize * 0.04);

        corners.forEach(({ u, v, gi }) => {
            const p   = this._toCanvas(u, v);
            const col = colors[gi];

            // 角点圆
            ctx.save();
            ctx.globalAlpha = cornerAlpha;
            ctx.beginPath();
            ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
            ctx.fillStyle   = col;
            ctx.fill();
            ctx.strokeStyle = 'rgba(255,255,255,0.8)';
            ctx.lineWidth   = 1.5;
            ctx.stroke();
            ctx.restore();

            // 梯度箭头
            ctx.save();
            ctx.globalAlpha = arrowAlpha;
            const gv  = g[gi];
            const ex  = p.x + gv.x * arrowLen;
            const ey  = p.y + gv.y * arrowLen;
            this._drawArrow(ctx, p.x, p.y, ex, ey, col, 1.8);
            ctx.restore();

            // 点乘值标签（phase>=2时显示）
            if (this.phase >= 2) {
                const dotVal = this.calc.dots ? this.calc.dots[gi] : 0;
                const lx = p.x + (u === 0 ? 24 : -24);
                const ly = p.y + (v === 0 ? 22 : -14);
                ctx.save();
                ctx.globalAlpha = Math.min(1, arrowAlpha + 0.3);
                ctx.fillStyle   = col;
                ctx.font        = `bold ${Math.max(9, this.layout.gridSize * 0.036)}px monospace`;
                ctx.textAlign   = 'center';
                ctx.fillText(`n=${dotVal.toFixed(3)}`, lx, ly);
                ctx.restore();
            }
        });
    }

    /** 绘制箭头 */
    _drawArrow(ctx, x1, y1, x2, y2, color, lw = 2) {
        const angle = Math.atan2(y2 - y1, x2 - x1);
        const hLen  = 7;
        ctx.strokeStyle = color;
        ctx.fillStyle   = color;
        ctx.lineWidth   = lw;
        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.lineTo(x2, y2);
        ctx.stroke();
        ctx.beginPath();
        ctx.moveTo(x2, y2);
        ctx.lineTo(x2 - hLen * Math.cos(angle - 0.4), y2 - hLen * Math.sin(angle - 0.4));
        ctx.lineTo(x2 - hLen * Math.cos(angle + 0.4), y2 - hLen * Math.sin(angle + 0.4));
        ctx.closePath();
        ctx.fill();
    }

    // ====================================================
    //  绘制：距离向量（从角点到P）
    // ====================================================

    _drawDistanceVectors(ctx, alpha) {
        const corners = [
            { u: 0, v: 0 }, { u: 1, v: 0 },
            { u: 0, v: 1 }, { u: 1, v: 1 },
        ];
        const P      = this._toCanvas(this.pU, this.pV);
        const colors = ['#00c853', '#ffab00', '#ff5252', '#5c9dff'];

        ctx.save();
        ctx.globalAlpha = alpha;
        ctx.setLineDash([4, 4]);

        corners.forEach(({ u, v }, i) => {
            const c = this._toCanvas(u, v);
            ctx.strokeStyle = colors[i];
            ctx.lineWidth   = 1.2;
            ctx.beginPath();
            ctx.moveTo(c.x, c.y);
            ctx.lineTo(P.x, P.y);
            ctx.stroke();
        });

        ctx.setLineDash([]);
        ctx.restore();
    }

    // ====================================================
    //  绘制：X轴插值（E 和 F 点）
    // ====================================================

    _drawXInterpolation(ctx, alpha, progress) {
        const { gridSize } = this.layout;
        const { su }       = this.calc;
        const P            = this._toCanvas(this.pU, this.pV);

        // E 点（上边：从(0,0)到(1,0)插值到 su 位置）
        const E0 = this._toCanvas(0,  0);
        const E1 = this._toCanvas(1,  0);
        const ETarget = { x: E0.x + (E1.x - E0.x) * su, y: E0.y };
        const eAnim   = { x: E0.x + (ETarget.x - E0.x) * this._easeOut(progress), y: E0.y };

        // F 点（下边：从(0,1)到(1,1)插值到 su 位置）
        const F0 = this._toCanvas(0, 1);
        const F1 = this._toCanvas(1, 1);
        const FTarget = { x: F0.x + (F1.x - F0.x) * su, y: F0.y };
        const fAnim   = { x: F0.x + (FTarget.x - F0.x) * this._easeOut(progress), y: F0.y };

        const rSm = Math.max(4, gridSize * 0.03);

        ctx.save();
        ctx.globalAlpha = alpha;

        // E 滑动轨迹
        ctx.strokeStyle = '#ffab00';
        ctx.lineWidth   = 2;
        ctx.beginPath(); ctx.moveTo(E0.x, E0.y); ctx.lineTo(eAnim.x, eAnim.y); ctx.stroke();

        // F 滑动轨迹
        ctx.strokeStyle = '#00e5ff';
        ctx.lineWidth   = 2;
        ctx.beginPath(); ctx.moveTo(F0.x, F0.y); ctx.lineTo(fAnim.x, fAnim.y); ctx.stroke();

        // E 点
        ctx.beginPath();
        ctx.arc(eAnim.x, eAnim.y, rSm, 0, Math.PI * 2);
        ctx.fillStyle = '#ffab00';
        ctx.fill();
        ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke();

        // E 标签
        const fs = Math.max(9, gridSize * 0.036);
        ctx.fillStyle = '#ffab00';
        ctx.font      = `bold ${fs}px monospace`;
        ctx.textAlign = 'center';
        ctx.fillText('E', eAnim.x, eAnim.y - rSm - 6);
        if (progress > 0.7) {
            ctx.font = `${fs * 0.85}px monospace`;
            ctx.fillText(this.calc.E.toFixed(3), eAnim.x, eAnim.y - rSm - 16);
        }

        // F 点
        ctx.beginPath();
        ctx.arc(fAnim.x, fAnim.y, rSm, 0, Math.PI * 2);
        ctx.fillStyle = '#00e5ff';
        ctx.fill();
        ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke();

        // F 标签
        ctx.fillStyle = '#00e5ff';
        ctx.font      = `bold ${fs}px monospace`;
        ctx.fillText('F', fAnim.x, fAnim.y + rSm + 14);
        if (progress > 0.7) {
            ctx.font = `${fs * 0.85}px monospace`;
            ctx.fillText(this.calc.F.toFixed(3), fAnim.x, fAnim.y + rSm + 24);
        }

        // E→F 连线（progress>0.8 才显示）
        if (progress > 0.8) {
            ctx.strokeStyle = 'rgba(255,255,255,0.25)';
            ctx.lineWidth   = 1;
            ctx.setLineDash([3, 3]);
            ctx.beginPath();
            ctx.moveTo(ETarget.x, ETarget.y);
            ctx.lineTo(FTarget.x, FTarget.y);
            ctx.stroke();
            ctx.setLineDash([]);
        }

        // E→P 的垂直指引线（P 的 X 投影）
        ctx.strokeStyle = 'rgba(255,171,0,0.3)';
        ctx.lineWidth   = 1;
        ctx.setLineDash([2, 4]);
        ctx.beginPath();
        ctx.moveTo(ETarget.x, ETarget.y);
        ctx.lineTo(ETarget.x, P.y);
        ctx.stroke();
        ctx.setLineDash([]);

        ctx.restore();
    }

    // ====================================================
    //  绘制：Y轴插值（G = 最终噪声）
    // ====================================================

    _drawYInterpolation(ctx, alpha, progress) {
        const { gridSize } = this.layout;
        const { su, sv }   = this.calc;

        const E0 = this._toCanvas(0, 0);
        const F0 = this._toCanvas(0, 1);
        const ETarget = { x: E0.x + (this._toCanvas(1,0).x - E0.x) * su, y: E0.y };
        const FTarget = { x: F0.x + (this._toCanvas(1,1).x - F0.x) * su, y: F0.y };

        const GTarget = {
            x: ETarget.x + (FTarget.x - ETarget.x) * sv,
            y: ETarget.y + (FTarget.y - ETarget.y) * sv,
        };
        const GAnim = {
            x: ETarget.x + (GTarget.x - ETarget.x) * this._easeOut(progress),
            y: ETarget.y + (GTarget.y - ETarget.y) * this._easeOut(progress),
        };

        const rSm = Math.max(5, gridSize * 0.035);
        const fs  = Math.max(9, gridSize * 0.036);

        ctx.save();
        ctx.globalAlpha = alpha;

        // G 滑动轨迹
        ctx.strokeStyle = '#a855f7';
        ctx.lineWidth   = 2;
        ctx.beginPath(); ctx.moveTo(ETarget.x, ETarget.y); ctx.lineTo(GAnim.x, GAnim.y); ctx.stroke();

        // G 点（发光效果）
        ctx.shadowColor  = '#a855f7';
        ctx.shadowBlur   = 12;
        ctx.beginPath();
        ctx.arc(GAnim.x, GAnim.y, rSm * 1.2, 0, Math.PI * 2);
        ctx.fillStyle = '#a855f7';
        ctx.fill();
        ctx.shadowBlur = 0;
        ctx.strokeStyle = '#fff'; ctx.lineWidth = 2; ctx.stroke();

        // G 标签
        ctx.fillStyle = '#a855f7';
        ctx.font      = `bold ${fs}px monospace`;
        ctx.textAlign = 'center';
        ctx.fillText('G', GAnim.x + rSm * 1.5 + 6, GAnim.y - 2);
        if (progress > 0.8) {
            ctx.font = `${fs * 0.85}px monospace`;
            ctx.fillText(`= ${this.calc.noise.toFixed(3)}`, GAnim.x + rSm * 1.5 + 6, GAnim.y + 12);
        }

        ctx.restore();
    }

    // ====================================================
    //  绘制：最终结果高亮
    // ====================================================

    // ====================================================
    //  绘制：查询点 P（带脉冲动效）
    // ====================================================

    _drawQueryPoint(ctx) {
        const P   = this._toCanvas(this.pU, this.pV);
        const t   = (performance.now() - this._t0) / 1000;
        const r   = Math.max(6, this.layout.gridSize * 0.038);
        const pulse = (Math.sin(t * 3) * 0.5 + 0.5) * 0.5; // 0~0.5

        // 脉冲外圈（仅待机或第1阶段之前）
        if (this.phase <= 1) {
            ctx.save();
            ctx.globalAlpha = 0.4 * (1 - pulse);
            ctx.beginPath();
            ctx.arc(P.x, P.y, r + 10 + pulse * 12, 0, Math.PI * 2);
            ctx.strokeStyle = '#fff';
            ctx.lineWidth   = 1.5;
            ctx.stroke();
            ctx.restore();
        }

        // P 点本体
        ctx.beginPath();
        ctx.arc(P.x, P.y, r, 0, Math.PI * 2);
        ctx.fillStyle   = '#ffffff';
        ctx.fill();
        ctx.strokeStyle = '#a855f7';
        ctx.lineWidth   = 2.5;
        ctx.stroke();

        ctx.fillStyle = '#1a1f3e';
        ctx.font      = `bold ${Math.max(8, r * 1.1)}px sans-serif`;
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('P', P.x, P.y);
        ctx.textBaseline = 'alphabetic';

        // P 坐标标签
        ctx.fillStyle = 'rgba(255,255,255,0.75)';
        ctx.font      = `${Math.max(8, this.layout.gridSize * 0.032)}px monospace`;
        ctx.textAlign = 'left';
        ctx.fillText(`(${this.pU.toFixed(2)}, ${this.pV.toFixed(2)})`, P.x + r + 6, P.y - 2);
    }

    // ====================================================
    //  绘制：右侧公式面板
    // ====================================================

    _drawFormulas(ctx) {
        const { formulaX, formulaW, H } = this.layout;
        if (formulaW < 60) return;

        const p  = this.phase;
        const c  = this.calc;
        // 字号根据面板宽度自适应，上限 11px，确保全部内容可容纳
        const fs    = Math.max(8, Math.min(formulaW * 0.075, 11));
        const lineH = fs * 1.5;
        const panelH = H * 0.88;
        const panelY = (H - panelH) / 2;

        // 背景面板
        ctx.fillStyle   = 'rgba(10,14,35,0.7)';
        ctx.strokeStyle = 'rgba(92,157,255,0.2)';
        ctx.lineWidth   = 1;
        this._roundRect(ctx, formulaX, panelY, formulaW, panelH, 8);
        ctx.fill(); ctx.stroke();

        const x0 = formulaX + 8;
        let y = panelY + lineH;

        // draw：单行文字
        const draw = (text, color, bold = false, highlight = false) => {
            ctx.save();
            if (highlight) {
                ctx.fillStyle = 'rgba(92,157,255,0.12)';
                ctx.fillRect(formulaX + 3, y - fs * 0.85, formulaW - 6, fs * 1.3);
            }
            ctx.fillStyle = color;
            ctx.font      = `${bold ? 'bold ' : ''}${fs}px monospace`;
            ctx.textAlign = 'left';
            ctx.fillText(text, x0, y);
            ctx.restore();
            y += lineH;
        };

        // drawPair：两列并排（key + value），节省行数
        const drawPair = (k1, v1, c1, k2, v2, c2, hl = false) => {
            const half = formulaW / 2 - 4;
            ctx.save();
            if (hl) {
                ctx.fillStyle = 'rgba(92,157,255,0.1)';
                ctx.fillRect(formulaX + 3, y - fs * 0.85, formulaW - 6, fs * 1.3);
            }
            ctx.font = `${fs}px monospace`;
            ctx.textAlign = 'left';
            ctx.fillStyle = c1; ctx.fillText(`${k1}${v1}`, x0, y);
            ctx.fillStyle = c2; ctx.fillText(`${k2}${v2}`, formulaX + half + 6, y);
            ctx.restore();
            y += lineH;
        };

        const sep = () => {
            ctx.strokeStyle = 'rgba(92,157,255,0.18)';
            ctx.lineWidth   = 0.5;
            ctx.beginPath();
            ctx.moveTo(formulaX + 6, y - lineH * 0.4);
            ctx.lineTo(formulaX + formulaW - 6, y - lineH * 0.4);
            ctx.stroke();
            y += lineH * 0.15;
        };

        // ① 标题 + 坐标（同行）
        draw('双线性插值', '#8eb8ff', true);
        drawPair('u=', c.u?.toFixed(3) ?? '?', '#e0e7ff', 'v=', c.v?.toFixed(3) ?? '?', '#e0e7ff');
        sep();

        // ② Smoothstep（阶段1高亮）
        const sHL = p === 1;
        const sC  = p >= 1 ? '#ffd54f' : '#6b7ab8';
        draw('S(t)=6t⁵-15t⁴+10t³', sC, false, sHL);
        drawPair('S(u)=', c.su?.toFixed(3) ?? '?', sC, 'S(v)=', c.sv?.toFixed(3) ?? '?', sC, sHL);
        sep();

        // ③ 点积（阶段2高亮）— 四角两行并排
        const dHL  = p === 2;
        const dots = c.dots ?? [0, 0, 0, 0];
        const dC   = (i) => p >= 2
            ? ['#00c853','#ffab00','#ff5252','#5c9dff'][i]
            : '#6b7ab8';
        draw('dot(梯度, 距离):', p >= 2 ? '#a5d6a7' : '#6b7ab8', false, dHL);
        drawPair('n₀₀=', dots[0].toFixed(3), dC(0), 'n₁₀=', dots[1].toFixed(3), dC(1), dHL);
        drawPair('n₀₁=', dots[2].toFixed(3), dC(2), 'n₁₁=', dots[3].toFixed(3), dC(3), dHL);
        sep();

        // ④ X轴插值（阶段3高亮）— E/F 两行并排
        const eHL = p === 3;
        const eC  = p >= 3 ? '#ffab00' : '#6b7ab8';
        const fC  = p >= 3 ? '#00e5ff' : '#6b7ab8';
        draw('X轴插值 lerp(·,·,S(u)):', p >= 3 ? '#ffab00' : '#6b7ab8', false, eHL);
        drawPair('E=', c.E?.toFixed(3) ?? '?', eC, 'F=', c.F?.toFixed(3) ?? '?', fC, eHL);
        sep();

        // ⑤ Y轴插值 → 最终值（阶段4+高亮）
        const nHL = p >= 4;
        const nC  = p >= 4 ? '#c084fc' : '#6b7ab8';
        draw('Y轴插值 lerp(E,F,S(v)):', nC, false, nHL);

        if (p >= 4) {
            const fval = c.noise?.toFixed(4) ?? '?';
            // 最终值大字高亮框
            const bh = fs * 1.8;
            ctx.save();
            ctx.fillStyle   = 'rgba(168,85,247,0.22)';
            ctx.strokeStyle = 'rgba(168,85,247,0.5)';
            ctx.lineWidth   = 1;
            this._roundRect(ctx, formulaX + 4, y - fs * 0.9, formulaW - 8, bh, 4);
            ctx.fill(); ctx.stroke();
            ctx.fillStyle = '#e8d5ff';
            ctx.font      = `bold ${fs * 1.15}px monospace`;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(`noise = ${fval}`, formulaX + formulaW / 2, y - fs * 0.9 + bh / 2);
            ctx.textBaseline = 'alphabetic';
            ctx.restore();
        }
    }

    // ====================================================
    //  绘制：待机提示文字
    // ====================================================

    _drawHint(ctx) {
        const { gx, gy, gridSize } = this.layout;
        const cx = gx + gridSize / 2;
        const cy = gy + gridSize * 0.94;
        const t  = (performance.now() - this._t0) / 1000;
        const a  = (Math.sin(t * 2) * 0.5 + 0.5) * 0.7 + 0.2;

        ctx.save();
        ctx.globalAlpha = a;
        ctx.fillStyle   = 'rgba(200,210,255,0.8)';
        ctx.font        = `${Math.max(9, gridSize * 0.034)}px sans-serif`;
        ctx.textAlign   = 'center';
        ctx.fillText('拖拽 P 点 或 点击「演示计算过程」', cx, cy);
        ctx.restore();
    }

    // ====================================================
    //  工具方法
    // ====================================================

    _roundRect(ctx, x, y, w, h, r) {
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.lineTo(x + w - r, y);
        ctx.quadraticCurveTo(x + w, y, x + w, y + r);
        ctx.lineTo(x + w, y + h - r);
        ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h);
        ctx.lineTo(x + r, y + h);
        ctx.quadraticCurveTo(x, y + h, x, y + h - r);
        ctx.lineTo(x, y + r);
        ctx.quadraticCurveTo(x, y, x + r, y);
        ctx.closePath();
    }
}

// ====================================================
//  全局 API（供 HTML onclick 调用）
// ====================================================

function animateNoiseCalculation() {
    if (window.noiseCalcViz) window.noiseCalcViz.animateCalculation();
}

function resetNoiseVisualization() {
    if (window.noiseCalcViz) window.noiseCalcViz.resetVisualization();
}

// ====================================================
//  初始化（在 DOMContentLoaded 中执行一次，无内部双重监听）
// ====================================================

document.addEventListener('DOMContentLoaded', () => {
    const canvas = document.getElementById('noiseCalculationCanvas');
    if (canvas) {
        window.noiseCalcViz = new NoiseCalculationVisualization(canvas);
    }

    // MutationObserver：只监听第三模块和第五页，不监听 subtree
    const initObserver = (selector) => {
        const el = document.querySelector(selector);
        if (!el) return;
        new MutationObserver(() => {
            const page5 = document.querySelector('.page[data-page="5"]');
            if (page5 && page5.classList.contains('active') && window.noiseCalcViz) {
                window.noiseCalcViz._fitCanvas();
                window.noiseCalcViz._recompute();
                if (!window.noiseCalcViz.isAnimating) {
                    window.noiseCalcViz._stopIdleLoop();
                    window.noiseCalcViz._startIdleLoop();
                }
            }
        }).observe(el, { attributes: true, attributeFilter: ['class'] });
    };

    initObserver('#module-3');
    initObserver('.page[data-page="5"]');
});
