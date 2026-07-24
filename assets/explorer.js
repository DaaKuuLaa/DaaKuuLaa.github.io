(function () {
    'use strict';

    const CFG = window.EXPLORER_CONFIG || { json: 'file.json', root: 'File', title: '文件管理器' };
    const DOWNLOAD_TIMEOUT = 30000;

    let currentPath = [];
    let fileData = {};

    const root = document.documentElement;

    function toggleTheme() {
        root.classList.toggle('dark-mode');
        const isDark = root.classList.contains('dark-mode');
        const toggle = document.querySelector('.theme-toggle');
        if (toggle) toggle.textContent = isDark ? '黑暗模式' : '明亮模式';
        try {
            localStorage.setItem('theme', isDark ? 'dark' : 'light');
        } catch (e) { }
    }

    function updateThemeToggleText() {
        const isDark = root.classList.contains('dark-mode');
        const toggle = document.querySelector('.theme-toggle');
        if (toggle) toggle.textContent = isDark ? '黑暗模式' : '明亮模式';
    }
    window.toggleTheme = toggleTheme;

    document.addEventListener('keydown', function (e) {
        if ((e.key === 'd' || e.key === 'D') && !e.target.matches('input, textarea')) {
            toggleTheme();
        }
        if (e.key === 'Escape') {
            window.location.href = 'index.html';
        }
        if (e.key === 'Backspace' && !e.target.matches('input, textarea')) {
            e.preventDefault();
            goBack();
        }
    });

    function sortItems(items) {
        return items.sort((a, b) => {
            const typeA = a.type === 'folder' ? 0 : (a.type === 'index' ? 1 : 2);
            const typeB = b.type === 'folder' ? 0 : (b.type === 'index' ? 1 : 2);
            if (typeA !== typeB) return typeA - typeB;
            return a.name.localeCompare(b.name, 'zh-CN');
        });
    }

    function updateNavPath() {
        const navPath = document.getElementById('nav-path');
        navPath.innerHTML = '';

        const rootItem = document.createElement('div');
        rootItem.className = 'nav-item';
        rootItem.textContent = CFG.root;
        rootItem.dataset.path = '';
        rootItem.tabIndex = 0;
        rootItem.setAttribute('role', 'button');
        rootItem.setAttribute('aria-label', '返回' + CFG.root + '根目录');
        rootItem.onclick = () => navigateTo([]);
        rootItem.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); navigateTo([]); } };
        navPath.appendChild(rootItem);

        let currentPathStr = '';
        currentPath.forEach((segment, index) => {
            const separator = document.createElement('span');
            separator.className = 'nav-separator';
            separator.setAttribute('aria-hidden', 'true');
            separator.textContent = '>';
            navPath.appendChild(separator);

            currentPathStr += segment + '/';
            const item = document.createElement('div');
            item.className = 'nav-item';
            item.textContent = segment;
            item.dataset.path = currentPath.slice(0, index + 1).join('/');
            item.tabIndex = 0;
            item.setAttribute('role', 'button');
            const target = currentPath.slice(0, index + 1);
            item.onclick = () => navigateTo(target);
            item.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); navigateTo(target); } };
            navPath.appendChild(item);
        });

        document.getElementById('back-button').style.display = currentPath.length > 0 ? 'flex' : 'none';
    }

    function getCurrentItems() {
        let items = fileData.projects;
        for (const segment of currentPath) {
            const found = items.find(item => item.name === segment);
            if (found && found.projects) {
                items = found.projects;
            } else {
                return [];
            }
        }
        return items;
    }

    function openItem(item) {
        if (item.type === 'folder') {
            navigateTo([...currentPath, item.name]);
            return;
        }
        let url = item.path;
        if (!url.startsWith('http://') && !url.startsWith('https://')) {
            url = 'https://' + url;
        }
        window.open(url, '_blank', 'noopener');
    }

    function renderItems() {
        const fileList = document.getElementById('file-list');
        const items = getCurrentItems();
        const sortedItems = sortItems([...items]);

        fileList.innerHTML = '';

        if (sortedItems.length === 0) {
            fileList.innerHTML = `
                <div class="empty-state" role="status">
                    <div class="icon" aria-hidden="true">📂</div>
                    <div class="title">空空如也</div>
                    <div class="subtitle">暂无文件或文件夹</div>
                </div>
            `;
            return;
        }

        sortedItems.forEach((item, index) => {
            const fileItem = document.createElement('div');
            fileItem.className = 'file-item';
            fileItem.style.animationDelay = (index * 0.1) + 's';
            fileItem.tabIndex = 0;
            fileItem.setAttribute('role', 'button');

            let icon = '📄';
            let typeText = '文件';

            if (item.type === 'folder') {
                icon = '📁';
                typeText = '文件夹';
            } else if (item.type === 'index') {
                icon = '🏠';
                typeText = '网页';
            }

            let downloadBtn = '';
            if (item.type !== 'folder' && item.type !== 'index') {
                downloadBtn = `<div class="download-btn" tabindex="0" role="button" aria-label="下载 ${item.name}" onclick="downloadFile(event, '${item.path}')">⬇️</div>`;
            }

            fileItem.setAttribute('aria-label', item.name + '，' + typeText);
            fileItem.innerHTML = `
                <div class="file-icon" aria-hidden="true">${icon}</div>
                <div class="file-info">
                    <div class="file-name">${item.name}</div>
                    <div class="file-type">${typeText}</div>
                </div>
                ${downloadBtn}
            `;

            let clickTimer = null;
            fileItem.addEventListener('click', (e) => {
                if (e.target.closest('.download-btn')) return;
                e.preventDefault();
                if (clickTimer) {
                    clearTimeout(clickTimer);
                    clickTimer = null;
                }
                if (item.type === 'folder') {
                    navigateTo([...currentPath, item.name]);
                } else if (item.type === 'index') {
                    clickTimer = setTimeout(() => openItem(item), 200);
                } else {
                    openItem(item);
                }
            });

            fileItem.addEventListener('dblclick', (e) => {
                if (e.target.closest('.download-btn')) return;
                e.preventDefault();
                if (clickTimer) {
                    clearTimeout(clickTimer);
                    clickTimer = null;
                }
                if (item.type === 'index') {
                    navigateTo([...currentPath, item.name]);
                }
            });

            fileItem.addEventListener('keydown', (e) => {
                if (e.target.closest('.download-btn')) return;
                if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    if (item.type === 'index') {
                        navigateTo([...currentPath, item.name]);
                    } else {
                        openItem(item);
                    }
                } else if (e.key === 'ArrowDown') {
                    e.preventDefault();
                    const next = fileList.querySelectorAll('.file-item')[index + 1];
                    if (next) next.focus();
                } else if (e.key === 'ArrowUp') {
                    e.preventDefault();
                    const prev = fileList.querySelectorAll('.file-item')[index - 1];
                    if (prev) prev.focus();
                }
            });

            fileList.appendChild(fileItem);
        });
    }

    function navigateTo(path) {
        currentPath = path;
        updateNavPath();
        renderItems();
    }

    function goBack() {
        if (currentPath.length > 0) {
            currentPath = currentPath.slice(0, -1);
            updateNavPath();
            renderItems();
        }
    }
    window.goBack = goBack;

    async function loadFileData() {
        try {
            const response = await fetch(CFG.json);
            if (!response.ok) {
                throw new Error('无法加载 ' + CFG.json);
            }
            fileData = await response.json();
            updateNavPath();
            renderItems();
        } catch (error) {
            fileData = {
                name: CFG.root,
                path: 'DaaKuuLaa.github.io/' + CFG.root,
                type: 'folder',
                projects: []
            };
            updateNavPath();
            renderItems();
        }
    }

    function showErrorBanner(message) {
        const banner = document.getElementById('error-banner');
        const msg = document.getElementById('error-message');
        msg.textContent = message;
        banner.classList.add('show');
        setTimeout(hideErrorBanner, 5000);
    }

    function hideErrorBanner() {
        document.getElementById('error-banner').classList.remove('show');
    }
    window.hideErrorBanner = hideErrorBanner;

    function generateUrls(path) {
        let directUrl = path;
        if (!directUrl.startsWith('http://') && !directUrl.startsWith('https://')) {
            directUrl = 'https://' + directUrl;
        }

        const domainPrefix = 'DaaKuuLaa.github.io/';
        let relativePath = path;
        if (relativePath.startsWith('http://')) {
            relativePath = relativePath.replace('http://', '');
        } else if (relativePath.startsWith('https://')) {
            relativePath = relativePath.replace('https://', '');
        }

        if (relativePath.startsWith(domainPrefix)) {
            relativePath = relativePath.substring(domainPrefix.length);
        }

        const githubBlobUrl = 'https://github.com/DaaKuuLaa/DaaKuuLaa.github.io/blob/main/' + relativePath;
        const proxyUrl = 'https://ghproxy.net/' + githubBlobUrl;

        return { direct: directUrl, proxy: proxyUrl };
    }

    async function isLFSFile(url) {
        const LFS_SIGNATURE = 'version https://git-lfs.github.com/spec/v1';
        try {
            const response = await fetch(url, {
                headers: { 'Range': 'bytes=0-200' },
                cache: 'no-cache'
            });
            if (!response.ok && response.status !== 206) {
                return false;
            }
            const blob = await response.blob();
            const text = await blob.text();
            return text.startsWith(LFS_SIGNATURE);
        } catch (error) {
            return false;
        }
    }

    function testSpeed(url, timeout) {
        return new Promise((resolve) => {
            const controller = new AbortController();
            const signal = controller.signal;
            const startTime = Date.now();
            const id = setTimeout(() => {
                controller.abort();
                resolve({ url, success: false, time: Infinity });
            }, timeout);

            fetch(url, { method: 'HEAD', signal, cache: 'no-cache' })
                .then(response => {
                    clearTimeout(id);
                    resolve({ url, success: response.ok, time: Date.now() - startTime });
                })
                .catch(() => {
                    clearTimeout(id);
                    resolve({ url, success: false, time: Infinity });
                });
        });
    }

    function downloadFromUrl(url, fileName) {
        return new Promise((resolve, reject) => {
            const controller = new AbortController();
            const timeoutId = setTimeout(() => {
                controller.abort();
                reject(new Error('下载超时'));
            }, DOWNLOAD_TIMEOUT);

            fetch(url, { signal: controller.signal, cache: 'no-cache' })
                .then(response => {
                    if (!response.ok) {
                        throw new Error('HTTP error! status: ' + response.status);
                    }
                    return response.blob();
                })
                .then(blob => {
                    clearTimeout(timeoutId);
                    const downloadUrl = window.URL.createObjectURL(blob);
                    const link = document.createElement('a');
                    link.href = downloadUrl;
                    link.download = fileName;
                    document.body.appendChild(link);
                    link.click();
                    setTimeout(() => {
                        document.body.removeChild(link);
                        window.URL.revokeObjectURL(downloadUrl);
                    }, 100);
                    resolve();
                })
                .catch(error => {
                    clearTimeout(timeoutId);
                    reject(error);
                });
        });
    }

    async function downloadFile(event, path) {
        event.stopPropagation();
        event.preventDefault();

        const target = event.currentTarget;
        const originalIcon = target.textContent;
        target.textContent = '⏳';
        target.style.pointerEvents = 'none';

        const fileName = path.split('/').pop();
        const urls = generateUrls(path);

        const tryDownload = async (urlList) => {
            for (const url of urlList) {
                try {
                    await downloadFromUrl(url, fileName);
                    return true;
                } catch (error) { }
            }
            return false;
        };

        const tryOpen = async (urlList) => {
            for (const url of urlList) {
                try {
                    window.open(url, '_blank', 'noopener');
                    return true;
                } catch (error) { }
            }
            return false;
        };

        try {
            const isLFS = await isLFSFile(urls.direct);
            if (isLFS) {
                if (await tryDownload([urls.proxy])) { resetBtn(); return; }
                if (await tryOpen([urls.proxy])) { resetBtn(); return; }
                showErrorBanner('下载失败，请稍后重试');
                resetBtn();
                return;
            }

            const [directResult, proxyResult] = await Promise.all([
                testSpeed(urls.direct, 3000),
                testSpeed(urls.proxy, 3000)
            ]);

            let primaryUrl, fallbackUrl;
            if (directResult.success && proxyResult.success) {
                if (directResult.time <= proxyResult.time) {
                    primaryUrl = urls.direct; fallbackUrl = urls.proxy;
                } else {
                    primaryUrl = urls.proxy; fallbackUrl = urls.direct;
                }
            } else if (directResult.success) {
                primaryUrl = urls.direct; fallbackUrl = urls.proxy;
            } else if (proxyResult.success) {
                primaryUrl = urls.proxy; fallbackUrl = urls.direct;
            } else {
                primaryUrl = urls.direct; fallbackUrl = urls.proxy;
            }

            if (await tryDownload([primaryUrl, fallbackUrl])) { resetBtn(); return; }
            if (await tryOpen([primaryUrl, fallbackUrl])) { resetBtn(); return; }
            showErrorBanner('下载失败，请稍后重试');
        } catch (error) {
            showErrorBanner('下载失败，请稍后重试');
        }

        function resetBtn() {
            target.textContent = originalIcon;
            target.style.pointerEvents = 'auto';
        }
        resetBtn();
    }
    window.downloadFile = downloadFile;

    // ====== 上传入口（右键菜单 + 模态框） ======
    const UPLOAD_WORKER_URL = CFG.uploadUrl || '';  // 通过 EXPLORER_CONFIG.uploadUrl 配置
    let modalOpen = false;

    function escapeDialog(e) {
        if (e.key === 'Escape' && modalOpen) {
            e.stopImmediatePropagation();
            e.preventDefault();
            closeModal();
        }
    }

    let ctxMenuCleanup = null;

    function showContextMenu(x, y) {
        // 关闭已有菜单、解绑旧监听
        if (ctxMenuCleanup) ctxMenuCleanup();
        document.querySelectorAll('.context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'context-menu';
        menu.setAttribute('role', 'menu');
        menu.style.left = x + 'px';
        menu.style.top = y + 'px';

        const item = document.createElement('div');
        item.className = 'context-menu-item';
        item.setAttribute('role', 'menuitem');
        item.setAttribute('tabindex', '0');
        item.innerHTML = '<span aria-hidden="true">➕</span>添加文件';
        item.onclick = () => { if (ctxMenuCleanup) ctxMenuCleanup(); openUploadDialog(); };
        item.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); if (ctxMenuCleanup) ctxMenuCleanup(); openUploadDialog(); } };
        menu.appendChild(item);

        document.body.appendChild(menu);
        // 防止超出视口
        const rect = menu.getBoundingClientRect();
        if (rect.right > window.innerWidth) menu.style.left = (window.innerWidth - rect.width - 8) + 'px';
        if (rect.bottom > window.innerHeight) menu.style.top = (window.innerHeight - rect.height - 8) + 'px';

        // 绑定关闭行为：任意点击 / 新右键
        function onMousedown(ev) {
            const insideMenu = menu.contains(ev.target) || ev.target.closest('.modal-overlay');
            if (!insideMenu) {
                cleanup();
            }
        }
        function onNewContextmenu(ev) {
            cleanup();
            // 如果在 .file-list 内右键，重新弹出菜单
            if (document.getElementById('file-list').contains(ev.target)) {
                setTimeout(() => showContextMenu(ev.clientX, ev.clientY), 0);
            }
        }
        function cleanup() {
            ctxMenuCleanup = null;
            document.removeEventListener('mousedown', onMousedown, true);
            document.removeEventListener('contextmenu', onNewContextmenu, true);
            document.querySelectorAll('.context-menu').forEach(m => m.remove());
        }
        ctxMenuCleanup = cleanup;

        setTimeout(() => {
            document.addEventListener('mousedown', onMousedown, true);
            document.addEventListener('contextmenu', onNewContextmenu, true);
        }, 0);
    }

    function openUploadDialog() {
        if (!UPLOAD_WORKER_URL) {
            showErrorBanner('未配置上传服务地址');
            return;
        }
        if (modalOpen) return;
        modalOpen = true;

        const overlay = document.createElement('div');
        overlay.className = 'modal-overlay';
        overlay.setAttribute('role', 'dialog');
        overlay.setAttribute('aria-modal', 'true');
        overlay.setAttribute('aria-label', '上传文件');

        const dialog = document.createElement('div');
        dialog.className = 'modal-dialog';

        // header
        const header = document.createElement('div');
        header.className = 'modal-header';
        header.innerHTML = '<span aria-hidden="true">📤</span> 上传文件';
        dialog.appendChild(header);

        // body
        const body = document.createElement('div');
        body.className = 'modal-body';

        const pwdRow = document.createElement('div');
        pwdRow.className = 'form-row';
        pwdRow.innerHTML = '<label for="up-pwd">密码</label>';
        const pwdInput = document.createElement('input');
        pwdInput.type = 'password';
        pwdInput.id = 'up-pwd';
        pwdInput.autocomplete = 'off';
        pwdRow.appendChild(pwdInput);
        body.appendChild(pwdRow);

        const fileRow = document.createElement('div');
        fileRow.className = 'form-row';
        fileRow.innerHTML = '<label>文件</label>';
        const pickRow = document.createElement('div');
        pickRow.className = 'file-pick-row';
        const pathBox = document.createElement('div');
        pathBox.className = 'file-path';
        pathBox.textContent = '未选择文件';
        const browseBtn = document.createElement('button');
        browseBtn.type = 'button';
        browseBtn.className = 'btn';
        browseBtn.textContent = '浏览';
        const fileInput = document.createElement('input');
        fileInput.type = 'file';
        fileInput.style.display = 'none';
        browseBtn.onclick = () => fileInput.click();
        fileInput.onchange = () => {
            if (fileInput.files && fileInput.files[0]) {
                pathBox.textContent = fileInput.files[0].name;
                pathBox.title = fileInput.files[0].name;
            } else {
                pathBox.textContent = '未选择文件';
            }
        };
        pickRow.appendChild(pathBox);
        pickRow.appendChild(browseBtn);
        pickRow.appendChild(fileInput);
        fileRow.appendChild(pickRow);
        body.appendChild(fileRow);

        const status = document.createElement('div');
        status.className = 'modal-status';
        body.appendChild(status);

        dialog.appendChild(body);

        // footer
        const footer = document.createElement('div');
        footer.className = 'modal-footer';
        const cancelBtn = document.createElement('button');
        cancelBtn.type = 'button';
        cancelBtn.className = 'btn';
        cancelBtn.textContent = '取消';
        cancelBtn.onclick = closeModal;
        const okBtn = document.createElement('button');
        okBtn.type = 'button';
        okBtn.className = 'btn btn-primary';
        okBtn.textContent = '确定';
        okBtn.onclick = doUpload;
        footer.appendChild(cancelBtn);
        footer.appendChild(okBtn);
        dialog.appendChild(footer);

        overlay.appendChild(dialog);
        overlay.onclick = (e) => { if (e.target === overlay) closeModal(); };
        document.body.appendChild(overlay);

        document.addEventListener('keydown', escapeDialog, true);

        function setStatus(msg, kind) {
            status.textContent = msg || '';
            status.className = 'modal-status' + (kind ? ' ' + kind : '');
        }

        async function doUpload() {
            const pwd = pwdInput.value;
            if (!pwd) { setStatus('请输入密码', 'err'); pwdInput.focus(); return; }
            if (!fileInput.files || !fileInput.files[0]) { setStatus('请选择文件', 'err'); return; }
            const file = fileInput.files[0];
            if (file.size > 50 * 1024 * 1024) { setStatus('文件超过 50MB 上限', 'err'); return; }

            okBtn.disabled = true;
            cancelBtn.disabled = true;
            setStatus('正在上传…');

            const form = new FormData();
            form.append('file', file);
            form.append('dir', CFG.root);
            form.append('subdirs', currentPath.join('/'));

            try {
                const res = await fetch(UPLOAD_WORKER_URL + '/upload', {
                    method: 'POST',
                    headers: { 'Authorization': pwd },
                    body: form
                });
                const data = await res.json().catch(() => ({}));
                if (res.ok && data.ok) {
                    setStatus(data.partial ? '已上传，索引稍后生效' : '上传成功', 'ok');
                    setTimeout(() => { closeModal(); loadFileData(); }, 900);
                } else {
                    const detail = data.detail || data.status || '';
                    setStatus('上传失败: ' + data.error + (detail ? ('（' + detail + '）') : ''), 'err');
                }
            } catch (err) {
                setStatus('网络错误：' + err.message, 'err');
            } finally {
                okBtn.disabled = false;
                cancelBtn.disabled = false;
            }
        }

        function closeModal() {
            if (!modalOpen) return;
            modalOpen = false;
            document.removeEventListener('keydown', escapeDialog, true);
            document.body.removeChild(overlay);
        }

        pwdInput.focus();
    }

    // 在文件列表区域启用右键菜单（脚本在 body 末尾运行，#file-list 已存在）
    (function initContextMenu() {
        const fileList = document.getElementById('file-list');
        if (fileList) {
            fileList.addEventListener('contextmenu', function (e) {
                e.preventDefault();
                showContextMenu(e.clientX, e.clientY);
            });
        }
    })();

    updateThemeToggleText();
    loadFileData();
})();
