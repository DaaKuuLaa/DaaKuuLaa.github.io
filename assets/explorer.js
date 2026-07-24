(function () {
    'use strict';

    const CFG = window.EXPLORER_CONFIG || { json: 'file.json', root: 'File', title: '文件管理器' };
    const DOWNLOAD_TIMEOUT = 30000;

    let currentPath = [];
    let fileData = {};

    // ====== 本地缓存（仅缓存 JSON 内容与时间戳；不缓存密码或任何敏感数据）======
    const CACHE_KEY = 'explorer_cache:' + CFG.json;

    function readCache() {
        try {
            const raw = localStorage.getItem(CACHE_KEY);
            if (!raw) return null;
            const obj = JSON.parse(raw);
            if (!obj || !obj.data) return null;
            return obj; // { data, ts }
        } catch (e) { return null; }
    }

    function writeCache(data) {
        try {
            localStorage.setItem(CACHE_KEY, JSON.stringify({ data, ts: Date.now() }));
        } catch (e) { /* 配额或禁用 → 静默降级 */ }
    }

    function clearCache() {
        try { localStorage.removeItem(CACHE_KEY); } catch (e) { }
    }

    // 用一份完整的索引对象替换 fileData，写缓存，并保留 currentPath 渲染
    function applyIndex(index) {
        if (!index || typeof index !== 'object') return;
        fileData = index;
        writeCache(fileData);
        // 校验 currentPath 在新数据里仍合法；不合法回退到根
        let n = fileData;
        for (const seg of currentPath) {
            const f = (n.projects || []).find(p => p.name === seg);
            if (!f) { currentPath = []; break; }
            n = f;
        }
        updateNavPath();
        renderItems();
    }

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
        // R 键刷新（仅单独的 R，不拦截 Ctrl+R 等浏览器原生刷新）
        if ((e.key === 'r' || e.key === 'R') && !e.ctrlKey && !e.metaKey && !e.altKey && !e.target.matches('input, textarea')) {
            handleRefreshKey();
        }
    });

    // 连续按 R 三次 → 强制从远端拉取，绕过缓存
    let rPressCount = 0;
    let rPressTimer = null;
    function handleRefreshKey() {
        rPressCount++;
        if (rPressTimer) clearTimeout(rPressTimer);
        rPressTimer = setTimeout(() => { rPressCount = 0; }, 600);
        if (rPressCount >= 3) {
            rPressCount = 0;
            clearCache();
            loadFileData({ force: true });
            showRefreshButtonSpin();
            return;
        }
        doRefresh();
    }
    function showRefreshButtonSpin() {
        const btn = document.getElementById('refresh-button');
        if (!btn) return;
        btn.classList.add('spinning');
        setTimeout(() => btn.classList.remove('spinning'), 600);
    }
    function doRefresh() {
        // 保留 currentPath；loadFileData 自己会用 applyIndex 校验路径合法性
        loadFileData();
        showRefreshButtonSpin();
    }
    window.doRefresh = doRefresh;

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
            // 右键菜单依赖这三个 dataset
            fileItem.dataset.filePath = item.path || '';
            fileItem.dataset.fileName = item.name || '';
            fileItem.dataset.fileType = item.type || '';

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

    async function loadFileData(opts) {
        const forceFromRemote = !!(opts && opts.force);

        // 1) 先尝试缓存立刻渲染，避免空白页
        if (!forceFromRemote) {
            const cached = readCache();
            if (cached && cached.data) {
                fileData = cached.data;
                updateNavPath();
                renderItems();
            }
        }

        // 2) 后台从远端拉取并对比；远端拉取失败时保留缓存数据
        try {
            const response = await fetch(CFG.json, { cache: 'no-cache' });
            if (!response.ok) {
                if (!fileData || !fileData.projects) {
                    throw new Error('无法加载 ' + CFG.json);
                }
                return; // 缓存还能用，远端暂时败了
            }
            const remote = await response.json();
            // 远端数据有效 → 覆盖缓存并渲染
            applyIndex(remote);
        } catch (error) {
            if (!forceFromRemote) return; // 缓存兜底
            // 强制模式下且无缓存 → 兜底空结构
            if (!fileData || !fileData.projects) {
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

    // targetItem: null → 空白区右键；非 null → 在某个 file-item 上右键
    function showContextMenu(x, y, targetItem) {
        // 关闭已有菜单、解绑旧监听
        if (ctxMenuCleanup) ctxMenuCleanup();
        document.querySelectorAll('.context-menu').forEach(m => m.remove());

        const menu = document.createElement('div');
        menu.className = 'context-menu';
        menu.setAttribute('role', 'menu');
        menu.style.left = x + 'px';
        menu.style.top = y + 'px';

        function addMenuItem(icon, label, action) {
            const mi = document.createElement('div');
            mi.className = 'context-menu-item';
            mi.setAttribute('role', 'menuitem');
            mi.setAttribute('tabindex', '0');
            mi.innerHTML = '<span aria-hidden="true">' + icon + '</span>' + label;
            mi.onclick = () => { if (ctxMenuCleanup) ctxMenuCleanup(); action(); };
            mi.onkeydown = (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); if (ctxMenuCleanup) ctxMenuCleanup(); action(); } };
            menu.appendChild(mi);
        }

        function addMenuSeparator() {
            const sep = document.createElement('div');
            sep.className = 'context-menu-separator';
            sep.setAttribute('aria-hidden', 'true');
            menu.appendChild(sep);
        }

        // 如果在文件项上右键 → 加下载与删除
        if (targetItem) {
            const filePath = targetItem.dataset.filePath || '';
            const fileName = (targetItem.dataset.fileName || filePath.split('/').pop() || '');

            // 下载（仅非 folder/index 的文件有下载按钮，逻辑一致）
            const itemType = targetItem.dataset.fileType || '';
            if (itemType !== 'folder' && itemType !== 'index' && filePath) {
                addMenuItem('⬇️', '下载', () => {
                    openDownloadFromMenu(filePath);
                });
            }

            if (filePath) {
                addMenuItem('🗑️', '删除', () => {
                    openDeleteDialog(filePath, fileName);
                });
            }

            addMenuSeparator();
        }

        // 总是有”添加文件“
        addMenuItem('➕', '添加文件', () => { openUploadDialog(); });

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
            // 如果在 .file-list 内右键，重新弹出菜单（重新判断 file-item）
            if (document.getElementById('file-list').contains(ev.target)) {
                const fi = ev.target.closest('.file-item');
                setTimeout(() => showContextMenu(ev.clientX, ev.clientY, fi || null), 0);
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

    // 右键菜单里的下载入口（复用现有 downloadFromUrl / generateUrls / tryDownload 逻辑）
    async function openDownloadFromMenu(filePath) {
        const fileName = filePath.split('/').pop();
        const urls = generateUrls(filePath);

        const tryDownload = async (urlList) => {
            for (const url of urlList) {
                try { await downloadFromUrl(url, fileName); return true; } catch (e) { }
            }
            return false;
        };
        const tryOpen = async (urlList) => {
            for (const url of urlList) {
                try { window.open(url, '_blank', 'noopener'); return true; } catch (e) { }
            }
            return false;
        };

        // 短暂 loading 反馈用 banner
        showErrorBanner('⏳ 正在尝试下载…');
        try {
            const isLFS = await isLFSFile(urls.direct);
            if (isLFS) {
                if (await tryDownload([urls.proxy])) { hideErrorBanner(); return; }
                if (await tryOpen([urls.proxy])) { hideErrorBanner(); return; }
                showErrorBanner('下载失败，请稍后重试');
                return;
            }
            const [directResult, proxyResult] = await Promise.all([
                testSpeed(urls.direct, 3000),
                testSpeed(urls.proxy, 3000)
            ]);
            let primaryUrl = urls.direct, fallbackUrl = urls.proxy;
            if (directResult.success && proxyResult.success && proxyResult.time < directResult.time) {
                primaryUrl = urls.proxy; fallbackUrl = urls.direct;
            } else if (!directResult.success && proxyResult.success) {
                primaryUrl = urls.proxy; fallbackUrl = urls.direct;
            }
            if (await tryDownload([primaryUrl, fallbackUrl])) { hideErrorBanner(); return; }
            if (await tryOpen([primaryUrl, fallbackUrl])) { hideErrorBanner(); return; }
            showErrorBanner('下载失败，请稍后重试');
        } catch (e) {
            showErrorBanner('下载失败，请稍后重试');
        }
    }

    // ====== 删除对话框 ======
    function openDeleteDialog(filePath, fileName) {
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
        overlay.setAttribute('aria-label', '删除文件');

        const dialog = document.createElement('div');
        dialog.className = 'modal-dialog';

        // header
        const header = document.createElement('div');
        header.className = 'modal-header';
        header.innerHTML = '<span aria-hidden="true">🗑️</span> 删除文件';
        dialog.appendChild(header);

        // body
        const body = document.createElement('div');
        body.className = 'modal-body';

        // 待删除文件名称展示
        const nameRow = document.createElement('div');
        nameRow.className = 'form-row';
        nameRow.innerHTML = '<label>文件</label>';
        const nameBox = document.createElement('div');
        nameBox.className = 'file-path';
        nameBox.textContent = fileName;
        nameBox.style.fontWeight = '600';
        nameBox.style.color = '#c53030';
        nameRow.appendChild(nameBox);
        body.appendChild(nameRow);

        // 密码
        const pwdRow = document.createElement('div');
        pwdRow.className = 'form-row';
        pwdRow.innerHTML = '<label for="del-pwd">密码</label>';
        const pwdInput = document.createElement('input');
        pwdInput.type = 'password';
        pwdInput.id = 'del-pwd';
        pwdInput.autocomplete = 'off';
        pwdRow.appendChild(pwdInput);
        body.appendChild(pwdRow);

        const status = document.createElement('div');
        status.className = 'modal-status';
        body.appendChild(status);

        dialog.appendChild(body);

        // footer (单按钮 → 弹出二次确认)
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
        okBtn.textContent = '下一步';
        okBtn.onclick = onVerify;
        footer.appendChild(cancelBtn);
        footer.appendChild(okBtn);
        dialog.appendChild(footer);

        overlay.appendChild(dialog);
        overlay.onclick = (e) => { if (e.target === overlay) closeModal(); };
        document.body.appendChild(overlay);

        document.addEventListener('keydown', escapeDialog, true);
        pwdInput.focus();

        let verifyOk = false;

        function setStatus(msg, kind) {
            status.textContent = msg || '';
            status.className = 'modal-status' + (kind ? ' ' + kind : '');
        }

        async function onVerify() {
            if (verifyOk) return; // 预防双击
            const pwd = pwdInput.value;
            if (!pwd) { setStatus('请输入密码', 'err'); pwdInput.focus(); return; }

            okBtn.disabled = true;
            cancelBtn.disabled = true;
            setStatus('正在验证密码…');

            // 用 /upload 的 password hash 做轻量级密码校验；发一个空校验给 Worker
            try {
                const res = await fetch(UPLOAD_WORKER_URL + '/delete', {
                    method: 'POST',
                    headers: {
                        'Authorization': pwd,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ path: filePath, confirm: false })
                });
                const data = await res.json().catch(() => ({}));
                // 如果 Worker 返回 { needConfirm: true, error: '需要二次确认' } 说明密码是对的
                // 如果 Worker 返回 401 说明密码错
                if (data && data.needConfirm) {
                    // 密码正确 → 进入二次确认脚本
                    verifyOk = true;
                    showConfirmStep();
                } else if (res.status === 401 || (data && data.error && /密码/.test(data.error))) {
                    setStatus('密码错误', 'err');
                } else if (data && data.error) {
                    setStatus(data.error, 'err');
                } else {
                    setStatus('验证失败 (' + (res.status || '') + ')', 'err');
                }
            } catch (err) {
                setStatus('网络错误：' + err.message, 'err');
            } finally {
                okBtn.disabled = false;
                cancelBtn.disabled = false;
            }
        }

        function showConfirmStep() {
            // 清 body 旧内容，显示二次确认
            body.innerHTML = '';
            const warnDiv = document.createElement('div');
            warnDiv.style.cssText = 'font-size:15px;color:#c53030;text-align:center;padding:9px 12px;border-radius:8px;background:rgba(229,62,62,0.08);margin-bottom:14px;';
            warnDiv.textContent = '⚠️ 此操作不可撤销！\n确认删除 「' + fileName + '」？';
            body.appendChild(warnDiv);

            const status2 = document.createElement('div');
            status2.className = 'modal-status';
            body.appendChild(status2);

            // 替换 footer 按钮为“确认删除”+ 取消
            footer.innerHTML = '';
            const cancel2 = document.createElement('button');
            cancel2.type = 'button';
            cancel2.className = 'btn';
            cancel2.textContent = '取消';
            cancel2.onclick = closeModal;
            const confirmBtn = document.createElement('button');
            confirmBtn.type = 'button';
            confirmBtn.className = 'btn btn-primary';
            confirmBtn.style.background = '#e53e3e';
            confirmBtn.style.borderColor = '#e53e3e';
            confirmBtn.textContent = '确认删除';
            confirmBtn.onclick = () => doDelete(status2, confirmBtn, cancel2, filePath, pwdInput.value);
            footer.appendChild(cancel2);
            footer.appendChild(confirmBtn);
        }

        async function doDelete(st, ok, cancel, path, pwd) {
            ok.disabled = true;
            cancel.disabled = true;
            st.textContent = '正在删除…';
            st.className = 'modal-status';

            try {
                const res = await fetch(UPLOAD_WORKER_URL + '/delete', {
                    method: 'POST',
                    headers: {
                        'Authorization': pwd,
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ path, confirm: true })
                });
                const data = await res.json().catch(() => ({}));
                if (res.ok && data.ok) {
                    st.textContent = '删除成功';
                    st.className = 'modal-status ok';
                    // 用 Worker 返回的最新 JSON 直接渲染
                    if (data.index && !data.partial) {
                        applyIndex(data.index);
                    }
                    setTimeout(() => { closeModal(); if (data.partial || !data.index) { doRefresh(fileName); } }, 900);
                } else {
                    const detail = (data && data.ghData && data.ghData.message) || (data && data.error) || '';
                    st.textContent = '删除失败: ' + (detail || ('HTTP ' + (res.status || '')));
                    st.className = 'modal-status err';
                }
            } catch (err) {
                st.textContent = '网络错误：' + err.message;
                st.className = 'modal-status err';
            } finally {
                ok.disabled = false;
                cancel.disabled = false;
            }
        }

        function closeModal() {
            if (!modalOpen) return;
            modalOpen = false;
            document.removeEventListener('keydown', escapeDialog, true);
            document.body.removeChild(overlay);
        }
    }

    // 在文件列表区域启用右键菜单（脚本在 body 末尾运行，#file-list 已存在）
    (function initContextMenu() {
        const fileList = document.getElementById('file-list');
        if (fileList) {
            fileList.addEventListener('contextmenu', function (e) {
                e.preventDefault();
                const fileItem = e.target.closest('.file-item');
                showContextMenu(e.clientX, e.clientY, fileItem || null);
            });
        }
    })();

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
                    // 用 Worker 返回的最新 JSON 直接渲染，避免等 GitHub 镜像延迟
                    if (data.index && !data.partial) {
                        applyIndex(data.index);
                    }
                    setTimeout(() => { closeModal(); if (data.partial || !data.index) { doRefresh(); } }, 900);
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
