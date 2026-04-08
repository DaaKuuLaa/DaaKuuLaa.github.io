// ==========================================
// 页面控制器
// Page Navigation Controller
// ==========================================

class PageController {
  constructor(moduleId) {
    this.moduleId = moduleId;
    this.currentPage = 1;
    this.totalPages = 1;
    this.pages = [];
    this.pageNavButtons = [];
    this.prevPageBtn = null;
    this.nextPageBtn = null;
    this.pageCurrentSpan = null;
    this.pageTotalSpan = null;
    this.init();
  }

  init() {
    this.cacheDOMElements();
    this.calculateTotalPages();
    this.bindEvents();
    this.showPage(1);
  }

  cacheDOMElements() {
    const module = document.getElementById(`module-${this.moduleId}`);
    if (!module) return;

    // 缓存页面元素
    this.pages = module.querySelectorAll('.page');
    
    // 缓存页面导航按钮
    const pageNav = document.getElementById(`page-nav-${this.moduleId}`);
    if (pageNav) {
      this.pageNavButtons = pageNav.querySelectorAll('.page-nav-btn[data-page]');
      this.pageCurrentSpan = pageNav.querySelector('.page-current');
      this.pageTotalSpan = pageNav.querySelector('.page-total');
    }
    
    // 缓存页面控制按钮
    this.prevPageBtn = document.getElementById(`prev-page-${this.moduleId}`);
    this.nextPageBtn = document.getElementById(`next-page-${this.moduleId}`);
  }

  calculateTotalPages() {
    this.totalPages = this.pages.length;
    if (this.pageTotalSpan) {
      this.pageTotalSpan.textContent = this.totalPages;
    }
  }

  bindEvents() {
    // 页面导航按钮事件（右侧圆点导航）
    this.pageNavButtons.forEach(button => {
      button.addEventListener('click', (e) => {
        const pageId = parseInt(e.currentTarget.dataset.page);
        this.showPage(pageId);
      });
    });

    // 注意：前进/后退按钮事件已在HTML的onclick中绑定，这里不再重复绑定
    // 避免事件重复触发导致跳转两页的问题
  }

  showPage(pageId) {
    if (pageId < 1 || pageId > this.totalPages) return;

    // 隐藏当前页面
    if (this.pages[this.currentPage - 1]) {
      this.pages[this.currentPage - 1].classList.remove('active');
    }

    // 显示新页面
    this.currentPage = pageId;
    if (this.pages[this.currentPage - 1]) {
      this.pages[this.currentPage - 1].classList.add('active');
    }

    // 更新页面导航状态
    this.updatePageNavigationState();
    this.updatePageProgress();
    this.updateActivePageNavButton();

    // 触发页面显示事件
    this.onPageShown(this.currentPage);
  }

  navigatePage(direction) {
    const newPageId = this.currentPage + direction;
    if (newPageId >= 1 && newPageId <= this.totalPages) {
      this.showPage(newPageId);
    }
  }

  updatePageNavigationState() {
    // 更新前进/后退按钮状态
    if (this.prevPageBtn) {
      this.prevPageBtn.disabled = this.currentPage === 1;
    }
    
    if (this.nextPageBtn) {
      this.nextPageBtn.disabled = this.currentPage === this.totalPages;
    }
  }

  updatePageProgress() {
    if (this.pageCurrentSpan) {
      this.pageCurrentSpan.textContent = this.currentPage;
    }
  }

  updateActivePageNavButton() {
    this.pageNavButtons.forEach(button => {
      const pageId = parseInt(button.dataset.page);
      if (pageId === this.currentPage) {
        button.classList.add('active');
      } else {
        button.classList.remove('active');
      }
    });
  }

  onPageShown(pageId) {
    // 触发展示事件
    const event = new CustomEvent('pageShown', {
      detail: { 
        moduleId: this.moduleId,
        pageId 
      }
    });
    document.dispatchEvent(event);

    // 记录到控制台（调试用）
    console.log(`Module ${this.moduleId} - Page ${pageId} shown`);
  }

  // 跳转到特定页面（公开方法）
  goToPage(pageId) {
    this.showPage(pageId);
  }

  // 获取当前页面ID（公开方法）
  getCurrentPage() {
    return this.currentPage;
  }

  // 获取总页面数（公开方法）
  getTotalPages() {
    return this.totalPages;
  }
}

// ==========================================
// 导航控制模块
// Navigation Control Module
// ==========================================

class NavigationController {
  constructor() {
    this.currentModule = 1;
    this.totalModules = 7;
    this.modules = [];
    this.navButtons = [];
    this.pageControllers = new Map(); // 存储各模块的页面控制器
    this.init();
  }

  init() {
    this.cacheDOMElements();
    this.bindEvents();
    this.initializePageControllers();
    this.updateProgress();
    this.showModule(1);
  }

  cacheDOMElements() {
    // 缓存模块元素
    this.modules = [];
    for (let i = 1; i <= this.totalModules; i++) {
      const module = document.getElementById(`module-${i}`);
      if (module) {
        this.modules[i] = module;
      }
    }

    // 缓存导航按钮
    this.navButtons = document.querySelectorAll('.nav-btn[data-module]');
    this.prevBtn = document.getElementById('prevBtn');
    this.nextBtn = document.getElementById('nextBtn');
    
    // 缓存进度条元素
    this.progressFill = document.getElementById('progressFill');
    this.currentModuleSpan = document.getElementById('currentModule');
  }

  initializePageControllers() {
    // 为每个有分页的模块创建页面控制器
    for (let i = 1; i <= this.totalModules; i++) {
      const module = document.getElementById(`module-${i}`);
      if (module && module.querySelectorAll('.page').length > 0) {
        this.pageControllers.set(i, new PageController(i));
      }
    }
  }

  bindEvents() {
    // 导航按钮事件
    this.navButtons.forEach(button => {
      button.addEventListener('click', (e) => {
        const moduleId = parseInt(e.currentTarget.dataset.module);
        this.showModule(moduleId);
      });
    });

    // 前进/后退按钮事件
    if (this.prevBtn) {
      this.prevBtn.addEventListener('click', () => this.navigate(-1));
    }
    
    if (this.nextBtn) {
      this.nextBtn.addEventListener('click', () => this.navigate(1));
    }

    // 键盘导航事件 - 修改：左右切换模块，上下切换子页面
    document.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowLeft') {
        // 左箭头：切换到上一模块
        this.navigate(-1);
      } else if (e.key === 'ArrowRight') {
        // 右箭头：切换到下一模块
        this.navigate(1);
      } else if (e.key === 'ArrowUp') {
        // 上箭头：在当前模块内切换到上一页
        const pageController = this.pageControllers.get(this.currentModule);
        if (pageController) {
          pageController.navigatePage(-1);
        }
      } else if (e.key === 'ArrowDown') {
        // 下箭头：在当前模块内切换到下一页
        const pageController = this.pageControllers.get(this.currentModule);
        if (pageController) {
          pageController.navigatePage(1);
        }
      }
    });

    // 窗口大小变化事件
    window.addEventListener('resize', this.debounce(() => {
      this.onWindowResize();
    }, 250));
  }

  showModule(moduleId) {
    if (moduleId < 1 || moduleId > this.totalModules) return;

    // 隐藏当前模块
    if (this.modules[this.currentModule]) {
      this.modules[this.currentModule].classList.remove('active');
    }

    // 显示新模块
    this.currentModule = moduleId;
    if (this.modules[this.currentModule]) {
      this.modules[this.currentModule].classList.add('active');
    }

    // 更新导航状态
    this.updateNavigationState();
    this.updateProgress();
    this.updateActiveNavButton();

    // 重置模块到第一页（如果有分页）
    const pageController = this.pageControllers.get(moduleId);
    if (pageController) {
      pageController.goToPage(1);
    }

    // 触发模块显示事件
    this.onModuleShown(this.currentModule);
  }

  navigate(direction) {
    const newModuleId = this.currentModule + direction;
    if (newModuleId >= 1 && newModuleId <= this.totalModules) {
      this.showModule(newModuleId);
    }
  }

  updateNavigationState() {
    // 更新前进/后退按钮状态
    if (this.prevBtn) {
      this.prevBtn.disabled = this.currentModule === 1;
    }
    
    if (this.nextBtn) {
      this.nextBtn.disabled = this.currentModule === this.totalModules;
    }
  }

  updateProgress() {
    const progress = (this.currentModule / this.totalModules) * 100;
    
    if (this.progressFill) {
      this.progressFill.style.width = `${progress}%`;
    }
    
    if (this.currentModuleSpan) {
      this.currentModuleSpan.textContent = this.currentModule;
    }
  }

  updateActiveNavButton() {
    this.navButtons.forEach(button => {
      const moduleId = parseInt(button.dataset.module);
      if (moduleId === this.currentModule) {
        button.classList.add('active');
      } else {
        button.classList.remove('active');
      }
    });
  }

  onModuleShown(moduleId) {
    // 触发模块特定的初始化
    const event = new CustomEvent('moduleShown', {
      detail: { moduleId }
    });
    document.dispatchEvent(event);

    // 记录到控制台（调试用）
    console.log(`Module ${moduleId} shown`);
  }

  onWindowResize() {
    // 窗口大小变化时的处理
    const event = new CustomEvent('windowResized', {
      detail: { moduleId: this.currentModule }
    });
    document.dispatchEvent(event);
  }

  // 工具函数：防抖
  debounce(func, wait) {
    let timeout;
    return function executedFunction(...args) {
      const later = () => {
        clearTimeout(timeout);
        func(...args);
      };
      clearTimeout(timeout);
      timeout = setTimeout(later, wait);
    };
  }

  // 跳转到特定模块（公开方法）
  goToModule(moduleId) {
    this.showModule(moduleId);
  }

  // 获取当前模块ID（公开方法）
  getCurrentModule() {
    return this.currentModule;
  }

  // 获取总模块数（公开方法）
  getTotalModules() {
    return this.totalModules;
  }
}

// ==========================================
// 全局函数
// ==========================================

// 跳转到指定模块
function goToModule(moduleId) {
  if (window.navigationController) {
    window.navigationController.goToModule(moduleId);
  }
}

// 导航到上一模块
function previousModule() {
  if (window.navigationController) {
    window.navigationController.navigate(-1);
  }
}

// 导航到下一模块
function nextModule() {
  if (window.navigationController) {
    window.navigationController.navigate(1);
  }
}

// 重新开始演示
function restartDemo() {
  if (window.navigationController) {
    window.navigationController.goToModule(1);
  }
}

// 在模块内导航页面
function navigatePage(moduleId, direction) {
  if (window.navigationController) {
    const pageController = window.navigationController.pageControllers.get(moduleId);
    if (pageController) {
      pageController.navigatePage(direction);
    }
  }
}

// 跳转到指定页面的全局函数
function goToPage(moduleId, pageId) {
  if (window.navigationController) {
    const pageController = window.navigationController.pageControllers.get(moduleId);
    if (pageController) {
      pageController.goToPage(pageId);
    }
  }
}

// ==========================================
// 初始化
// ==========================================

document.addEventListener('DOMContentLoaded', () => {
  // 初始化导航控制器
  window.navigationController = new NavigationController();
  
  console.log('Navigation system initialized');
});
