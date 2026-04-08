// ==========================================
// 语言切换功能 - 完整版
// Language Switcher Functionality - Full Version
// ==========================================

class LanguageSwitcher {
    constructor() {
        this.currentLanguage = 'zh'; // 默认中文
        this.translations = {};
        this.init();
    }

    init() {
        this.loadTranslations();
        this.bindEvents();
        this.loadSavedLanguage();
    }

    // 加载翻译数据
    loadTranslations() {
        this.translations = {
            zh: {
                // 导航栏
                'nav.module1': '问题引入',
                'nav.module2': '肯·柏林',
                'nav.module3': '算法原理',
                'nav.module4': '参数演示',
                'nav.module5': '对比分析',
                'nav.module6': '总结',
                'nav.progress': '进度',
                
                // 折叠标题
                'collapsed.title': '柏林噪声算法',
                
                // 模块 1：问题引入
                'module1.title': '第一部分：从问题开始',
                'module1.description': '为什么需要柏林噪声？从实际问题出发，理解算法的诞生背景',
                'module1.page1.title': '🎮 游戏开发者的挑战',
                'module1.page1.problem': '想象你正在开发一款开放世界游戏，需要生成<span class="highlight">无限的自然地形</span>。',
                'module1.page1.formula': '传统方法：每个点独立随机生成高度',
                'module1.viz.regenerate': '重新生成',
                'module1.viz.whiteNoise': '白噪声：每个点完全独立，地形看起来像"像素化"的噪声',
                'module1.page2.title': '❌ 传统随机算法的三大问题',
                'module1.page2.problem1': '缺乏连续性',
                'module1.page2.problem1.desc': '相邻点数值跳跃剧烈，无规律可言',
                'module1.page2.problem2': '不够自然',
                'module1.page2.problem2.desc': '像素化块状，缺乏真实地形的平滑感',
                'module1.page2.problem3': '难以控制',
                'module1.page2.problem3.desc': '无法定向生成山脉、平原等地貌特征',
                'module1.insight.title': '核心问题',
                'module1.insight.content': '如何在<span class="highlight">随机性</span>和<span class="highlight">自然性</span>之间找到平衡？需要一种<strong>伪随机但连续平滑</strong>的算法。',
                'module1.page3.title': '🏔️ 我们需要什么样的地形？',
                'module1.page3.ideal': '理想的地形：随机且平滑，自然真实',
                'module1.page3.action': '了解解决方案 →',
                'module1.page1.navTitle': '问题背景',
                'module1.page2.navTitle': '问题分析',
                'module1.page3.navTitle': '解决方案',
                
                // 模块 2：肯·柏林
                'module2.title': '第二部分：算法发明者',
                'module2.description': '了解 Ken Perlin 的背景和算法发明的历史故事',
                'module2.page1.navTitle': '人物简介',
                'module2.page2.navTitle': '发明故事',
                'module2.page1.timeline': '重要事件时间轴',
                'module2.page1.event1.year': '1983',
                'module2.page1.event1.title': '算法诞生',
                'module2.page1.event1.desc': '为《Tron》开发特效时发明柏林噪声',
                'module2.page1.event2.year': '1985',
                'module2.page1.event2.title': '论文发表',
                'module2.page1.event2.desc': 'SIGGRAPH 发表经典论文，算法广为人知',
                'module2.page1.event3.year': '1997',
                'module2.page1.event3.title': '奥斯卡奖',
                'module2.page1.event3.desc': '获奥斯卡技术成就奖',
                'module2.page1.event4.year': '2002',
                'module2.page1.event4.title': 'Simplex 噪声',
                'module2.page1.event4.desc': '改进算法，解决高维性能问题',
                'module2.page1.fact': '算法命名：以发明者姓氏命名，"Perlin + Noise"',
                'module2.page1.profile': '肯·柏林 (Ken Perlin)',
                'module2.page1.meta1': '美国计算机科学家，纽约大学教授',
                'module2.page1.meta2': '1983 年发明柏林噪声 | 1997 年获奥斯卡技术奖',
                'module2.page2.story1.title': '发明背景',
                'module2.page2.story1.cardTitle': '《Tron》电影的挑战',
                'module2.page2.story1.cardContent': '1982 年，迪士尼电影《Tron》开创性地使用计算机生成图像。但 Ken Perlin 发现，计算机生成的纹理看起来<span class="highlight">太过人工</span>，缺乏自然界的有机感。',
                'module2.page2.story2.title': '如何被想到的？',
                'module2.page2.story2.card1Title': '从自然中寻找灵感',
                'module2.page2.story2.card1Content': 'Perlin 观察自然现象：山脉的起伏、云朵的流动、大理石的纹理...它们都有共同特点：<span class="highlight">随机但连续</span>。他想：能否用数学模拟这种特性？',
                'module2.page2.story2.card2Title': '核心突破',
                'module2.page2.story2.card2Content': '将空间划分为网格，为每个顶点分配随机梯度，通过<span class="highlight">平滑插值</span>连接它们。这种方法产生的噪声既随机又连续，完美模拟自然！',
                'module2.page2.story3.title': '带给我们的启发',
                'module2.page2.story3.inspiration1': '观察自然，从中学习',
                'module2.page2.story3.inspiration2': '在矛盾中寻找平衡（随机 vs 连续）',
                'module2.page2.story3.inspiration3': '简单规则创造复杂美感',
                'module2.page2.story3.inspiration4': '跨学科思维的重要价值',
                
                // 通用 UI
                'ui.reload': '重新生成',
                'ui.play': '演示计算过程',
                'ui.reset': '重置',
                'ui.random': '随机梯度',
                'ui.heatmap': '全图噪声值',
                'ui.speed': '速度',
                'ui.prev': '上一模块',
                'ui.next': '下一模块',
                'ui.page': '页',
                'ui.reference': '查看参考效果',
                
                // 按钮文本
                'btn.randomSeed': '随机种子',
                'btn.arrows': '箭头',
                'btn.particles': '粒子',
                'btn.trail': '拖尾',
                'btn.noiseMap': '噪声图',
                'btn.terrain': '地形',
                'btn.flowField': '流场',
                'btn.prevModule': '上一模块',
                'btn.nextModule': '下一模块',
                
                // Canvas 可视化文字
                'canvas.whiteNoise': '白噪声地形：太随机，不平滑',
                'canvas.desiredTerrain': '柏林噪声地形：随机且平滑，自然真实',
                'canvas.noiseTypes.whiteNoise': '白噪声',
                'canvas.appScenarios.game': '游戏开发 - 地形生成',
                
                // Module 3: 算法原理
                'module3.title': '第三部分：算法原理',
                'module3.description': '学习柏林噪声算法的 4 个核心步骤',
                'module3.page1.navTitle': '网格',
                'module3.page2.navTitle': '梯度',
                'module3.page3.navTitle': '点乘',
                'module3.page4.navTitle': '平滑',
                'module3.page5.navTitle': '计算',
                'module3.page6.navTitle': '总结',
                'module3.page1.title': '第 1 步：网格划分',
                'module3.page1.desc': '将空间划分为规则网格，每个网格顶点有固定的整数坐标。',
                'module3.page1.formula': 'gridX = ⌊x⌋, gridY = ⌊y⌋',
                'module3.page1.formulaDesc': '向下取整得到网格坐标',
                'module3.page2.title': '第 2 步：梯度向量',
                'module3.page2.desc': '为每个网格顶点分配一个随机梯度向量（单位向量）。',
                'module3.page2.formula': 'g = (cos(θ), sin(θ)), θ ∈ [0, 2π]',
                'module3.page2.formulaDesc': '随机角度生成单位向量',
                'module3.page2.regenerate': '重新生成',
                'module3.page3.title': '第 3 步：点乘运算',
                'module3.page3.desc': '计算每个顶点的梯度向量与到该顶点距离向量的点乘。',
                'module3.page3.formula': 'dot = g · d = gₓ × dₓ + gᵧ × dᵧ',
                'module3.page3.formulaDesc': '点乘结果表示梯度对当前点的影响',
                'module3.page3.viewAll': '查看全图点积',
                'module3.page3.mouseHint': '移动鼠标查看实时的点乘计算结果',
                'module3.page4.title': '第 4 步：平滑处理',
                'module3.page4.desc': '使用平滑的插值函数（5 阶多项式）对 4 个角点的点乘结果进行插值，获得平滑连续的噪声值。',
                'module3.page4.formula': 'smooth(t) = 6t⁵ - 15t⁴ + 10t³',
                'module3.page4.formulaDesc': 't 表示单元格内的相对位置（0 到 1 之间），平滑函数使插值更自然',
                'module3.page4.keyPoint': '关键性质',
                'module3.page4.keyPointDesc': 's(0)=0, s(1)=1, s\'(0)=s\'(1)=0。导数为 0 确保相邻网格切线连续，视觉上无缝衔接！',
                'module3.page4.linearInterp': '线性插值',
                'module3.page4.linearInterpDesc': 'lerp(a, b, t) = a + (b - a) × t',
                'module3.page4.linearInterpDetail': '通过两次线性插值，先沿 X 轴，再沿 Y 轴',
                'module3.page4.cornerA': '角点 A:',
                'module3.page4.cornerB': '角点 B:',
                'module3.page4.cornerC': '角点 C:',
                'module3.page4.cornerD': '角点 D:',
                // Module 3 Page 5: Noise Calculation
                'module3.page5.title': '第 5 步：噪声计算',
                'module3.page5.desc': '综合所有步骤，计算最终的柏林噪声值。通过双线性插值结合平滑函数，得到连续自然的噪声值。',
                'module3.page5.formulaTitle': '完整噪声计算公式',
                'module3.page5.formulaDesc': '先沿 X 轴插值，再沿 Y 轴插值',
                'module3.page5.relativePos': '相对位置',
                'module3.page5.relativePosDesc': '单元格内相对位置',
                'module3.page5.smoothFunc': '平滑插值函数',
                'module3.page5.smoothFuncDesc': '五次多项式平滑',
                'module3.page5.lerpFunc': '线性插值函数',
                'module3.page5.lerpFuncDesc': '线性加权平均',
                'module3.page5.vizTitle': '噪声计算流程可视化',
                'module3.page5.gradient': '梯度向量',
                'module3.page5.xInterp': 'X 轴插值',
                'module3.page5.yInterp': 'Y 轴插值',
                'module3.page5.playBtn': '演示计算过程',
                'module3.page5.step1': '展示梯度向量 & 距离向量',
                'module3.page5.step2': 'X 轴插值：E 点和 F 点',
                'module3.page5.step3': 'Y 轴插值 → 最终噪声值',
                
                // Module 3 Page 6: Summary
                'module3.page6.title': '算法总结',
                'module3.page6.properties': '算法特性',
                'module3.page6.property1': '连续性',
                'module3.page6.property1.desc': '相邻点之间平滑过渡',
                'module3.page6.property2': '随机性',
                'module3.page6.property2.desc': '随机梯度产生多样化的噪声',
                'module3.page6.property3': '可重复性',
                'module3.page6.property3.desc': '相同坐标产生相同结果',
                'module3.page6.property4': '可控性',
                'module3.page6.property4.desc': '通过参数控制噪声特性',
                'module3.page6.core': '核心思想',
                'module3.page6.core.desc': '柏林噪声在<span class="highlight">伪随机梯度</span>和<span class="highlight">平滑插值</span>之间找到了完美平衡。',
                
                // Module 4: 参数控制与演示
                'module4.title': '第四部分：参数控制与演示',
                'module4.description': '通过参数调节，实时观察柏林噪声的效果变化',
                'module4.page1.navTitle': '参数控制',
                'module4.page1.noiseParams': '噪声参数',
                'module4.page1.scale': '缩放',
                'module4.page1.octaves': '叠加层数',
                'module4.page1.persistence': '持续度',
                'module4.page1.lacunarity': '空隙度',
                'module4.page1.actions': '操作',
                'module4.page1.flowField': '流场控制',
                'module4.page1.particleSettings': '粒子设置',
                'module4.page1.particleCount': '粒子数量',
                'module4.page1.livePreview': '实时预览',
                'module4.page1.currentEffect': '当前效果：<span id="currentPreset">噪声图</span>',
                
                // Module 4 帮助弹窗
                'module4.help.scale.title': '缩放 (Scale)',
                'module4.help.scale.desc': '控制噪声采样的频率，决定生成图案的缩放级别。',
                'module4.help.scale.detail1': '值越小：图案被放大，显示更大范围的地形特征',
                'module4.help.scale.detail2': '值越大：图案被缩小，显示更多细节和变化',
                'module4.help.scale.detail3': '类似于地图的比例尺，控制观察的尺度',
                'module4.help.scale.formula': '实际坐标 = 屏幕坐标 × scale',
                'module4.help.scale.note': '提示：较小的值（如 0.001-0.01）适合生成大陆级别的地形，较大的值（如 0.05-0.1）适合生成细节纹理。',
                
                'module4.help.octaves.title': '叠加层数 (Octaves)',
                'module4.help.octaves.desc': '控制分形布朗运动中叠加的噪声层数，影响图案的细节程度。',
                'module4.help.octaves.detail1': '每一层称为一个"八度"，频率是前一层的两倍',
                'module4.help.octaves.detail2': '层数越多：图案细节越丰富，但计算量越大',
                'module4.help.octaves.detail3': '层数越少：图案越平滑简单，计算速度越快',
                'module4.help.octaves.formula': 'fbm(x) = Σ noise(x × 2ⁱ) × persistenceⁱ',
                'module4.help.octaves.note': '提示：4-6 层是常用范围，能在视觉效果和性能之间取得良好平衡。',
                
                'module4.help.persistence.title': '持续度 (Persistence)',
                'module4.help.persistence.desc': '控制每层噪声的振幅衰减速度，影响图案的粗糙程度。',
                'module4.help.persistence.detail1': '值越小：高频层振幅快速衰减，图案更平滑',
                'module4.help.persistence.detail2': '值越大：高频层保持较大振幅，图案更粗糙',
                'module4.help.persistence.detail3': '决定地形特征的主导频率范围',
                'module4.help.persistence.formula': 'amplitudeᵢ = amplitude₀ × persistenceⁱ',
                'module4.help.persistence.note': '提示：0.5 是常用值，小于 0.3 会产生非常平滑的效果，大于 0.7 会产生非常粗糙的效果。',
                
                'module4.help.lacunarity.title': '空隙度 (Lacunarity)',
                'module4.help.lacunarity.desc': '控制每层噪声的频率增长速度，影响图案的间隙和密度。',
                'module4.help.lacunarity.detail1': '值越小：频率增长慢，图案更连续',
                'module4.help.lacunarity.detail2': '值越大：频率增长快，图案更多间隙和断裂',
                'module4.help.lacunarity.detail3': '决定分形结构的"空隙"程度',
                'module4.help.lacunarity.formula': 'frequencyᵢ = frequency₀ × lacunarityⁱ',
                'module4.help.lacunarity.note': '提示：2.0 是标准值（每层频率翻倍），小于 1.5 会产生非常连续的图案，大于 2.5 会产生非常破碎的效果。',
                
                // 帮助弹窗通用文本
                'module4.help.details': '详细说明',
                'module4.help.formula': '公式',
                'module4.help.note': '提示',
            },
            en: {
                // Navigation
                'nav.module1': 'Problem',
                'nav.module2': 'Ken Perlin',
                'nav.module3': 'How It Works',
                'nav.module4': 'Try It',
                'nav.module5': 'Compare',
                'nav.module6': 'Summary',
                'nav.progress': 'Progress',
                
                // Collapsed Title
                'collapsed.title': 'Perlin Noise',
                
                // Module 1: Problem Introduction
                'module1.title': 'Part 1: Start with a Problem',
                'module1.description': 'Why do we need Perlin Noise? Learn from a real problem.',
                'module1.page1.title': '🎮 Game Maker Problem',
                'module1.page1.problem': 'Imagine you make an open-world game. You need to create <span class="highlight">endless natural terrain</span>.',
                'module1.page1.formula': 'Old way: Each point gets random height',
                'module1.viz.regenerate': 'Try Again',
                'module1.viz.whiteNoise': 'White noise: Each point is different. Looks like pixel blocks.',
                'module1.page2.title': '❌ Three Big Problems',
                'module1.page2.problem1': 'Not Continuous',
                'module1.page2.problem1.desc': 'Next point jumps wildly. No pattern.',
                'module1.page2.problem2': 'Not Natural',
                'module1.page2.problem2.desc': 'Looks like blocks. Not smooth like real land.',
                'module1.page2.problem3': 'Hard to Control',
                'module1.page2.problem3.desc': 'Cannot make mountains or plains on purpose.',
                'module1.insight.title': 'The Big Question',
                'module1.insight.content': 'How to balance <span class="highlight">random</span> and <span class="highlight">natural</span>? We need math that is <strong>random but smooth</strong>.',
                'module1.page3.title': '🏔️ What Terrain Do We Want?',
                'module1.page3.ideal': 'Good terrain: Random and smooth. Looks real.',
                'module1.page3.action': 'See the Solution →',
                'module1.page1.navTitle': 'Problem',
                'module1.page2.navTitle': 'Analysis',
                'module1.page3.navTitle': 'Solution',
                
                // Module 2: Ken Perlin
                'module2.title': 'Part 2: The Inventor',
                'module2.description': 'Meet Ken Perlin and learn the invention story.',
                'module2.page1.navTitle': 'Profile',
                'module2.page2.navTitle': 'Story',
                'module2.page1.timeline': 'Timeline',
                'module2.page1.event1.year': '1983',
                'module2.page1.event1.title': 'Born',
                'module2.page1.event1.desc': 'Invented Perlin Noise for movie "Tron"',
                'module2.page1.event2.year': '1985',
                'module2.page1.event2.title': 'Paper',
                'module2.page1.event2.desc': 'Published at SIGGRAPH. Became famous.',
                'module2.page1.event3.year': '1997',
                'module2.page1.event3.title': 'Oscar Prize',
                'module2.page1.event3.desc': 'Won Academy Award for Technical Achievement',
                'module2.page1.event4.year': '2002',
                'module2.page1.event4.title': 'Simplex Noise',
                'module2.page1.event4.desc': 'Made better version. Faster for 3D.',
                'module2.page1.fact': 'Name: "Perlin" (inventor) + "Noise" (math)',
                'module2.page1.profile': 'Ken Perlin',
                'module2.page1.meta1': 'Computer scientist. NYU professor.',
                'module2.page1.meta2': 'Invented Perlin Noise in 1983 | Won Oscar in 1997',
                'module2.page2.story1.title': 'Background Story',
                'module2.page2.story1.cardTitle': 'The "Tron" Movie Problem',
                'module2.page2.story1.cardContent': 'In 1982, Disney made "Tron" with computer graphics. But Ken Perlin saw a problem: computer pictures looked <span class="highlight">too fake</span>. They lacked nature feel.',
                'module2.page2.story2.title': 'How He Got the Idea',
                'module2.page2.story2.card1Title': 'Learn from Nature',
                'module2.page2.story2.card1Content': 'Perlin watched nature: mountains, clouds, marble stone... They shared one thing: <span class="highlight">random but continuous</span>. He asked: Can math do this?',
                'module2.page2.story2.card2Title': 'Big Breakthrough',
                'module2.page2.story2.card2Content': 'Divide space into grid. Give random direction to each corner. Use <span class="highlight">smooth math</span> to connect them. Result: random but continuous. Perfect for nature!',
                'module2.page2.story3.title': 'Lessons for Us',
                'module2.page2.story3.inspiration1': 'Watch nature. Learn from it.',
                'module2.page2.story3.inspiration2': 'Find balance (random vs smooth)',
                'module2.page2.story3.inspiration3': 'Simple rules make beautiful things',
                'module2.page2.story3.inspiration4': 'Mix different subjects',
                
                // Module 2 Page 2 - Invention Story Details
                'module2.page2.story1.icon': '🎬',
                'module2.page2.story2.icon1': '🏔️',
                'module2.page2.story2.icon2': '⚙️',
                'module2.page2.story3.icon1': '👁️',
                'module2.page2.story3.icon2': '⚖️',
                'module2.page2.story3.icon3': '🛠️',
                'module2.page2.story3.icon4': '🚀',
                
                // Module 3: How Algorithm Works
                'module3.title': 'Part 3: How It Works',
                'module3.description': 'Learn the 4 core steps of Perlin Noise algorithm',
                'module3.page1.navTitle': 'Grid',
                'module3.page2.navTitle': 'Gradient',
                'module3.page3.navTitle': 'Dot Product',
                'module3.page4.navTitle': 'Smooth',
                'module3.page5.navTitle': 'Calculate',
                'module3.page6.navTitle': 'Summary',
                'module3.page1.title': 'Step 1: Grid Setup',
                'module3.page1.desc': 'Divide space into regular grid. Each grid point has integer coordinates.',
                'module3.page1.formula': 'gridX = ⌊x⌋, gridY = ⌊y⌋',
                'module3.page1.formulaDesc': 'Floor function to get grid coordinates',
                'module3.page2.title': 'Step 2: Gradient Vectors',
                'module3.page2.desc': 'Give each grid point a random gradient vector (unit vector).',
                'module3.page2.formula': 'g = (cos(θ), sin(θ)), θ ∈ [0, 2π]',
                'module3.page2.formulaDesc': 'Random angle creates unit vector',
                'module3.page2.regenerate': 'Regenerate',
                'module3.page3.title': 'Step 3: Dot Product',
                'module3.page3.desc': 'Calculate dot product of gradient vector and distance vector.',
                'module3.page3.formula': 'dot = g · d = gₓ × dₓ + gᵧ × dᵧ',
                'module3.page3.formulaDesc': 'Dot product shows gradient influence',
                'module3.page3.viewAll': 'View Full Dot Product',
                'module3.page3.mouseHint': 'Move mouse to see real-time dot product',
                'module3.page4.title': 'Step 4: Smooth Interpolation',
                'module3.page4.desc': 'Use smooth function (5th order polynomial) to interpolate the 4 corner dot products.',
                'module3.page4.formula': 'smooth(t) = 6t⁵ - 15t⁴ + 10t³',
                'module3.page4.formulaDesc': 't is relative position (0 to 1). Smooth function makes interpolation natural.',
                'module3.page4.keyPoint': 'Key Property',
                'module3.page4.keyPointDesc': 's(0)=0, s(1)=1, s\'(0)=s\'(1)=0. Zero derivative ensures smooth connection!',
                'module3.page4.linearInterp': 'Linear Interpolation',
                'module3.page4.linearInterpDesc': 'lerp(a, b, t) = a + (b - a) × t',
                'module3.page4.linearInterpDetail': 'Two linear interpolations: first X axis, then Y axis',
                'module3.page4.cornerA': 'Corner A:',
                'module3.page4.cornerB': 'Corner B:',
                'module3.page4.cornerC': 'Corner C:',
                'module3.page4.cornerD': 'Corner D:',
                // Module 3 Page 5: Noise Calculation
                'module3.page5.title': 'Step 5: Noise Calculation',
                'module3.page5.desc': 'Combine all steps. Calculate final Perlin Noise value with bilinear interpolation.',
                'module3.page5.formulaTitle': 'Full Noise Formula',
                'module3.page5.formulaDesc': 'First X axis, then Y axis',
                'module3.page5.relativePos': 'Relative Position',
                'module3.page5.relativePosDesc': 'Position inside cell',
                'module3.page5.smoothFunc': 'Smooth Function',
                'module3.page5.smoothFuncDesc': '5th order polynomial',
                'module3.page5.lerpFunc': 'Linear Interpolation',
                'module3.page5.lerpFuncDesc': 'Linear weighted average',
                'module3.page5.vizTitle': 'Noise Calculation Flow',
                'module3.page5.gradient': 'Gradient Vector',
                'module3.page5.xInterp': 'X Interpolation',
                'module3.page5.yInterp': 'Y Interpolation',
                'module3.page5.playBtn': 'Play Demo',
                'module3.page5.step1': 'Show gradient vectors & distance vectors',
                'module3.page5.step2': 'X interpolation: Points E and F',
                'module3.page5.step3': 'Y interpolation → Final noise value',
                
                // Module 3 Page 6: Summary
                'module3.page6.title': 'Algorithm Summary',
                'module3.page6.properties': 'Properties',
                'module3.page6.property1': 'Continuity',
                'module3.page6.property1.desc': 'Smooth transition between adjacent points',
                'module3.page6.property2': 'Randomness',
                'module3.page6.property2.desc': 'Random gradients create diverse noise',
                'module3.page6.property3': 'Repeatability',
                'module3.page6.property3.desc': 'Same coordinates produce same result',
                'module3.page6.property4': 'Controllability',
                'module3.page6.property4.desc': 'Control noise with parameters',
                'module3.page6.core': 'Core Idea',
                'module3.page6.core.desc': 'Perlin Noise finds perfect balance between <span class="highlight">pseudo-random gradients</span> and <span class="highlight">smooth interpolation</span>.',
                
                // Module 4: Try It
                'module4.title': 'Part 4: Try It Yourself',
                'module4.description': 'Adjust parameters and see Perlin Noise effects in real-time',
                'module4.page1.navTitle': 'Controls',
                'module4.page1.noiseParams': 'Noise Parameters',
                'module4.page1.scale': 'Scale',
                'module4.page1.octaves': 'Octaves',
                'module4.page1.persistence': 'Persistence',
                'module4.page1.lacunarity': 'Lacunarity',
                'module4.page1.actions': 'Actions',
                'module4.page1.flowField': 'Flow Field Controls',
                'module4.page1.particleSettings': 'Particle Settings',
                'module4.page1.particleCount': 'Particle Count',
                'module4.page1.livePreview': 'Live Preview',
                'module4.page1.currentEffect': 'Current Effect: <span id="currentPreset">Noise Map</span>',
                
                // Module 4 Help Modal
                'module4.help.scale.title': 'Scale',
                'module4.help.scale.desc': 'Controls the frequency of noise sampling, determining the zoom level of the generated pattern.',
                'module4.help.scale.detail1': 'Smaller value: Pattern is zoomed in, showing larger terrain features',
                'module4.help.scale.detail2': 'Larger value: Pattern is zoomed out, showing more details and variations',
                'module4.help.scale.detail3': 'Similar to map scale, controls the observation scale',
                'module4.help.scale.formula': 'Actual Coordinate = Screen Coordinate × scale',
                'module4.help.scale.note': 'Tip: Smaller values (0.001-0.01) are suitable for continent-level terrain, larger values (0.05-0.1) for detail textures.',
                
                'module4.help.octaves.title': 'Octaves',
                'module4.help.octaves.desc': 'Controls the number of noise layers in fractal brownian motion, affecting pattern detail.',
                'module4.help.octaves.detail1': 'Each layer is called an "octave", with double the frequency of the previous',
                'module4.help.octaves.detail2': 'More octaves: Richer detail, but higher computation cost',
                'module4.help.octaves.detail3': 'Fewer octaves: Smoother and simpler pattern, faster computation',
                'module4.help.octaves.formula': 'fbm(x) = Σ noise(x × 2ⁱ) × persistenceⁱ',
                'module4.help.octaves.note': 'Tip: 4-6 octaves is a common range, providing good balance between visual quality and performance.',
                
                'module4.help.persistence.title': 'Persistence',
                'module4.help.persistence.desc': 'Controls the amplitude decay rate of each noise layer, affecting pattern roughness.',
                'module4.help.persistence.detail1': 'Smaller value: High-frequency layers decay quickly, smoother pattern',
                'module4.help.persistence.detail2': 'Larger value: High-frequency layers maintain amplitude, rougher pattern',
                'module4.help.persistence.detail3': 'Determines the dominant frequency range of terrain features',
                'module4.help.persistence.formula': 'amplitudeᵢ = amplitude₀ × persistenceⁱ',
                'module4.help.persistence.note': 'Tip: 0.5 is a common value, less than 0.3 produces very smooth effects, greater than 0.7 produces very rough effects.',
                
                'module4.help.lacunarity.title': 'Lacunarity',
                'module4.help.lacunarity.desc': 'Controls the frequency growth rate of each noise layer, affecting pattern gaps and density.',
                'module4.help.lacunarity.detail1': 'Smaller value: Slow frequency growth, more continuous pattern',
                'module4.help.lacunarity.detail2': 'Larger value: Fast frequency growth, more gaps and breaks in pattern',
                'module4.help.lacunarity.detail3': 'Determines the "gap" degree of fractal structure',
                'module4.help.lacunarity.formula': 'frequencyᵢ = frequency₀ × lacunarityⁱ',
                'module4.help.lacunarity.note': 'Tip: 2.0 is the standard value (frequency doubles each layer), less than 1.5 produces very continuous patterns, greater than 2.5 produces very fragmented effects.',
                
                // Help Modal Common Text
                'module4.help.details': 'Details',
                'module4.help.formula': 'Formula',
                'module4.help.note': 'Tip',
                
                // Module 5: Compare
                'module5.title': 'Part 5: Compare',
                'module5.description': 'Compare different noise algorithms and their uses',
                'module5.page1.navTitle': 'Types',
                'module5.page2.navTitle': 'vs White Noise',
                'module5.page3.navTitle': 'vs Simplex',
                'module5.page4.navTitle': 'Applications',
                'module5.page1.title': 'Noise Algorithm Types',
                'module5.page1.whiteNoise': 'White Noise',
                'module5.page1.whiteNoiseDesc': 'Completely random, no correlation',
                'module5.page1.perlinNoise': 'Perlin Noise',
                'module5.page1.perlinNoiseDesc': 'Pseudo-random, smooth and continuous',
                'module5.page1.simplexNoise': 'Simplex Noise',
                'module5.page1.simplexNoiseDesc': 'Improved Perlin Noise',
                'module5.page1.fractalNoise': 'Fractal Noise',
                'module5.page1.fractalNoiseDesc': 'Multiple noise layers',
                'module5.page1.vizTitle': 'Noise Types Comparison',
                'module5.page2.title': 'Perlin Noise vs White Noise',
                'module5.page3.title': 'Perlin Noise vs Simplex Noise',
                'module5.page4.title': 'Application Scenarios',
                
                // Module 6: Summary
                'module6.title': 'Part 6: Summary',
                'module6.description': 'Review key concepts and explore applications',
                'module6.page1.navTitle': 'Review',
                'module6.page2.navTitle': 'End',
                'module6.page1.title': 'Key Concepts Review',
                'module6.page1.coreAlgo': 'Algorithm Core',
                'module6.page1.coreAlgoDesc': 'Balance <span class="highlight">random gradients</span> and <span class="highlight">smooth interpolation</span>',
                'module6.page1.steps': 'Key Steps',
                'module6.page1.stepsDesc': 'Grid → Gradient → Dot Product → Smooth → Calculate',
                'module6.page1.idea': 'Core Idea',
                'module6.page1.ideaDesc': 'Local calculation, global continuity. Smooth function makes natural transition.',
                'module6.page1.value': 'Algorithm Value',
                'module6.page1.valueDesc': 'Perlin Noise is a model of <span class="highlight">learning from nature</span>. Simple rules create complex beauty.',
                'module6.page2.thanks': 'Thank You!',
                'module6.page2.subtitle': 'You now understand Perlin Noise',
                'module6.page2.explore': 'Keep Exploring',
                'module6.page2.resources': 'Learning Resources',
                
                // Generic UI
                'ui.reload': 'Reload',
                'ui.play': 'Play',
                'ui.reset': 'Reset',
                'ui.random': 'Random',
                'ui.heatmap': 'Noise Map',
                'ui.speed': 'Speed',
                'ui.prev': 'Previous',
                'ui.next': 'Next',
                'ui.page': 'Page',
                'ui.reference': 'See Reference',
                
                // Button Text
                'btn.randomSeed': 'Random Seed',
                'btn.arrows': 'Arrows',
                'btn.particles': 'Particles',
                'btn.trail': 'Trail',
                'btn.noiseMap': 'Noise Map',
                'btn.terrain': 'Terrain',
                'btn.flowField': 'Flow Field',
                'btn.prevModule': 'Previous',
                'btn.nextModule': 'Next',
                
                // Canvas Visualization Text
                'canvas.whiteNoise': 'White noise terrain: Too random, not smooth',
                'canvas.desiredTerrain': 'Perlin noise terrain: Random and smooth, looks natural',
                'canvas.noiseTypes.whiteNoise': 'White Noise',
                'canvas.appScenarios.game': 'Game Dev - Terrain Generation'
            }
        };
    }

    // 绑定事件
    bindEvents() {
        const languageToggleBtn = document.getElementById('languageToggleBtn');
        if (languageToggleBtn) {
            languageToggleBtn.addEventListener('click', () => this.toggleLanguage());
        }
        
        // 添加键盘快捷键（L 键切换语言）
        document.addEventListener('keydown', (e) => {
            // 检查是否按下了 L 键，且不在输入框中
            if (e.key === 'l' || e.key === 'L') {
                // 如果焦点不在输入框或文本域中，则切换语言
                if (e.target.tagName !== 'INPUT' && e.target.tagName !== 'TEXTAREA') {
                    e.preventDefault();
                    this.toggleLanguage();
                }
            }
        });
    }

    // 加载保存的语言
    loadSavedLanguage() {
        const savedLanguage = localStorage.getItem('preferredLanguage');
        if (savedLanguage && (savedLanguage === 'zh' || savedLanguage === 'en')) {
            this.currentLanguage = savedLanguage;
            this.updateLanguageButton();
            this.applyTranslations();
        }
    }

    // 切换语言
    toggleLanguage() {
        this.currentLanguage = this.currentLanguage === 'zh' ? 'en' : 'zh';
        localStorage.setItem('preferredLanguage', this.currentLanguage);
        this.updateLanguageButton();
        this.applyTranslations();
    }

    // 更新语言按钮文本
    updateLanguageButton() {
        const languageText = document.querySelector('.language-text');
        if (languageText) {
            languageText.textContent = this.currentLanguage === 'zh' ? '中文' : 'English';
        }
    }

    // 应用翻译
    applyTranslations() {
        const translations = this.translations[this.currentLanguage];
        if (!translations) return;

        // 更新所有带有 data-i18n 属性的元素
        document.querySelectorAll('[data-i18n]').forEach(element => {
            const key = element.getAttribute('data-i18n');
            const translation = translations[key];
            
            if (translation !== undefined) {
                if (element.tagName === 'INPUT' || element.tagName === 'TEXTAREA') {
                    element.value = translation;
                } else if (element.tagName === 'BUTTON' && !element.querySelector('span')) {
                    // 按钮且没有子 span，直接设置文本
                    const icon = element.querySelector('i');
                    if (icon) {
                        element.innerHTML = '';
                        element.appendChild(icon.cloneNode(true));
                        element.appendChild(document.createTextNode(' ' + translation));
                    } else {
                        element.textContent = translation;
                    }
                } else {
                    element.innerHTML = translation;
                }
            }
        });

        // 更新导航按钮
        this.updateNavigationButtons();
        
        // 更新模块标题和描述
        this.updateModuleContent();
        
        // 触发语言切换事件，通知其他模块（如 Canvas 重绘）
        document.dispatchEvent(new CustomEvent('languageChanged', { 
            detail: { language: this.currentLanguage } 
        }));
        
        // 更新页面导航标题
        this.updatePageNavTitles();

        // 触发语言变更事件
        const event = new CustomEvent('languageChanged', {
            detail: { language: this.currentLanguage }
        });
        document.dispatchEvent(event);
        
        console.log('Language applied:', this.currentLanguage);
    }

    // 更新导航按钮
    updateNavigationButtons() {
        const translations = this.translations[this.currentLanguage];
        const navButtons = document.querySelectorAll('.nav-btn[data-module]');
        
        const moduleKeys = [
            'nav.module1',
            'nav.module2',
            'nav.module3',
            'nav.module4',
            'nav.module5',
            'nav.module6'
        ];
        
        navButtons.forEach((button, index) => {
            const key = moduleKeys[index];
            if (key && translations[key]) {
                const span = button.querySelector('span');
                if (span) {
                    span.textContent = translations[key];
                }
            }
        });

        // 更新进度文本
        const progressText = document.querySelector('.progress-text');
        if (progressText) {
            const progressLabel = translations['nav.progress'] || 'Progress';
            const currentModule = document.getElementById('currentModule')?.textContent || '1';
            const totalModules = document.getElementById('totalModules')?.textContent || '6';
            progressText.innerHTML = `${progressLabel}: <span id="currentModule">${currentModule}</span> / <span id="totalModules">${totalModules}</span>`;
        }
        
        // 更新箭头按钮标题
        const prevBtn = document.getElementById('prevBtn');
        const nextBtn = document.getElementById('nextBtn');
        if (prevBtn) {
            prevBtn.setAttribute('title', translations['btn.prevModule'] || 'Previous Module');
        }
        if (nextBtn) {
            nextBtn.setAttribute('title', translations['btn.nextModule'] || 'Next Module');
        }
    }

    // 更新模块标题和描述
    updateModuleContent() {
        const translations = this.translations[this.currentLanguage];
        
        // 更新折叠标题
        const collapsedTitle = document.querySelector('.collapsed-title h2');
        if (collapsedTitle) {
            const icon = collapsedTitle.querySelector('i');
            if (icon && translations['collapsed.title']) {
                collapsedTitle.innerHTML = '';
                collapsedTitle.appendChild(icon.cloneNode(true));
                collapsedTitle.appendChild(document.createTextNode(' ' + translations['collapsed.title']));
            }
        }
        
        // 更新模块标题和描述
        const moduleHeaders = document.querySelectorAll('.module-header');
        moduleHeaders.forEach((header, index) => {
            const moduleId = index + 1;
            const titleKey = `module${moduleId}.title`;
            const descKey = `module${moduleId}.description`;
            
            const h2 = header.querySelector('h2');
            const p = header.querySelector('.module-description p');
            
            if (h2 && translations[titleKey]) {
                // 保留图标，只更新文本
                const icon = h2.querySelector('i');
                if (icon) {
                    h2.innerHTML = '';
                    h2.appendChild(icon.cloneNode(true));
                    h2.appendChild(document.createTextNode(' ' + translations[titleKey]));
                }
            }
            
            if (p && translations[descKey]) {
                p.textContent = translations[descKey];
            }
        });
    }
    
    // 更新页面导航标题
    updatePageNavTitles() {
        const translations = this.translations[this.currentLanguage];
        
        // 更新所有页面导航按钮的 title 属性
        document.querySelectorAll('.page-nav-btn[data-page]').forEach(button => {
            const pageNav = button.closest('.page-nav');
            if (!pageNav) return;
            
            const moduleId = pageNav.id.replace('page-nav-', '');
            const pageId = button.getAttribute('data-page');
            const key = `module${moduleId}.page${pageId}.navTitle`;
            
            if (translations[key]) {
                button.setAttribute('title', translations[key]);
            }
        });
    }

    // 获取当前语言
    getCurrentLanguage() {
        return this.currentLanguage;
    }

    // 设置语言
    setLanguage(language) {
        if (language === 'zh' || language === 'en') {
            this.currentLanguage = language;
            localStorage.setItem('preferredLanguage', language);
            this.updateLanguageButton();
            this.applyTranslations();
        }
    }
}

// 初始化语言切换器
document.addEventListener('DOMContentLoaded', () => {
    window.languageSwitcher = new LanguageSwitcher();
    console.log('Language switcher initialized');
});

// 全局函数
function setLanguage(language) {
    if (window.languageSwitcher) {
        window.languageSwitcher.setLanguage(language);
    }
}

function getCurrentLanguage() {
    return window.languageSwitcher ? window.languageSwitcher.getCurrentLanguage() : 'zh';
}
