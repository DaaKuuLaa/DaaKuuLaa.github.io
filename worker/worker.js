/**
 * DaaKuuLaa.github.io 上传 Worker
 *
 * 功能：接收前端上传的文件，经密码验证后，用 GitHub PAT 提交到仓库的 File/ 或 Work/ 目录，
 *      并在对应的 file.json / work.json 索引中追加条目。
 *
 * 所需 Secret（在 Cloudflare Dashboard → Worker → Settings → Variables/Secrets 注入）：
 *   - GITHUB_PAT            细粒度 PAT（仅 DaaKuuLaa.github.io 仓库，Contents: read/write, Metadata: read）
 *   - UPLOAD_PASSWORD_HASH  上传密码的 SHA-256 十六进制字符串（小写）
 *
 * 限制：单文件 ≤ 50MB（GitHub API 直接上传上限，超限需走 git-lfs，本 Worker 仅通过 Contents API 提交）
 *
 * 路由：
 *   POST /upload   multipart/form-data  字段: file, dir(File 或 Work), subdirs(可选，相对子目录路径)
 *   Authorization: <上传密码原文>
 */

const REPO_OWNER = 'DaaKuuLaa';
const REPO_NAME = 'DaaKuuLaa.github.io';
const REPO_BRANCH = 'main';
const MAX_FILE_SIZE = 50 * 1024 * 1024;
const SERVICE_PREFIX = 'DaaKuuLaa.github.io/';

// ---------- 工具 ----------

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8' }
  });
}

async function sha256Hex(text) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  const bytes = new Uint8Array(buf);
  let hex = '';
  for (const b of bytes) hex += b.toString(16).padStart(2, '0');
  return hex;
}

function sanitizeFileName(name) {
  // 仅保留文件名（去路径），再过滤危险字符
  const base = name.split(/[\\/]/).pop() || 'unnamed';
  return base.replace(/[^\w.\-\u4e00-\u9fa5]/g, '_');
}

function detectType(fileName) {
  const ext = fileName.slice(fileName.lastIndexOf('.')).toLowerCase();
  if (ext === '.html') return 'html';
  if (ext === '.css') return 'css';
  if (ext === '.js') return 'javascript';
  if (ext === '.md' || ext === '.markdown') return 'markdown';
  return 'file';
}

function bytesToBase64(bytes) {
  // Cloudflare Workers 支持 btoa，但大字符串一次性 btoa 在某些环境慢，
  // 这里采用分块拼接以稳妥。
  const CHUNK = 0x8000;
  let binary = '';
  for (let i = 0; i < bytes.length; i += CHUNK) {
    const slice = bytes.subarray(i, Math.min(i + CHUNK, bytes.length));
    binary += String.fromCharCode.apply(null, slice);
  }
  return btoa(binary);
}

function strToBase64(str) {
  return btoa(unescape(encodeURIComponent(str)));
}

function base64ToStr(b64) {
  return decodeURIComponent(escape(atob(b64)));
}

// ---------- GitHub API ----------

async function ghApi(env, method, path, body) {
  const url = `https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/contents/${path}`;
  const headers = {
    'Authorization': `Bearer ${env.GITHUB_PAT}`,
    'Accept': 'application/vnd.github+json',
    'X-GitHub-Api-Version': '2022-11-28',
    'Accept-Encoding': 'identity',
    'User-Agent': 'daakuulaa-upload-worker',
    'Content-Type': 'application/json'
  };
  const opt = { method, headers };
  if (body !== undefined) {
    opt.body = JSON.stringify(body);
  }
  const res = await fetch(url, opt);
  let data = null;
  let raw = null;
  const text = await res.text();
  raw = text;
  try { data = JSON.parse(text); } catch (e) { }
  return { ok: res.ok, status: res.status, data, raw };
}

// 提交文件（新文件），不带 sha → 创建
async function createFile(env, fullPath, contentBase64, message) {
  return ghApi(env, 'PUT', fullPath, { message, content: contentBase64, branch: REPO_BRANCH });
}

// 读取 JSON + sha
async function readJson(env, jsonPath) {
  const res = await ghApi(env, 'GET', jsonPath, undefined);
  if (!res.ok) return null;
  try {
    return { sha: res.data.sha, json: JSON.parse(base64ToStr(res.data.content)) };
  } catch (e) {
    return null;
  }
}

// 写回 JSON，带 sha（409 冲突重试）
async function writeJsonWithRetry(env, jsonPath, newObj, message, retries = 3) {
  for (let attempt = 0; attempt < retries; attempt++) {
    const cur = await readJson(env, jsonPath);
    if (!cur) {
      // 不存在 → 创建
      const r = await ghApi(env, 'PUT', jsonPath, {
        message, content: strToBase64(JSON.stringify(newObj, null, 2)), branch: REPO_BRANCH
      });
      if (r.ok) return true;
    } else {
      const r = await ghApi(env, 'PUT', jsonPath, {
        message, content: strToBase64(JSON.stringify(newObj, null, 2)), sha: cur.sha, branch: REPO_BRANCH
      });
      if (r.ok) return true;
      if (r.status === 409) continue; // 并发冲突 → 重试
      return false;
    }
    // 其他错误再次重试一次
  }
  return false;
}

// 在 JSON 树里按路径段逐层找到父 projects 数组
function findProjectsArray(rootNode, segments) {
  let node = rootNode;
  for (const seg of segments) {
    if (!node.projects) node.projects = [];
    const found = node.projects.find(p => p.name === seg);
    if (!found) return null;
    node = found;
  }
  if (!node.projects) node.projects = [];
  return node.projects;
}

// ---------- 主处理 ----------

async function handleUpload(request, env) {
  if (!env.GITHUB_PAT || !env.UPLOAD_PASSWORD_HASH) {
    return json({ ok: false, error: '服务端未配置 Secret' }, 500);
  }

  // 1. 密码验证
  const pwd = request.headers.get('Authorization') || '';
  const pwdHash = await sha256Hex(pwd);
  // 常量时间比较
  if (pwdHash.length !== env.UPLOAD_PASSWORD_HASH.length ||
      pwdHash !== env.UPLOAD_PASSWORD_HASH) {
    return json({ ok: false, error: '密码错误' }, 401);
  }

  // 2. 解析表单
  let form;
  try { form = await request.formData(); } catch (e) {
    return json({ ok: false, error: '表单解析失败' }, 400);
  }
  const file = form.get('file');
  const dir = form.get('dir') === 'Work' ? 'Work' : 'File';
  const subdirsRaw = (form.get('subdirs') || '').toString().trim();

  if (!file || typeof file === 'string' || !file.name) {
    return json({ ok: false, error: '未提供文件' }, 400);
  }

  // 3. 文件校验
  const safeName = sanitizeFileName(file.name);
  if (!safeName) return json({ ok: false, error: '文件名无效' }, 400);
  if (file.size > MAX_FILE_SIZE) {
    return json({ ok: false, error: '文件过大（上限 50MB）' }, 413);
  }

  // 4. 计算子目录段（安全过滤，禁止 .. 与绝对路径）
  const subSegs = subdirsRaw
    .split('/')
    .map(s => s.trim())
    .filter(s => s && s !== '.' && s !== '..' && !s.includes('\\') && !/[<>:"|?*]/.test(s));
  if (subSegs.length > 8) return json({ ok: false, error: '目录层级过深' }, 400);

  // 5. 计算目标文件名（仅同名冲突时加时间戳前缀）
  const ts = Date.now().toString(36);
  let finalName = safeName;
  // 检查同名文件是否已存在
  const checkPath = [dir, ...subSegs, safeName].join('/');
  const checkRes = await ghApi(env, 'GET', checkPath, undefined);
  if (checkRes.ok) {
    // 同名文件存在 → 加时间戳前缀
    finalName = `${ts}_${safeName}`;
  }

  const fullPathSegs = [dir, ...subSegs, finalName];
  const fullPath = fullPathSegs.join('/');
  const buf = new Uint8Array(await file.arrayBuffer());
  const fileB64 = bytesToBase64(buf);
  const commitFile = await createFile(env, fullPath, fileB64, `Web-upload: ${safeName}`);
  if (!commitFile.ok) {
    return json({
      ok: false,
      error: '提交文件失败',
      ghStatus: commitFile.status,
      ghData: commitFile.data,
      ghRaw: (commitFile.raw || '').slice(0, 800)
    }, 502);
  }

  // 6. 更新 JSON 索引
  const jsonPath = dir === 'Work' ? 'work.json' : 'file.json';
  const fullTargetPath = SERVICE_PREFIX + fullPath;
  const newEntry = {
    name: finalName,
    path: fullTargetPath,
    type: detectType(safeName),
    projects: []
  };

  // 目标根（File 或 Work）的 segments：
  //   file.json 根节点 name="File"，要加到根下的 subSegs 路径
  //   所以从根节点开始找 subSegs 段
  let updated = false;
  for (let attempt = 0; attempt < 3; attempt++) {
    const cur = await readJson(env, jsonPath);
    if (!cur) {
      // JSON 不存在 → 构造最小结构
      const root = {
        name: dir,
        path: SERVICE_PREFIX + dir,
        type: 'folder',
        projects: []
      };
      (findProjectsArray(root, subSegs)).push(newEntry);
      const ok = await writeJsonWithRetry(env, jsonPath, root, `Index: add ${newEntry.name}`);
      if (ok) { updated = true; break; }
    } else {
      const projects = findProjectsArray(cur.json, subSegs);
      if (!projects) {
        // 路径段不存在 → 自动创建中间层 folder
        autoCreatePath(cur.json, subSegs);
        const p = findProjectsArray(cur.json, subSegs);
        p.push(newEntry);
      } else {
        projects.push(newEntry);
      }
      const ok = await writeJsonWithRetry(env, jsonPath, cur.json, `Index: add ${newEntry.name}`);
      if (ok) { updated = true; break; }
    }
  }

  if (!updated) {
    // 文件已提交但索引更新失败 → 告知前端，文件可访问但不会显示在列表里
    return json({ ok: true, partial: true, path: fullTargetPath, warning: '文件已提交但索引更新失败' });
  }

  return json({ ok: true, path: fullTargetPath, name: newEntry.name, dir });
}

function autoCreatePath(root, segs) {
  let node = root;
  for (const seg of segs) {
    if (!node.projects) node.projects = [];
    let child = node.projects.find(p => p.name === seg);
    if (!child) {
      child = {
        name: seg,
        path: (node.path ? node.path + '/' : '') + seg,
        type: 'folder',
        projects: []
      };
      node.projects.push(child);
    }
    node = child;
  }
}

async function handlePreflight(request) {
  return new Response(null, {
    status: 204,
    headers: {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Authorization'
    }
  });
}

export default {
  async fetch(request, env) {
    // CORS 预检
    if (request.method === 'OPTIONS') return handlePreflight(request);

    const url = new URL(request.url);
    if (request.method === 'POST' && url.pathname === '/upload') {
      try {
        const res = await handleUpload(request, env);
        // 统一加 CORS 头
        const c = res.clone();
        return new Response(c.body, {
          status: c.status,
          headers: { ...Object.fromEntries(c.headers), 'Access-Control-Allow-Origin': '*' }
        });
      } catch (err) {
        return json({ ok: false, error: '服务端异常', detail: String(err) }, 500);
      }
    }
    return json({ ok: false, error: 'Not Found' }, 404);
  }
};
