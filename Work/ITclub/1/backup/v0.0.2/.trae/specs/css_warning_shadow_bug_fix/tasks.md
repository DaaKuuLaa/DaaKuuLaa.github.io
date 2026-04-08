# CSS 警告和第七页阴影 BUG 修复 - 实现计划

## [x] 任务 1: 修复 CSS appearance 属性警告
- **Priority**: P0
- **Depends On**: None
- **Description**: 
  - 修复 main.css 第 1209 行的 `-webkit-appearance` 属性警告
  - 添加标准的 `appearance` 属性以实现兼容性
- **Acceptance Criteria Addressed**: [AC-1]
- **Test Requirements**:
  - `human-judgement` TR-1.1: CSS 警告不再显示
  - `programmatic` TR-1.2: 检查 appearance 属性是否同时定义了标准属性和前缀属性

## [x] 任务 2: 彻底排查第三模块第七页阴影问题
- **Priority**: P0
- **Depends On**: None
- **Description**: 
  - 全面检查第三模块第七页的所有元素
  - 重点检查 insight-box 元素的阴影
  - 检查是否有其他元素导致阴影问题
- **Acceptance Criteria Addressed**: [AC-2]
- **Test Requirements**:
  - `human-judgement` TR-2.1: 检查第七页所有元素的阴影
  - `human-judgement` TR-2.2: 确保 insight-box 元素阴影正常

## [x] 任务 3: 修复 insight-box 元素的阴影问题
- **Priority**: P0
- **Depends On**: 任务 2
- **Description**: 
  - 修复 insight-box 元素的阴影设置
  - 确保与其他元素的阴影一致
- **Acceptance Criteria Addressed**: [AC-2]
- **Test Requirements**:
  - `human-judgement` TR-3.1: insight-box 阴影显示正常
  - `human-judgement` TR-3.2: 悬浮动画阴影过渡正常

## [x] 任务 4: 全面测试所有页面
- **Priority**: P1
- **Depends On**: 任务 1, 任务 2, 任务 3
- **Description**: 
  - 测试所有模块的所有页面
  - 确保阴影显示一致
  - 确保没有新问题引入
- **Acceptance Criteria Addressed**: [AC-3]
- **Test Requirements**:
  - `human-judgement` TR-4.1: 所有页面阴影显示一致
  - `human-judgement` TR-4.2: 没有新的布局异常
  - `programmatic` TR-4.3: 所有功能正常工作
