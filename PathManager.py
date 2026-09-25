import sys
import os
import re
import json
import urllib.parse
import urllib.request
import urllib.error
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, 
    QTreeWidget, QTreeWidgetItem, QPushButton, QFileDialog, 
    QLabel, QComboBox, QMenu, QAction, QSplitter, QToolBar, QHeaderView,
    QMessageBox, QDialog, QDialogButtonBox, QFormLayout, QGroupBox,
    QLineEdit, QProgressBar, QPlainTextEdit, QInputDialog
)
from PyQt5.QtCore import Qt, QMimeData, QSize, QThread, pyqtSignal
from PyQt5.QtGui import QDrag, QIcon, QFont

# ---------------------------------------------------------------------------
# GitHub Releases 发布支持
#
# 沿用仓库既有 Releases 的约定（见 README「下载（GitHub Releases）」）：
#   * 分类目录对应固定 tag：File/Tool -> tools、File/Game -> games、File/Test -> testdata
#   * 资产名 = 工具包文件名，长期保持不变
#   * 更新时覆盖同名资产，下载链接不变（file.json 里的 path 无需改动）
#   * 索引条目形如 {name, path: <releases 直链>, type: "file", projects: []}
# ---------------------------------------------------------------------------

REPO_OWNER = "DaaKuuLaa"
REPO_NAME = "DaaKuuLaa.github.io"
REPO_BRANCH = "main"
GH_API_BASE = "https://api.github.com"
GH_UPLOAD_BASE = "https://uploads.github.com"
RELEASES_DOWNLOAD_BASE = f"https://github.com/{REPO_OWNER}/{REPO_NAME}/releases/download"
TOKEN_FILE_NAME = ".pathmanager_token"

# (索引目录（与 file.json 中的 path 同格式）, release tag, release 标题)
RELEASE_CATEGORIES = [
    ("DaaKuuLaa.github.io/File/Tool", "tools", "工具 Tools"),
    ("DaaKuuLaa.github.io/File/Game", "games", "游戏 Games"),
    ("DaaKuuLaa.github.io/File/Test", "testdata", "测试数据"),
]
RELEASE_CATEGORY_BY_FOLDER = {item[0]: item for item in RELEASE_CATEGORIES}


class ReleaseError(Exception):
    """发布流程中可预期的错误，消息直接展示给用户。"""


def release_download_url(tag, asset_name):
    """拼出与既有条目完全一致格式的下载直链。"""
    return f"{RELEASES_DOWNLOAD_BASE}/{tag}/{asset_name}"


def parse_release_download_url(url):
    """把 Releases 直链还原为 (tag, asset_name)；不是本仓库 Releases 直链时返回 None。"""
    if not isinstance(url, str):
        return None
    prefix = RELEASES_DOWNLOAD_BASE + "/"
    if not url.startswith(prefix):
        return None
    tag, _, asset_name = url[len(prefix):].partition("/")
    if not tag or not asset_name or "/" in asset_name:
        return None
    return tag, asset_name


def sanitize_asset_name(name):
    """与网页端 Worker 的 sanitizeFileName 保持一致的清洗规则。"""
    base = os.path.basename((name or "").strip()) or "unnamed"
    return re.sub(r"[^\w.\-]", "_", base)


class GitHubReleases:
    """极简 GitHub Releases 客户端：只用标准库，便于打包进单文件 exe。"""

    def __init__(self, token, api_base=GH_API_BASE, upload_base=GH_UPLOAD_BASE, timeout=120):
        self.token = token
        self.api_base = api_base.rstrip("/")
        self.upload_base = upload_base.rstrip("/")
        self.timeout = timeout

    def _headers(self, extra=None):
        headers = {
            "Authorization": f"Bearer {self.token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "daakuulaa-pathmanager",
        }
        if extra:
            headers.update(extra)
        return headers

    def _send(self, method, url, data=None, headers=None):
        request = urllib.request.Request(
            url, data=data, method=method, headers=headers or self._headers())
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.read()

    def _api(self, method, path, payload=None):
        url = f"{self.api_base}/repos/{REPO_OWNER}/{REPO_NAME}{path}"
        data = None
        headers = self._headers()
        if payload is not None:
            data = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = "application/json"
        status, body = self._send(method, url, data, headers)
        parsed = None
        if body:
            try:
                parsed = json.loads(body.decode("utf-8"))
            except ValueError:
                parsed = None
        if status >= 400:
            detail = parsed.get("message") if isinstance(parsed, dict) else None
            suffix = detail or repr(body[:200])
            raise ReleaseError(f"GitHub API {method} {path} 返回 HTTP {status}：{suffix}")
        return parsed

    def get_release(self, tag):
        """按 tag 查询 Release；不存在返回 None。"""
        url = (f"{self.api_base}/repos/{REPO_OWNER}/{REPO_NAME}"
               f"/releases/tags/{urllib.parse.quote(tag)}")
        status, body = self._send("GET", url)
        if status == 404:
            return None
        if status >= 400:
            raise ReleaseError(f"查询 Release「{tag}」失败：HTTP {status} {body[:200]!r}")
        return json.loads(body.decode("utf-8"))

    def create_release(self, tag, title, body=""):
        return self._api("POST", "/releases", {
            "tag_name": tag,
            "name": title,
            "body": body,
            "target_commitish": REPO_BRANCH,
            "draft": False,
            "prerelease": False,
        })

    @staticmethod
    def find_asset(release, asset_name):
        for asset in release.get("assets") or []:
            if asset.get("name") == asset_name:
                return asset
        return None

    def delete_asset(self, asset_id):
        self._api("DELETE", f"/releases/assets/{asset_id}")

    def upload_asset(self, release, asset_name, file_path):
        """以固定资产名流式上传（不把整包读进内存）。"""
        size = os.path.getsize(file_path)
        url = (f"{self.upload_base}/repos/{REPO_OWNER}/{REPO_NAME}"
               f"/releases/{release['id']}/assets?"
               + urllib.parse.urlencode({"name": asset_name}))
        headers = self._headers({
            "Content-Type": "application/octet-stream",
            "Content-Length": str(size),
        })
        with open(file_path, "rb") as handle:
            request = urllib.request.Request(url, data=handle, method="POST", headers=headers)
            try:
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    return json.loads(response.read().decode("utf-8"))
            except urllib.error.HTTPError as exc:
                raise ReleaseError(f"上传资产「{asset_name}」失败：HTTP {exc.code} {exc.read()[:300]!r}")


def _ensure_release(client, tag, title, log):
    release = client.get_release(tag)
    if release is None:
        log(f"Release「{tag}」不存在，正在创建，标题 {title}…")
        release = client.create_release(
            tag, title,
            f"{title} 资源。资产文件名保持稳定，更新时覆盖同名资产即可，下载链接不变。")
    else:
        log(f"已找到 Release「{tag}」 · {release.get('name') or tag}")
    return release


def _check_local_file(file_path):
    if not os.path.isfile(file_path):
        raise ReleaseError(f"找不到文件：{file_path}")


def publish_new_tool(token, folder_path, asset_name, file_path, log=None, client=None):
    """首次发布工具：只上传资产，索引由调用方写入。返回下载直链。"""
    category = RELEASE_CATEGORY_BY_FOLDER.get(folder_path)
    if category is None:
        raise ReleaseError(f"未知分类目录：{folder_path}")
    _, tag, title = category
    asset_name = sanitize_asset_name(asset_name)
    if not asset_name:
        raise ReleaseError("资产名为空")
    _check_local_file(file_path)

    log = log or (lambda message: None)
    client = client or GitHubReleases(token)
    release = _ensure_release(client, tag, title, log)
    if client.find_asset(release, asset_name) is not None:
        raise ReleaseError(
            f"Release「{tag}」中已存在资产「{asset_name}」。\n"
            "该工具已发布过，请改用「更新工具」发布新版本。")
    log(f"上传 {os.path.basename(file_path)} · {os.path.getsize(file_path):,} 字节"
        f"→ 资产名「{asset_name}」…")
    client.upload_asset(release, asset_name, file_path)
    url = release_download_url(tag, asset_name)
    log(f"发布完成：{url}")
    return url


def publish_tool_update(token, tag, asset_name, file_path, log=None, client=None):
    """更新已有工具：删除同名旧资产后原样重传，下载直链保持不变。"""
    asset_name = sanitize_asset_name(asset_name)
    if not asset_name:
        raise ReleaseError("资产名为空")
    _check_local_file(file_path)

    log = log or (lambda message: None)
    client = client or GitHubReleases(token)
    release = client.get_release(tag)
    if release is None:
        raise ReleaseError(f"Release「{tag}」不存在")
    existing = client.find_asset(release, asset_name)
    if existing is None:
        raise ReleaseError(f"Release「{tag}」中找不到资产「{asset_name}」")

    log(f"删除旧版本「{asset_name}」 · asset id={existing['id']}…")
    client.delete_asset(existing["id"])
    log(f"上传新版本 · {os.path.getsize(file_path):,} 字节，资产名保持「{asset_name}」…")
    client.upload_asset(release, asset_name, file_path)
    url = release_download_url(tag, asset_name)
    log(f"更新完成，下载链接不变：{url}")
    return url


class ReleaseJob(QThread):
    """后台执行发布会话，避免界面卡死。"""

    progressed = pyqtSignal(str)
    succeeded = pyqtSignal(object)
    failed = pyqtSignal(str)

    def __init__(self, job, parent=None):
        super().__init__(parent)
        self._job = job

    def run(self):
        try:
            result = self._job(self.progressed.emit)
        except Exception as exc:
            self.failed.emit(str(exc))
        else:
            self.succeeded.emit(result)


class PathManager(QMainWindow):
    def __init__(self, repo_dir=None):
        super().__init__()
        self.current_json = "work.json"
        self.json_dir = repo_dir or self.default_json_dir()
        self.selected_items = []
        self.last_selected_item = None
        self.initUI()
        self.load_json()
    
    @staticmethod
    def default_json_dir():
        """定位索引 JSON 所在目录：优先脚本/exe 所在目录，
        若该目录没有 work.json / file.json 则逐级向上查找。"""
        if getattr(sys, 'frozen', False):
            base = os.path.dirname(sys.executable)
        else:
            base = os.path.dirname(os.path.abspath(__file__))
        current = base
        for _ in range(4):
            if os.path.exists(os.path.join(current, "work.json")) or \
               os.path.exists(os.path.join(current, "file.json")):
                return current
            parent = os.path.dirname(current)
            if parent == current:
                break
            current = parent
        return base

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
        
        # 发布按钮（发布 / 更新 GitHub Releases 工具）
        self.btn_release = QPushButton("📦")
        self.btn_release.setFixedSize(35, 35)
        self.btn_release.setToolTip("发布管理器：上传 zip 并发布/更新 GitHub Releases")
        self.btn_release.clicked.connect(self.open_release_dialog)

        self.toolbar.addWidget(self.btn_clear)
        self.toolbar.addWidget(self.btn_add)
        self.toolbar.addWidget(self.btn_scan)
        self.toolbar.addWidget(self.btn_release)
        
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
        """添加文件或文件夹：先选择类型，再打开对应对话框"""
        box = QMessageBox(self)
        box.setWindowTitle("添加")
        box.setText("要添加什么？")
        btn_files = box.addButton("文件", QMessageBox.AcceptRole)
        btn_dir = box.addButton("文件夹", QMessageBox.AcceptRole)
        box.addButton("取消", QMessageBox.RejectRole)
        box.exec_()

        clicked = box.clickedButton()
        if clicked is btn_files:
            paths = QFileDialog.getOpenFileNames(self, "选择文件")[0]
        elif clicked is btn_dir:
            directory = QFileDialog.getExistingDirectory(self, "选择文件夹")
            paths = [directory] if directory else []
        else:
            return

        if not paths:
            return

        for path in paths:
            self.add_file_to_json(path)
        self.save_json()
        self.update_tree()
    
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
            # 先记录外部直链节点（如 GitHub Releases 地址），扫描后合并回去，避免被磁盘扫描覆盖
            externals = []
            self.collect_external_nodes(self.data, None, externals)
            self.data["projects"] = []
            self.scan_directory_full(target_dir, self.data["projects"])
            self.restore_external_nodes(externals)
            self.save_json()
            self.update_tree()

    def collect_external_nodes(self, node, parent_path, out):
        """收集 path 为 http(s) 直链的节点，连同其父目录路径一起记录"""
        path = node.get("path") or ""
        if path.startswith("http://") or path.startswith("https://"):
            out.append((parent_path, dict(node)))
            return
        for child in node.get("projects") or []:
            self.collect_external_nodes(child, path, out)

    def restore_external_nodes(self, externals):
        """把外部直链节点合并回扫描结果，父目录缺失时按 path 规则补建"""
        for parent_path, node in externals:
            if not parent_path:
                continue
            folder = self.ensure_folder_node(self.data, parent_path)
            if folder is None:
                continue
            children = folder.setdefault("projects", [])
            children[:] = [c for c in children if c.get("name") != node.get("name")]
            children.append(node)

    def ensure_folder_node(self, root, folder_path):
        """按 path 在树中定位目录节点，缺失的中间目录自动补建"""
        root_path = (root.get("path") or "").rstrip("/")
        if folder_path.rstrip("/") == root_path:
            return root
        prefix = root_path + "/"
        if not folder_path.startswith(prefix):
            return None
        node = root
        for part in [p for p in folder_path[len(prefix):].split("/") if p]:
            child = None
            for c in node.get("projects") or []:
                if c.get("name") == part and c.get("type") in ("folder", "index"):
                    child = c
                    break
            if child is None:
                child = {
                    "name": part,
                    "path": (node.get("path") or "").rstrip("/") + "/" + part,
                    "type": "folder",
                    "projects": []
                }
                node.setdefault("projects", []).append(child)
            node = child
        return node
    
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
    
    # ------------------------------------------------------------------
    # GitHub Releases 发布（发布管理器）
    # ------------------------------------------------------------------
    def open_release_dialog(self):
        dialog = ReleaseDialog(self, self)
        dialog.exec_()

    def token_path(self):
        return os.path.join(self.json_dir, TOKEN_FILE_NAME)

    def resolve_token(self):
        """Token 来源：环境变量 GH_TOKEN / GITHUB_TOKEN → 本地 .pathmanager_token。

        返回 (token, 来源说明)，都没配置时返回 (None, None)。
        """
        for key in ("GH_TOKEN", "GITHUB_TOKEN"):
            value = (os.environ.get(key) or "").strip()
            if value:
                return value, f"环境变量 {key}"
        path = self.token_path()
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8") as handle:
                    value = handle.read().strip()
            except OSError:
                value = ""
            if value:
                return value, TOKEN_FILE_NAME
        return None, None

    def save_token(self, token):
        with open(self.token_path(), "w", encoding="utf-8") as handle:
            handle.write(token.strip())

    def load_index_file(self, json_name):
        path = os.path.join(self.json_dir, json_name)
        if not os.path.exists(path):
            return None
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)

    def save_index_file(self, json_name, data):
        path = os.path.join(self.json_dir, json_name)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(data, handle, ensure_ascii=False, indent=2)

    def refresh_view(self, json_name):
        """索引被外部改写后刷新界面（供发布线程结束回主线程时调用）。"""
        if json_name == self.current_json:
            self.load_json()

    @staticmethod
    def index_name_for_folder(folder_path):
        parts = [part for part in (folder_path or "").split("/") if part]
        return "work.json" if "Work" in parts else "file.json"

    def add_release_entry(self, folder_path, name, url):
        """把 Releases 直链条目写入对应索引，返回被写入的索引文件名。

        与扫描结果的格式保持一致：type 固定为 file、projects 为空、按名称排序插入；
        同名条目已存在时只刷新其 path，保证链接始终指向最新版。
        """
        json_name = self.index_name_for_folder(folder_path)
        data = self.load_index_file(json_name)
        if data is None:
            raise ReleaseError(f"找不到索引文件 {json_name}")
        folder = self.ensure_folder_node(data, folder_path)
        if folder is None:
            raise ReleaseError(f"{json_name} 中无法定位目录 {folder_path}")

        entry = {"name": name, "path": url, "type": "file", "projects": []}
        children = folder.setdefault("projects", [])
        for index, child in enumerate(children):
            if child.get("name") == name:
                children[index] = entry
                self.save_index_file(json_name, data)
                return json_name

        key = name.lower()
        position = len(children)
        for index, child in enumerate(children):
            if (child.get("name") or "").lower() > key:
                position = index
                break
        children.insert(position, entry)
        self.save_index_file(json_name, data)
        return json_name

    def collect_release_entries(self):
        """扫描 file.json / work.json，收集所有 Releases 直链条目。"""
        entries = []
        for json_name in ("file.json", "work.json"):
            data = self.load_index_file(json_name)
            if data:
                self._collect_release_entries(data, json_name, None, entries)
        return entries

    def _collect_release_entries(self, node, json_name, folder_path, out):
        parsed = parse_release_download_url(node.get("path"))
        if parsed:
            out.append({
                "json_name": json_name,
                "folder_path": folder_path,
                "name": node.get("name"),
                "tag": parsed[0],
                "asset_name": parsed[1],
                "url": node.get("path"),
            })
        for child in node.get("projects") or []:
            self._collect_release_entries(child, json_name, node.get("path"), out)

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
                # 在同一父节点下（parent 为 None 表示顶层节点）
                items = []
                if parent is None:
                    for i in range(self.tree_widget.topLevelItemCount()):
                        items.append(self.tree_widget.topLevelItem(i))
                else:
                    for i in range(parent.childCount()):
                        items.append(parent.child(i))
                
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
        count = 0
        for item in self.selected_items[:]:
            parent = item.parent()
            if parent:
                item_data = item.data(0, Qt.UserRole)
                self.remove_from_data(self.data, item_data["path"])
                parent.removeChild(item)
                self.selected_items.remove(item)
                count += 1
        
        self.save_json()
        print(f"已保存 JSON，删除了 {count} 个项目")
    
    def remove_from_data(self, data, target_path):
        """递归从 data 中删除指定 path 的项目（path 在树中唯一）"""
        if "projects" in data:
            data["projects"] = [p for p in data["projects"] if p.get("path") != target_path]
            for project in data["projects"]:
                if "projects" in project:
                    self.remove_from_data(project, target_path)
    
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
        paths_to_remove = [item.data(0, Qt.UserRole)["path"] for item in self.selected_items]
        for path in paths_to_remove:
            self.remove_from_data(self.data, path)
        
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
        
        # 添加子项目（递归展开多级结构）
        if merged_projects:
            self.add_children(index_item, merged_projects)
        
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
        self.remove_from_data(self.data, dragged_item_data["path"])
        
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
    
    def is_child_of(self, child_data, parent_data):
        """检查 child_data 是否是 parent_data 的子节点"""
        if child_data == parent_data:
            return True
        if "projects" in parent_data:
            for project in parent_data["projects"]:
                if self.is_child_of(child_data, project):
                    return True
        return False

class ReleaseDialog(QDialog):
    """发布管理器：新建工具 / 更新工具版本，并自动维护索引里的 Path。"""

    MODE_NEW = 0
    MODE_UPDATE = 1

    def __init__(self, manager, parent=None):
        super().__init__(parent)
        self.manager = manager
        self.job_thread = None
        self.existing_entries = []
        self.auto_asset_name = ""
        self.setWindowTitle("发布管理器 · GitHub Releases")
        self.setMinimumWidth(640)
        self.build_ui()
        self.reload_token_status()
        self.reload_existing_tools()
        self.on_mode_changed(self.MODE_NEW)

    # ---------------- 界面 ----------------
    def build_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(8)

        top = QFormLayout()
        self.combo_mode = QComboBox()
        self.combo_mode.addItems(["新建工具", "更新工具"])
        self.combo_mode.currentIndexChanged.connect(self.on_mode_changed)
        top.addRow("操作", self.combo_mode)

        file_row = QHBoxLayout()
        self.edit_file = QLineEdit()
        self.edit_file.setPlaceholderText("选择要发布的压缩包")
        self.edit_file.textChanged.connect(self.on_file_changed)
        btn_browse = QPushButton("浏览…")
        btn_browse.clicked.connect(self.browse_file)
        file_row.addWidget(self.edit_file)
        file_row.addWidget(btn_browse)
        top.addRow("压缩包文件", file_row)
        layout.addLayout(top)

        self.group_new = QGroupBox("新建工具")
        form_new = QFormLayout(self.group_new)
        self.combo_category = QComboBox()
        for folder, tag, title in RELEASE_CATEGORIES:
            self.combo_category.addItem(f"{folder}   →   Release「{tag}」", (folder, tag, title))
        self.combo_category.currentIndexChanged.connect(self.refresh_preview)
        form_new.addRow("目标分类", self.combo_category)
        self.edit_asset_name = QLineEdit()
        self.edit_asset_name.setPlaceholderText("资产名，默认取压缩包文件名并长期保持不变")
        self.edit_asset_name.textChanged.connect(self.refresh_preview)
        form_new.addRow("资产名", self.edit_asset_name)
        layout.addWidget(self.group_new)

        self.group_update = QGroupBox("更新工具")
        form_update = QFormLayout(self.group_update)
        self.combo_tool = QComboBox()
        self.combo_tool.currentIndexChanged.connect(self.refresh_preview)
        form_update.addRow("已有工具", self.combo_tool)
        self.label_current = QLabel("—")
        self.label_current.setWordWrap(True)
        form_update.addRow("当前链接", self.label_current)
        layout.addWidget(self.group_update)

        preview_box = QGroupBox("发布后下载直链")
        preview_layout = QVBoxLayout(preview_box)
        self.label_preview = QLabel()
        self.label_preview.setWordWrap(True)
        self.label_preview.setTextInteractionFlags(Qt.TextSelectableByMouse)
        preview_layout.addWidget(self.label_preview)
        layout.addWidget(preview_box)

        token_row = QHBoxLayout()
        self.label_token = QLabel()
        btn_token = QPushButton("设置 Token…")
        btn_token.clicked.connect(self.edit_token)
        token_row.addWidget(self.label_token, 1)
        token_row.addWidget(btn_token)
        layout.addLayout(token_row)

        self.progress = QProgressBar()
        self.progress.setRange(0, 0)
        self.progress.setVisible(False)
        layout.addWidget(self.progress)

        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMinimumHeight(150)
        self.log_view.setPlaceholderText("发布日志…")
        layout.addWidget(self.log_view)

        buttons = QDialogButtonBox()
        self.btn_publish = buttons.addButton("发布", QDialogButtonBox.ActionRole)
        self.btn_publish.clicked.connect(self.publish)
        btn_close = buttons.addButton("关闭", QDialogButtonBox.RejectRole)
        btn_close.clicked.connect(self.request_close)
        layout.addWidget(buttons)

    # ---------------- 状态 ----------------
    def append_log(self, message):
        self.log_view.appendPlainText(message)

    def reload_token_status(self):
        token, source = self.manager.resolve_token()
        if token:
            self.label_token.setText(f"Token：已就绪 · 来源 {source}")
        else:
            self.label_token.setText(
                f"Token：未设置 · 可设环境变量 GH_TOKEN，或点右侧保存到 {TOKEN_FILE_NAME}")

    def reload_existing_tools(self):
        self.existing_entries = self.manager.collect_release_entries()
        self.combo_tool.clear()
        for entry in self.existing_entries:
            self.combo_tool.addItem(
                f"{entry['name']}   →   Release「{entry['tag']}」 · {entry['json_name']}", entry)
        if not self.existing_entries:
            self.combo_tool.addItem("索引中还没有 Releases 直链条目", None)

    def selected_entry(self):
        return self.combo_tool.currentData()

    def selected_category(self):
        return self.combo_category.currentData()

    def set_busy(self, busy):
        self.progress.setVisible(busy)
        for widget in (self.btn_publish, self.combo_mode, self.combo_category,
                       self.combo_tool, self.edit_file, self.edit_asset_name):
            widget.setEnabled(not busy)

    # ---------------- 交互 ----------------
    def on_mode_changed(self, index):
        is_new = index == self.MODE_NEW
        self.group_new.setVisible(is_new)
        self.group_update.setVisible(not is_new)
        self.refresh_preview()

    def browse_file(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "选择要发布的压缩包", "", "*.zip *.7z *.tar.gz;;*")
        if path:
            self.edit_file.setText(path)

    def on_file_changed(self, text):
        if self.combo_mode.currentIndex() == self.MODE_NEW:
            current = self.edit_asset_name.text().strip()
            if not current or current == self.auto_asset_name:
                self.auto_asset_name = sanitize_asset_name(text) if text else ""
                self.edit_asset_name.setText(self.auto_asset_name)
        self.refresh_preview()

    def refresh_preview(self):
        if not hasattr(self, "label_preview"):
            return
        if self.combo_mode.currentIndex() == self.MODE_UPDATE:
            entry = self.selected_entry()
            if entry:
                self.label_current.setText(entry["url"])
                self.label_preview.setText(
                    f"{entry['url']}\n\n更新后链接保持不变，只把内容替换为所选压缩包。")
            else:
                self.label_current.setText("—")
                self.label_preview.setText("索引中还没有可更新的 Releases 条目。")
            return
        category = self.selected_category()
        asset_name = self.edit_asset_name.text().strip()
        if category and asset_name:
            folder, tag, _title = category
            target_json = self.manager.index_name_for_folder(folder)
            self.label_preview.setText(
                f"{release_download_url(tag, asset_name)}\n\n"
                f"会写入 {target_json} 的 {folder} 目录，type=file。")
        else:
            self.label_preview.setText("选择压缩包后会按分类与资产名生成链接。")

    def edit_token(self):
        token, _source = self.manager.resolve_token()
        text, ok = QInputDialog.getText(
            self, "设置 GitHub Token",
            "粘贴具有本仓库 Contents: read/write 权限的 PAT：\n"
            f"保存到 {self.manager.token_path()}，该文件已在 .gitignore 中",
            QLineEdit.Password, token or "")
        if not ok:
            return
        text = text.strip()
        if not text:
            QMessageBox.warning(self, "设置 Token", "Token 不能为空。")
            return
        try:
            self.manager.save_token(text)
        except OSError as exc:
            QMessageBox.critical(self, "设置 Token", f"保存失败：{exc}")
            return
        self.reload_token_status()
        self.append_log(f"Token 已保存到 {self.manager.token_path()}")

    # ---------------- 发布 ----------------
    def publish(self):
        if self.job_thread is not None and self.job_thread.isRunning():
            return
        token, _source = self.manager.resolve_token()
        if not token:
            QMessageBox.warning(
                self, "缺少 Token",
                "未找到 GitHub Token。\n\n请点「设置 Token…」填写具有 Contents: read/write "
                "权限的 PAT，或设置环境变量 GH_TOKEN。")
            return
        file_path = self.edit_file.text().strip()
        if not file_path or not os.path.isfile(file_path):
            QMessageBox.warning(self, "缺少文件", "请选择要发布的压缩包文件。")
            return

        if self.combo_mode.currentIndex() == self.MODE_NEW:
            category = self.selected_category()
            asset_name = sanitize_asset_name(self.edit_asset_name.text())
            if not asset_name:
                QMessageBox.warning(self, "缺少资产名", "请填写资产名，发布后保持不变。")
                return
            folder_path, tag, _title = category
            json_name = self.manager.index_name_for_folder(folder_path)
            if self.manager.load_index_file(json_name) is None:
                QMessageBox.critical(self, "发布失败", f"找不到索引文件 {json_name}。")
                return

            def job(log):
                url = publish_new_tool(token, folder_path, asset_name, file_path, log)
                self.manager.add_release_entry(folder_path, asset_name, url)
                log(f"已写入索引 {json_name}：{folder_path}/{asset_name}")
                return {
                    "summary": f"新建工具 {asset_name} → Release「{tag}」",
                    "url": url,
                    "json_name": json_name,
                }
            summary = f"新建工具 {asset_name} → Release「{tag}」"
        else:
            entry = self.selected_entry()
            if not entry:
                QMessageBox.warning(self, "没有可更新的工具", "索引中还没有 Releases 直链条目。")
                return

            def job(log):
                url = publish_tool_update(
                    token, entry["tag"], entry["asset_name"], file_path, log)
                return {
                    "summary": f"更新工具 {entry['name']} · Release「{entry['tag']}」",
                    "url": url,
                    "json_name": entry["json_name"],
                }
            summary = f"更新工具 {entry['name']} · Release「{entry['tag']}」"

        self.set_busy(True)
        self.append_log(f"— {summary} —")
        self.job_thread = ReleaseJob(job, self)
        self.job_thread.progressed.connect(self.append_log)
        self.job_thread.succeeded.connect(self.on_publish_succeeded)
        self.job_thread.failed.connect(self.on_publish_failed)
        self.job_thread.start()

    def on_publish_succeeded(self, result):
        self.set_busy(False)
        self.reload_existing_tools()
        self.manager.refresh_view(result.get("json_name"))
        QMessageBox.information(
            self, "发布成功", f"{result['summary']}\n\n{result['url']}")

    def on_publish_failed(self, message):
        self.set_busy(False)
        self.append_log(f"失败：{message}")
        QMessageBox.critical(self, "发布失败", message)

    def request_close(self):
        if self.job_thread is not None and self.job_thread.isRunning():
            QMessageBox.information(self, "发布进行中", "请等待当前发布完成后再关闭。")
            return
        self.reject()

    def closeEvent(self, event):
        if self.job_thread is not None and self.job_thread.isRunning():
            QMessageBox.information(self, "发布进行中", "请等待当前发布完成后再关闭。")
            event.ignore()
            return
        super().closeEvent(event)


if __name__ == "__main__":
    repo_dir = None
    args = sys.argv[1:]
    if "--repo" in args:
        index = args.index("--repo")
        if index + 1 < len(args):
            repo_dir = args[index + 1]
    app = QApplication(sys.argv)
    window = PathManager(repo_dir=repo_dir)
    window.show()
    sys.exit(app.exec_())
