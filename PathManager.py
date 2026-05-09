import sys
import os
import json
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, 
    QTreeWidget, QTreeWidgetItem, QPushButton, QFileDialog, 
    QLabel, QComboBox, QMenu, QAction, QSplitter, QToolBar, QHeaderView
)
from PyQt5.QtCore import Qt, QMimeData, QSize
from PyQt5.QtGui import QDrag, QIcon, QFont

class PathManager(QMainWindow):
    def __init__(self):
        super().__init__()
        self.current_json = "work.json"
        self.json_dir = "a:\\DKL\\DaaKuuLaa.github.io"
        self.selected_items = []
        self.last_selected_item = None
        self.initUI()
        self.load_json()
    
    def initUI(self):
        self.setWindowTitle("PathManager")
        self.setGeometry(100, 100, 1000, 700)
        
        # 设置毛玻璃效果样式
        self.setStyleSheet("""
            QMainWindow {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #f5f7fa, stop:1 #c3cfe2);
            }
            QWidget {
                background: rgba(255, 255, 255, 0.25);
                backdrop-filter: blur(10px);
                border-radius: 15px;
                border: 1px solid rgba(255, 255, 255, 0.18);
            }
            QToolBar {
                background: rgba(255, 255, 255, 0.3);
                border: none;
                padding: 2px;
                spacing: 5px;
            }
            QToolButton {
                background: rgba(255, 255, 255, 0.2);
                border-radius: 8px;
                padding: 5px;
                border: 1px solid rgba(255, 255, 255, 0.15);
            }
            QToolButton:hover {
                background: rgba(255, 255, 255, 0.35);
            }
            QTreeWidget {
                background: rgba(255, 255, 255, 0.2);
                border-radius: 10px;
            }
            QTreeWidget::item {
                height: 25px;
            }
            QTreeWidget::item:selected {
                background: rgba(255, 255, 255, 0.3);
            }
            QComboBox {
                background: rgba(255, 255, 255, 0.3);
                border-radius: 8px;
                padding: 5px;
            }
        """)
        
        # 主布局
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        main_layout = QVBoxLayout(central_widget)
        main_layout.setContentsMargins(5, 5, 5, 5)
        main_layout.setSpacing(5)
        
        # 功能导航栏（工具栏）
        self.toolbar = QToolBar()
        self.toolbar.setMovable(False)
        self.toolbar.setIconSize(QSize(20, 20))
        
        # 清空按钮（垃圾桶图标）
        self.btn_clear = QPushButton("🗑️")
        self.btn_clear.setFixedSize(35, 35)
        self.btn_clear.setToolTip("清空 JSON")
        self.btn_clear.clicked.connect(self.clear_json)
        
        # 添加按钮（加号图标）
        self.btn_add = QPushButton("➕")
        self.btn_add.setFixedSize(35, 35)
        self.btn_add.setToolTip("添加文件/文件夹")
        self.btn_add.clicked.connect(self.add_files)
        
        # 扫描按钮（刷新图标）
        self.btn_scan = QPushButton("🔄")
        self.btn_scan.setFixedSize(35, 35)
        self.btn_scan.setToolTip("自动扫描")
        self.btn_scan.clicked.connect(self.scan_directory)
        
        self.toolbar.addWidget(self.btn_clear)
        self.toolbar.addWidget(self.btn_add)
        self.toolbar.addWidget(self.btn_scan)
        
        main_layout.addWidget(self.toolbar)
        
        # 下方分割布局
        splitter = QSplitter(Qt.Horizontal)
        
        # 左侧目录预览
        self.tree_widget = QTreeWidget()
        self.tree_widget.setHeaderLabels(["名称", "类型", "路径"])
        self.tree_widget.header().setSectionResizeMode(0, QHeaderView.ResizeToContents)
        self.tree_widget.header().setSectionResizeMode(1, QHeaderView.ResizeToContents)
        self.tree_widget.header().setSectionResizeMode(2, QHeaderView.Stretch)
        self.tree_widget.setContextMenuPolicy(Qt.CustomContextMenu)
        self.tree_widget.customContextMenuRequested.connect(self.show_context_menu)
        self.tree_widget.setDragEnabled(True)
        self.tree_widget.setAcceptDrops(True)
        self.tree_widget.setDropIndicatorShown(True)
        self.tree_widget.setSelectionMode(QTreeWidget.ExtendedSelection)
        self.tree_widget.setDragDropMode(QTreeWidget.InternalMove)
        
        # 连接拖拽事件
        self.tree_widget.dropEvent = self.on_drop
        self.tree_widget.dragEnterEvent = self.on_drag_enter
        self.tree_widget.dragMoveEvent = self.on_drag_move
        self.tree_widget.mousePressEvent = self.on_tree_mouse_press
        self.tree_widget.mouseMoveEvent = self.on_tree_mouse_move
        
        # 连接选择信号
        self.tree_widget.itemClicked.connect(self.on_item_clicked)
        
        # 右侧工作对象切换
        right_widget = QWidget()
        right_layout = QVBoxLayout(right_widget)
        right_layout.setContentsMargins(10, 10, 10, 10)
        
        self.label_current = QLabel("当前工作对象:")
        self.combo_json = QComboBox()
        self.combo_json.addItems(["work.json", "file.json"])
        self.combo_json.currentTextChanged.connect(self.change_json)
        
        # 重新加载按钮
        self.btn_reload = QPushButton("🔄")
        self.btn_reload.setFixedSize(35, 35)
        self.btn_reload.setToolTip("重新加载 JSON 文件")
        self.btn_reload.clicked.connect(self.reload_json)
        
        self.btn_browse = QPushButton("浏览 JSON 目录")
        self.btn_browse.clicked.connect(self.browse_json_dir)
        
        right_layout.addWidget(self.label_current)
        right_layout.addWidget(self.combo_json)
        right_layout.addWidget(self.btn_reload)
        right_layout.addWidget(self.btn_browse)
        right_layout.addStretch()
        
        splitter.addWidget(self.tree_widget)
        splitter.addWidget(right_widget)
        splitter.setSizes([800, 200])
        
        main_layout.addWidget(splitter)
    
    def load_json(self):
        try:
            json_path = os.path.join(self.json_dir, self.current_json)
            if os.path.exists(json_path):
                with open(json_path, 'r', encoding='utf-8') as f:
                    self.data = json.load(f)
                self.update_tree()
            else:
                # 创建默认结构
                self.data = {
                    "name": "Work" if self.current_json == "work.json" else "File",
                    "path": f"DaaKuuLaa.github.io/{'Work' if self.current_json == 'work.json' else 'File'}",
                    "type": "folder",
                    "projects": []
                }
                self.save_json()
                self.update_tree()
        except Exception as e:
            print(f"加载 JSON 失败：{e}")
            self.data = {
                "name": "Work" if self.current_json == "work.json" else "File",
                "path": f"DaaKuuLaa.github.io/{'Work' if self.current_json == 'work.json' else 'File'}",
                "type": "folder",
                "projects": []
            }
    
    def save_json(self):
        try:
            json_path = os.path.join(self.json_dir, self.current_json)
            with open(json_path, 'w', encoding='utf-8') as f:
                json.dump(self.data, f, ensure_ascii=False, indent=2)
        except Exception as e:
            print(f"保存 JSON 失败：{e}")
    
    def update_tree(self):
        self.tree_widget.clear()
        self.selected_items = []
        self.last_selected_item = None
        root_item = QTreeWidgetItem([self.data["name"], self.data["type"], self.data["path"]])
        root_item.setData(0, Qt.UserRole, self.data)
        self.tree_widget.addTopLevelItem(root_item)
        self.add_children(root_item, self.data.get("projects", []))
        root_item.setExpanded(True)
    
    def add_children(self, parent_item, children):
        for child in children:
            child_item = QTreeWidgetItem([child["name"], child["type"], child["path"]])
            child_item.setData(0, Qt.UserRole, child)
            parent_item.addChild(child_item)
            # index 类型和 folder 类型一样，可以展开显示子项目
            if child.get("type") in ["folder", "index"]:
                if "projects" in child and child["projects"]:
                    self.add_children(child_item, child["projects"])
    
    def clear_json(self):
        self.data = {
            "name": "Work" if self.current_json == "work.json" else "File",
            "path": f"DaaKuuLaa.github.io/{'Work' if self.current_json == 'work.json' else 'File'}",
            "type": "folder",
            "projects": []
        }
        self.save_json()
        self.update_tree()
    
    def add_files(self):
        # 使用 QFileDialog 获取文件和文件夹
        dialog = QFileDialog()
        dialog.setFileMode(QFileDialog.Directory)
        dialog.setOption(QFileDialog.ShowDirsOnly, False)
        
        # 让用户选择是添加文件还是文件夹
        choice = dialog.exec_()
        
        if choice:
            # 获取选中的目录
            directories = dialog.selectedFiles()
            if directories:
                for dir_path in directories:
                    self.add_file_to_json(dir_path)
                self.save_json()
                # 不重新渲染，只更新 UI 显示
                self.refresh_view()
    
    def add_file_to_json(self, file_path):
        # 获取相对路径（从 json_dir 开始）
        relative_path = file_path.replace(self.json_dir + "\\", "").replace("\\", "/")
        # 添加 DaaKuuLaa.github.io/ 前缀
        if not relative_path.startswith("DaaKuuLaa.github.io/"):
            relative_path = "DaaKuuLaa.github.io/" + relative_path
        name = os.path.basename(file_path)
        
        # 确定文件类型
        if os.path.isdir(file_path):
            file_type = "folder"
            projects = self.scan_directory_recursive(file_path)
        else:
            ext = os.path.splitext(file_path)[1].lower()
            if ext == ".html":
                file_type = "html"
            elif ext == ".css":
                file_type = "css"
            elif ext == ".js":
                file_type = "javascript"
            elif ext in [".md", ".markdown"]:
                file_type = "markdown"
            else:
                file_type = "file"
            projects = []
        
        # 找到正确的父节点
        parts = relative_path.split("/")
        parent_data = self.data
        
        for i, part in enumerate(parts[:-1]):
            found = False
            for project in parent_data.get("projects", []):
                if project["name"] == part:
                    parent_data = project
                    found = True
                    break
            if not found:
                # 创建中间目录
                new_folder = {
                    "name": part,
                    "path": "/".join(parts[:i+1]),
                    "type": "folder",
                    "projects": []
                }
                parent_data.setdefault("projects", []).append(new_folder)
                parent_data = new_folder
        
        # 检查是否已存在
        for project in parent_data.get("projects", []):
            if project["name"] == name:
                return
        
        # 添加文件
        new_item = {
            "name": name,
            "path": relative_path,
            "type": file_type,
            "projects": projects
        }
        parent_data.setdefault("projects", []).append(new_item)
    
    def scan_directory_recursive(self, directory):
        projects = []
        for root, dirs, files in os.walk(directory):
            for file in files:
                file_path = os.path.join(root, file)
                relative_path = file_path.replace(self.json_dir + "\\", "").replace("\\", "/")
                # 添加 DaaKuuLaa.github.io/ 前缀
                if not relative_path.startswith("DaaKuuLaa.github.io/"):
                    relative_path = "DaaKuuLaa.github.io/" + relative_path
                name = file
                
                ext = os.path.splitext(file)[1].lower()
                if ext == ".html":
                    file_type = "html"
                elif ext == ".css":
                    file_type = "css"
                elif ext == ".js":
                    file_type = "javascript"
                elif ext in [".md", ".markdown"]:
                    file_type = "markdown"
                else:
                    file_type = "file"
                
                projects.append({
                    "name": name,
                    "path": relative_path,
                    "type": file_type,
                    "projects": []
                })
        return projects
    
    def scan_directory(self):
        target_dir = os.path.join(self.json_dir, "Work" if self.current_json == "work.json" else "File")
        if os.path.exists(target_dir):
            self.data["projects"] = []
            self.scan_directory_full(target_dir, self.data["projects"])
            self.save_json()
            self.update_tree()
    
    def scan_directory_full(self, directory, projects):
        for item in os.listdir(directory):
            item_path = os.path.join(directory, item)
            relative_path = item_path.replace(self.json_dir + "\\", "").replace("\\", "/")
            # 添加 DaaKuuLaa.github.io/ 前缀
            if not relative_path.startswith("DaaKuuLaa.github.io/"):
                relative_path = "DaaKuuLaa.github.io/" + relative_path
            
            if os.path.isdir(item_path):
                new_item = {
                    "name": item,
                    "path": relative_path,
                    "type": "folder",
                    "projects": []
                }
                projects.append(new_item)
                self.scan_directory_full(item_path, new_item["projects"])
            else:
                ext = os.path.splitext(item)[1].lower()
                if ext == ".html":
                    file_type = "html"
                elif ext == ".css":
                    file_type = "css"
                elif ext == ".js":
                    file_type = "javascript"
                elif ext in [".md", ".markdown"]:
                    file_type = "markdown"
                else:
                    file_type = "file"
                
                new_item = {
                    "name": item,
                    "path": relative_path,
                    "type": file_type,
                    "projects": []
                }
                projects.append(new_item)
    
    def change_json(self, text):
        try:
            self.current_json = text
            self.selected_items = []
            self.last_selected_item = None
            self.load_json()
        except Exception as e:
            print(f"切换 JSON 失败：{e}")
    
    def reload_json(self):
        """重新加载当前 JSON 文件"""
        self.load_json()
    
    def browse_json_dir(self):
        directory = QFileDialog.getExistingDirectory(self, "选择 JSON 目录")
        if directory:
            self.json_dir = directory
            self.load_json()
    
    def on_item_clicked(self, item, column):
        modifiers = QApplication.keyboardModifiers()
        
        if modifiers == Qt.ControlModifier:
            # Ctrl 点击：切换选择状态
            if item in self.selected_items:
                self.selected_items.remove(item)
                item.setSelected(False)
            else:
                self.selected_items.append(item)
                item.setSelected(True)
            self.last_selected_item = item
        elif modifiers == Qt.ShiftModifier and self.last_selected_item:
            # Shift 点击：选择范围内所有项
            parent = item.parent()
            last_parent = self.last_selected_item.parent()
            
            if parent == last_parent:
                # 在同一父节点下
                items = []
                for i in range(parent.topLevelItemCount() if not parent else parent.childCount()):
                    items.append(parent.topLevelItem(i) if not parent else parent.child(i))
                
                if item in items and self.last_selected_item in items:
                    start_idx = items.index(self.last_selected_item)
                    end_idx = items.index(item)
                    
                    if start_idx > end_idx:
                        start_idx, end_idx = end_idx, start_idx
                    
                    # 清除之前的选择（保留 Ctrl 选中的其他项）
                    for sel_item in self.selected_items[:]:
                        if sel_item in items:
                            self.selected_items.remove(sel_item)
                            sel_item.setSelected(False)
                    
                    # 添加范围内的所有项
                    for i in range(start_idx, end_idx + 1):
                        if items[i] not in self.selected_items:
                            self.selected_items.append(items[i])
                            items[i].setSelected(True)
        else:
            # 普通点击：单选
            if item not in self.selected_items:
                for sel_item in self.selected_items:
                    sel_item.setSelected(False)
                self.selected_items = [item]
                item.setSelected(True)
            
            self.last_selected_item = item
    
    def show_context_menu(self, position):
        # 检查是否有选中的项目
        has_selection = len(self.selected_items) > 0
        
        # 获取右键点击的项目
        item = self.tree_widget.itemAt(position)
        if not item:
            return
        
        menu = QMenu()
        
        delete_action = QAction("删除")
        delete_action.triggered.connect(self.delete_selected_items)
        menu.addAction(delete_action)
        
        # 只要有选中的项目，就显示"合并为 index 类型"选项
        if has_selection:
            merge_action = QAction("合并为 index 类型")
            merge_action.triggered.connect(self.merge_selected_to_index)
            menu.addAction(merge_action)
        
        menu.exec_(self.tree_widget.viewport().mapToGlobal(position))
    
    def delete_selected_items(self):
        for item in self.selected_items[:]:
            parent = item.parent()
            if parent:
                parent_data = parent.data(0, Qt.UserRole)
                item_data = item.data(0, Qt.UserRole)
                # 直接从 self.data 中删除，而不是只修改 parent_data
                self.remove_from_data(self.data, item_data["name"])
                parent.removeChild(item)
                self.selected_items.remove(item)
        
        self.save_json()
        print(f"已保存 JSON，删除了 {len(self.selected_items)} 个项目")
    
    def remove_from_data(self, data, name):
        """递归从 data 中删除指定名称的项目"""
        if "projects" in data:
            data["projects"] = [p for p in data["projects"] if p["name"] != name]
            # 递归检查子项目
            for project in data["projects"]:
                if "projects" in project:
                    self.remove_from_data(project, name)
    
    def refresh_view(self):
        """刷新视图，不重新加载整个树"""
        # 这个方法用于在数据改变后刷新显示
        pass  # 已经在删除时直接移除了 UI 项，不需要额外操作
    
    def merge_selected_to_index(self):
        """合并选中的所有项目为一个 index 类型"""
        if not self.selected_items:
            return
        
        # 获取第一个选中项作为 index 的基础
        first_item = self.selected_items[0]
        first_data = first_item.data(0, Qt.UserRole)
        parent = first_item.parent()
        
        if not parent:
            return  # 根节点不能合并
        
        parent_data = parent.data(0, Qt.UserRole)
        
        # 收集所有选中项的数据（包括文件夹）
        merged_projects = []
        for item in self.selected_items:
            item_data = item.data(0, Qt.UserRole)
            merged_projects.append({
                "name": item_data["name"],
                "path": item_data["path"],
                "type": item_data["type"],
                "projects": item_data.get("projects", [])
            })
        
        # 创建新的 index 项目
        # index 的 path 应该和父节点一致，因为它是当前目录的索引
        index_project = {
            "name": "index",
            "path": parent_data["path"],
            "type": "index",
            "projects": merged_projects
        }
        
        # 从 self.data 中递归删除选中的项目（参考删除操作的实现）
        names_to_remove = [item.data(0, Qt.UserRole)["name"] for item in self.selected_items]
        for name in names_to_remove:
            self.remove_from_data(self.data, name)
        
        # 找到父节点在 self.data 中的位置并添加 index 项目
        self.add_index_to_parent(self.data, parent_data["name"], index_project)
        
        # 保存 JSON
        self.save_json()
        
        # 直接更新 UI，不重新渲染整个树（类似删除操作）
        # 先移除所有选中的项目
        for item in self.selected_items[:]:
            parent.removeChild(item)
        
        # 创建新的 index 项目项
        index_item = QTreeWidgetItem([index_project["name"], index_project["type"], index_project["path"]])
        index_item.setData(0, Qt.UserRole, index_project)
        parent.addChild(index_item)
        
        # 添加子项目（如果有）
        if merged_projects:
            for child_project in merged_projects:
                child_item = QTreeWidgetItem([child_project["name"], child_project["type"], child_project["path"]])
                child_item.setData(0, Qt.UserRole, child_project)
                index_item.addChild(child_item)
        
        # 展开 index 项
        index_item.setExpanded(True)
        
        # 清空选择
        self.selected_items = []
        self.last_selected_item = None
    
    def add_index_to_parent(self, data, parent_name, index_project):
        """递归找到指定父节点并添加 index 项目"""
        if "projects" in data:
            for project in data["projects"]:
                if project["name"] == parent_name:
                    project.setdefault("projects", []).append(index_project)
                    return
                if "projects" in project:
                    self.add_index_to_parent(project, parent_name, index_project)
    
    def merge_to_index(self, item):
        """保留旧函数作为兼容"""
        self.merge_selected_to_index()
    
    def keyPressEvent(self, event):
        if event.key() == Qt.Key_Delete:
            self.delete_selected_items()
        elif event.key() == Qt.Key_Escape:
            for sel_item in self.selected_items:
                sel_item.setSelected(False)
            self.selected_items = []
            self.last_selected_item = None
    
    def on_tree_mouse_press(self, event):
        """处理树形控件的鼠标按下事件"""
        if event.button() == Qt.LeftButton:
            item = self.tree_widget.itemAt(event.pos())
            if item:
                self.drag_start_position = event.pos()
        # 调用原始鼠标按下事件处理
        QTreeWidget.mousePressEvent(self.tree_widget, event)
    
    def on_tree_mouse_move(self, event):
        """处理树形控件的鼠标移动事件 - 触发拖拽"""
        if event.buttons() & Qt.LeftButton:
            if hasattr(self, 'drag_start_position'):
                if (event.pos() - self.drag_start_position).manhattanLength() >= QApplication.startDragDistance():
                    item = self.tree_widget.itemAt(self.drag_start_position)
                    if item:
                        mime_data = QMimeData()
                        # 使用唯一标识符（路径）而不是名称
                        item_data = item.data(0, Qt.UserRole)
                        mime_data.setText(item_data["path"])
                        
                        drag = QDrag(self.tree_widget)
                        drag.setMimeData(mime_data)
                        drag.exec_(Qt.MoveAction)
                        return
        # 调用原始鼠标移动事件处理
        QTreeWidget.mouseMoveEvent(self.tree_widget, event)
    
    def on_drag_enter(self, event):
        """处理拖拽进入事件"""
        if event.source() == self.tree_widget:
            event.acceptProposedAction()
    
    def on_drag_move(self, event):
        """处理拖拽移动事件"""
        if event.source() == self.tree_widget:
            event.acceptProposedAction()
    
    def on_drop(self, event):
        """处理放置事件 - 实现移动功能"""
        source_item = self.tree_widget.itemAt(event.pos() - self.tree_widget.viewport().pos())
        
        # 如果没有目标项，尝试获取根节点
        if not source_item:
            source_item = self.tree_widget.topLevelItem(0)
        
        if not source_item:
            return
        
        source_data = source_item.data(0, Qt.UserRole)
        
        # 获取被拖拽的项目（从 mimeData 中获取）
        mime_data = event.mimeData()
        if not mime_data.hasText():
            return
        
        dragged_path = mime_data.text()
        
        # 找到被拖拽的项目在 self.data 中的位置
        dragged_item_data = self.find_item_by_path(self.data, dragged_path)
        
        if not dragged_item_data:
            return
        
        # 检查是否是移动到自己或自己的子节点
        if self.is_child_of(dragged_item_data, source_data):
            return
        
        # 从原位置删除
        self.remove_from_data(self.data, dragged_item_data["name"])
        
        # 添加到新位置
        source_data.setdefault("projects", []).append(dragged_item_data)
        
        # 保存 JSON
        self.save_json()
        
        # 刷新 UI
        self.update_tree()
        
        event.acceptProposedAction()
    
    def find_item_by_path(self, data, path):
        """递归在 data 中查找指定路径的项目"""
        if data.get("path") == path:
            return data
        if "projects" in data:
            for project in data["projects"]:
                if project.get("path") == path:
                    return project
                result = self.find_item_by_path(project, path)
                if result:
                    return result
        return None
    
    def find_item_in_data(self, data, name):
        """递归在 data 中查找指定名称的项目"""
        if "projects" in data:
            for project in data["projects"]:
                if project["name"] == name:
                    return project
                result = self.find_item_in_data(project, name)
                if result:
                    return result
        return None
    
    def is_child_of(self, child_data, parent_data):
        """检查 child_data 是否是 parent_data 的子节点"""
        if child_data == parent_data:
            return True
        if "projects" in parent_data:
            for project in parent_data["projects"]:
                if self.is_child_of(child_data, project):
                    return True
        return False

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = PathManager()
    window.show()
    sys.exit(app.exec_())
