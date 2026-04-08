// ==========================================
// 第五页：噪声计算可视化
// Noise Calculation Visualization
// ==========================================

class NoiseCalculationVisualization {
    constructor() {
        this.canvas = null;
        this.ctx = null;
        this.animationId = null;
        this.calculationStep = 0;
        this.speed = 5;
        this.isAnimating = false;
        
        // 坐标点定义
        this.points = {
            A: { x: 0.2, y: 0.2, value: 0.4, color: '#00c853' },
            B: { x: 0.8, y: 0.2, value: 0.6, color: '#ffab00' },
            C: { x: 0.2, y: 0.8, value: 0.3, color: '#ff5252' },
            D: { x: 0.8, y: 0.8, value: 0.7, color: '#5c9dff' }
        };
        
        this.intermediateValues = {
            x1: null,  // 沿X轴插值结果1
            x2: null,  // 沿X轴插值结果2
            final: null // 最终噪声值
        };
        
        this.initialize();
    }
    
    initialize() {
        document.addEventListener('DOMContentLoaded', () => {
            this.setupCanvas();
            this.setupEventListeners();
            this.render();
        });
    }
    
    setupCanvas() {
        this.canvas = document.getElementById('noiseCalculationCanvas');
        if (!this.canvas) {
            console.warn('Noise calculation canvas not found');
            return;
        }
        
        this.ctx = this.canvas.getContext('2d');
        
        // 响应式调整
        this.adjustCanvasSize();
        window.addEventListener('resize', () => this.adjustCanvasSize());
    }
    
    adjustCanvasSize() {
        if (!this.canvas || !this.canvas.parentElement) return;
        
        const container = this.canvas.parentElement;
        const width = container.clientWidth;
        const height = container.clientHeight;
        
        // 保持16:9比例
        const targetRatio = 16 / 9;
        const currentRatio = width / height;
        
        let newWidth, newHeight;
        if (currentRatio > targetRatio) {
            // 太宽，按高度调整
            newHeight = height;
            newWidth = height * targetRatio;
        } else {
            // 太高，按宽度调整
            newWidth = width;
            newHeight = width / targetRatio;
        }
        
        this.canvas.width = newWidth;
        this.canvas.height = newHeight;
        
        // 更新坐标比例
        this.updateCoordinateSystem();
    }
    
    updateCoordinateSystem() {
        const margin = 50;
        this.drawingArea = {
            x: margin,
            y: margin,
            width: this.canvas.width - margin * 2,
            height: this.canvas.height - margin * 2
        };
    }
    
    setupEventListeners() {
        // 演示按钮
        const demoBtn = document.querySelector('button[onclick="animateNoiseCalculation()"]');
        if (demoBtn) {
            demoBtn.onclick = () => this.animateCalculation();
        }
        
        // 重置按钮
        const resetBtn = document.querySelector('button[onclick="resetNoiseVisualization()"]');
        if (resetBtn) {
            resetBtn.onclick = () => this.resetVisualization();
        }
        
        // 速度滑块
        const speedSlider = document.getElementById('calculationSpeed');
        if (speedSlider) {
            speedSlider.addEventListener('input', (e) => {
                this.speed = parseInt(e.target.value);
            });
        }
        
        // 监听页面切换
        document.addEventListener('pageChange', (e) => {
            if (e.detail.page === 5) {
                this.onPageActivated();
            } else {
                this.onPageDeactivated();
            }
        });
    }
    
    onPageActivated() {
        // 页面激活时重新渲染
        this.adjustCanvasSize();
        this.render();
        
        // 更新步骤指示器
        this.updateStepIndicators(0);
    }
    
    onPageDeactivated() {
        // 停止动画
        if (this.animationId) {
            cancelAnimationFrame(this.animationId);
            this.animationId = null;
        }
        this.isAnimating = false;
    }
    
    // 平滑插值函数
    smoothstep(t) {
        return t * t * t * (t * (t * 6 - 15) + 10);
    }
    
    // 线性插值函数
    lerp(a, b, t) {
        return a + (b - a) * t;
    }
    
    // 计算噪声值
    calculateNoise() {
        const { A, B, C, D } = this.points;
        
        // 计算相对位置
        const u = 0.5; // 示例：中间位置
        const v = 0.5; // 示例：中间位置
        
        // 应用平滑函数
        const su = this.smoothstep(u);
        const sv = this.smoothstep(v);
        
        // 沿X轴插值
        this.intermediateValues.x1 = this.lerp(A.value, B.value, su);
        this.intermediateValues.x2 = this.lerp(C.value, D.value, su);
        
        // 沿Y轴插值得到最终值
        this.intermediateValues.final = this.lerp(this.intermediateValues.x1, this.intermediateValues.x2, sv);
        
        return this.intermediateValues.final;
    }
    
    // 绘制网格系统
    drawGrid() {
        const { x, y, width, height } = this.drawingArea;
        const ctx = this.ctx;
        
        // 清空画布
        ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
        
        // 绘制背景
        ctx.fillStyle = 'rgba(10, 13, 26, 0.8)';
        ctx.fillRect(x, y, width, height);
        
        // 绘制网格线
        const gridSize = 50;
        ctx.strokeStyle = 'rgba(92, 157, 255, 0.15)';
        ctx.lineWidth = 1;
        
        // 垂直线
        for (let i = 0; i <= width; i += gridSize) {
            ctx.beginPath();
            ctx.moveTo(x + i, y);
            ctx.lineTo(x + i, y + height);
            ctx.stroke();
        }
        
        // 水平线
        for (let i = 0; i <= height; i += gridSize) {
            ctx.beginPath();
            ctx.moveTo(x, y + i);
            ctx.lineTo(x + width, y + i);
            ctx.stroke();
        }
        
        // 绘制坐标轴
        ctx.strokeStyle = 'rgba(92, 157, 255, 0.5)';
        ctx.lineWidth = 2;
        
        // X轴
        ctx.beginPath();
        ctx.moveTo(x, y + height / 2);
        ctx.lineTo(x + width, y + height / 2);
        ctx.stroke();
        
        // Y轴
        ctx.beginPath();
        ctx.moveTo(x + width / 2, y);
        ctx.lineTo(x + width / 2, y + height);
        ctx.stroke();
        
        // 坐标轴标签
        ctx.fillStyle = 'rgba(224, 229, 255, 0.8)';
        ctx.font = '12px Arial';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        
        // X轴标签
        ctx.fillText('X', x + width + 15, y + height / 2);
        ctx.fillText('0', x, y + height / 2 + 15);
        ctx.fillText('1', x + width, y + height / 2 + 15);
        
        // Y轴标签
        ctx.fillText('Y', x + width / 2, y - 15);
        ctx.fillText('1', x + width / 2 - 15, y);
        ctx.fillText('0', x + width / 2 - 15, y + height);
    }
    
    // 绘制坐标点
    drawPoints() {
        const { x, y, width, height } = this.drawingArea;
        const ctx = this.ctx;
        
        Object.entries(this.points).forEach(([key, point]) => {
            const px = x + point.x * width;
            const py = y + point.y * height;
            
            // 绘制点
            ctx.beginPath();
            ctx.arc(px, py, 10, 0, Math.PI * 2);
            ctx.fillStyle = point.color;
            ctx.fill();
            ctx.strokeStyle = 'white';
            ctx.lineWidth = 2;
            ctx.stroke();
            
            // 绘制标签
            ctx.fillStyle = 'white';
            ctx.font = 'bold 14px Arial';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(key, px, py);
            
            // 绘制数值
            ctx.fillStyle = point.color;
            ctx.font = '12px Arial';
            ctx.fillText(point.value.toFixed(2), px, py + 20);
        });
    }
    
    // 绘制插值线
    drawInterpolationLines() {
        if (this.calculationStep < 1) return;
        
        const { x, y, width, height } = this.drawingArea;
        const ctx = this.ctx;
        const { A, B, C, D } = this.points;
        
        // 计算沿X轴插值点的位置
        const progress = Math.min(this.calculationStep / 20, 1);
        
        // 沿X轴的插值点
        const x1x = x + this.lerp(A.x, B.x, progress) * width;
        const x1y = y + A.y * height;
        const x2x = x + this.lerp(C.x, D.x, progress) * width;
        const x2y = y + C.y * height;
        
        // 绘制X轴插值线
        ctx.strokeStyle = 'rgba(255, 171, 0, 0.7)';
        ctx.lineWidth = 3;
        ctx.setLineDash([5, 5]);
        
        ctx.beginPath();
        ctx.moveTo(x + A.x * width, y + A.y * height);
        ctx.lineTo(x1x, x1y);
        ctx.stroke();
        
        ctx.beginPath();
        ctx.moveTo(x + C.x * width, y + C.y * height);
        ctx.lineTo(x2x, x2y);
        ctx.stroke();
        
        ctx.setLineDash([]);
        
        // 绘制X轴插值点
        if (this.calculationStep >= 10) {
            ctx.beginPath();
            ctx.arc(x1x, x1y, 8, 0, Math.PI * 2);
            ctx.fillStyle = 'rgba(255, 171, 0, 0.9)';
            ctx.fill();
            ctx.strokeStyle = 'white';
            ctx.lineWidth = 2;
            ctx.stroke();
            
            ctx.fillStyle = 'white';
            ctx.font = 'bold 12px Arial';
            ctx.fillText('X1', x1x, x1y - 15);
            
            ctx.beginPath();
            ctx.arc(x2x, x2y, 8, 0, Math.PI * 2);
            ctx.fill();
            ctx.fillText('X2', x2x, x2y - 15);
        }
        
        // 沿Y轴插值
        if (this.calculationStep >= 20) {
            const yProgress = Math.min((this.calculationStep - 20) / 20, 1);
            const finalX = x + this.lerp(x1x - x, x2x - x, yProgress);
            const finalY = y + this.lerp(x1y - y, x2y - y, yProgress);
            
            // 绘制Y轴插值线
            ctx.strokeStyle = 'rgba(92, 157, 255, 0.7)';
            ctx.lineWidth = 3;
            ctx.setLineDash([5, 5]);
            
            ctx.beginPath();
            ctx.moveTo(x1x, x1y);
            ctx.lineTo(finalX, finalY);
            ctx.stroke();
            
            ctx.beginPath();
            ctx.moveTo(x2x, x2y);
            ctx.lineTo(finalX, finalY);
            ctx.stroke();
            
            ctx.setLineDash([]);
            
            // 绘制最终点
            if (this.calculationStep >= 30) {
                ctx.beginPath();
                ctx.arc(finalX, finalY, 12, 0, Math.PI * 2);
                ctx.fillStyle = 'rgba(92, 157, 255, 0.9)';
                ctx.fill();
                ctx.strokeStyle = 'white';
                ctx.lineWidth = 3;
                ctx.stroke();
                
                ctx.fillStyle = 'white';
                ctx.font = 'bold 14px Arial';
                ctx.fillText('Noise', finalX, finalY - 20);
                
                // 显示最终值
                ctx.fillStyle = '#5c9dff';
                ctx.font = '12px Arial';
                const noiseValue = this.calculateNoise();
                ctx.fillText(noiseValue.toFixed(3), finalX, finalY + 25);
            }
        }
    }
    
    // 绘制信息面板
    drawInfoPanel() {
        const ctx = this.ctx;
        const x = 20;
        let y = 20;
        
        ctx.fillStyle = 'rgba(15, 20, 41, 0.8)';
        ctx.fillRect(x, y, 200, 120);
        ctx.strokeStyle = 'rgba(92, 157, 255, 0.5)';
        ctx.lineWidth = 1;
        ctx.strokeRect(x, y, 200, 120);
        
        ctx.fillStyle = 'white';
        ctx.font = 'bold 14px Arial';
        ctx.textAlign = 'left';
        ctx.fillText('噪声计算信息', x + 10, y + 20);
        
        ctx.fillStyle = '#e0e7ff';
        ctx.font = '12px Arial';
        
        if (this.calculationStep < 10) {
            ctx.fillText('步骤 1: 获取角点值', x + 10, y + 45);
        } else if (this.calculationStep < 20) {
            ctx.fillText('步骤 2: 沿X轴插值', x + 10, y + 45);
            if (this.intermediateValues.x1 !== null) {
                ctx.fillText(`X1 = ${this.intermediateValues.x1.toFixed(3)}`, x + 10, y + 65);
                ctx.fillText(`X2 = ${this.intermediateValues.x2.toFixed(3)}`, x + 10, y + 85);
            }
        } else if (this.calculationStep < 30) {
            ctx.fillText('步骤 3: 沿Y轴插值', x + 10, y + 45);
        } else {
            ctx.fillText('步骤 4: 计算完成', x + 10, y + 45);
            ctx.fillStyle = '#5c9dff';
            ctx.font = 'bold 14px Arial';
            ctx.fillText(`最终噪声值: ${this.intermediateValues.final.toFixed(3)}`, x + 10, y + 70);
        }
        
        // 显示进度
        ctx.fillStyle = '#b8c2ff';
        ctx.font = '11px Arial';
        ctx.fillText(`进度: ${Math.min(this.calculationStep, 40)}/40`, x + 10, y + 105);
    }
    
    // 渲染主函数
    render() {
        if (!this.ctx || !this.drawingArea) return;
        
        this.drawGrid();
        this.drawPoints();
        this.drawInterpolationLines();
        this.drawInfoPanel();
        
        if (this.isAnimating) {
            this.animationId = requestAnimationFrame(() => this.animateFrame());
        }
    }
    
    // 动画帧
    animateFrame() {
        if (this.calculationStep >= 40) {
            this.isAnimating = false;
            this.updateStepIndicators(3); // 完成所有步骤
            return;
        }
        
        this.calculationStep += this.speed * 0.05;
        this.updateStepIndicators(Math.floor(this.calculationStep / 10));
        this.render();
    }
    
    // 更新步骤指示器
    updateStepIndicators(activeStep) {
        const steps = document.querySelectorAll('.info-step');
        steps.forEach((step, index) => {
            if (index === activeStep) {
                step.classList.add('active');
            } else {
                step.classList.remove('active');
            }
        });
    }
    
    // 开始动画演示
    animateCalculation() {
        if (this.isAnimating) return;
        
        this.isAnimating = true;
        this.calculationStep = 0;
        this.intermediateValues = { x1: null, x2: null, final: null };
        
        // 重置步骤指示器
        this.updateStepIndicators(0);
        
        // 开始动画
        this.animateFrame();
    }
    
    // 重置可视化
    resetVisualization() {
        this.isAnimating = false;
        this.calculationStep = 0;
        this.intermediateValues = { x1: null, x2: null, final: null };
        
        if (this.animationId) {
            cancelAnimationFrame(this.animationId);
            this.animationId = null;
        }
        
        // 重置步骤指示器
        this.updateStepIndicators(0);
        
        // 重新渲染
        this.render();
    }
    
    // 获取随机点值
    randomizePoints() {
        Object.values(this.points).forEach(point => {
            point.value = Math.random() * 0.8 + 0.1; // 0.1-0.9之间的随机值
        });
        
        if (!this.isAnimating) {
            this.render();
        }
    }
}

// 全局函数供HTML调用
function animateNoiseCalculation() {
    if (window.noiseCalcViz) {
        window.noiseCalcViz.animateCalculation();
    }
}

function resetNoiseVisualization() {
    if (window.noiseCalcViz) {
        window.noiseCalcViz.resetVisualization();
    }
}

// 初始化
document.addEventListener('DOMContentLoaded', () => {
    window.noiseCalcViz = new NoiseCalculationVisualization();
    
    // 监听页面切换
    const observer = new MutationObserver(() => {
        const page5 = document.querySelector('.page[data-page="5"]');
        if (page5 && page5.classList.contains('active')) {
            if (window.noiseCalcViz) {
                window.noiseCalcViz.onPageActivated();
            }
        }
    });
    
    const container = document.querySelector('.module-content');
    if (container) {
        observer.observe(container, { 
            attributes: true, 
            attributeFilter: ['class'],
            subtree: true 
        });
    }
});