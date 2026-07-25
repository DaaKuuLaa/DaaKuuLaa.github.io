import * as PostalMime from 'postal-mime';

// ─── Constants ──────────────────────────────────────────────────────────────
const DOMAIN = 'dkl.cc.cd';
const ATTACHMENT_MAX_SIZE = 10 * 1024 * 1024; // 10MB - 超过此大小的附件不存储
const SESSION_TTL = 86400 * 7; // 7 days
const PAGE_SIZE = 20;
const REPO_OWNER = 'DaaKuuLaa';
const REPO_NAME = 'DaaKuuLaa.github.io';
const REPO_BRANCH = 'main';
const ATTACH_DIR = 'Email/Attachments';

// ─── Response Helpers ───────────────────────────────────────────────────────

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8' }
  });
}

function html(content, status = 200) {
  return new Response(content, {
    status,
    headers: { 'Content-Type': 'text/html; charset=utf-8' }
  });
}

// ─── Auth ────────────────────────────────────────────────────────────────────

async function sha256(text) {
  const buf = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  const bytes = new Uint8Array(buf);
  return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join('');
}

async function createSession(env, userId) {
  const token = crypto.randomUUID();
  const payload = JSON.stringify({ userId, createdAt: Date.now() });
  await env.SESSIONS.put(token, payload, { expirationTtl: SESSION_TTL });
  return token;
}

async function getSession(env, token) {
  if (!token) return null;
  const payload = await env.SESSIONS.get(token);
  if (!payload) return null;
  try { return JSON.parse(payload); } catch { return null; }
}

async function requireAuth(request, env) {
  const cookie = request.headers.get('Cookie') || '';
  const token = cookie.split(';').map(c => c.trim()).find(c => c.startsWith('token='));
  if (!token) return null;
  const session = await getSession(env, token.slice(6));
  return session;
}

// ─── GitHub API ─────────────────────────────────────────────────────────────

async function ghApi(env, method, path, body) {
  if (!env.GITHUB_PAT) return { ok: false, status: 0, data: null, raw: 'No GITHUB_PAT configured' };
  const url = `https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/contents/${path}`;
  const headers = {
    'Authorization': `Bearer ${env.GITHUB_PAT}`,
    'Accept': 'application/vnd.github+json',
    'User-Agent': 'daakuulaa-email-worker',
    'Content-Type': 'application/json'
  };
  const opt = { method, headers };
  if (body !== undefined) opt.body = JSON.stringify(body);
  const res = await fetch(url, opt);
  const text = await res.text();
  let data = null;
  try { data = JSON.parse(text); } catch {}
  return { ok: res.ok, status: res.status, data, raw: text };
}

function bytesToBase64(bytes) {
  const CHUNK = 0x8000;
  let binary = '';
  for (let i = 0; i < bytes.length; i += CHUNK) {
    const slice = bytes.subarray(i, Math.min(i + CHUNK, bytes.length));
    binary += String.fromCharCode.apply(null, slice);
  }
  return btoa(binary);
}

async function uploadToRepo(env, repoPath, content, message) {
  const b64 = bytesToBase64(new Uint8Array(content));
  return ghApi(env, 'PUT', repoPath, { message, content: b64, branch: REPO_BRANCH });
}

async function deleteFromRepo(env, repoPath, message) {
  // Need to get the file's SHA first to delete it
  const getRes = await ghApi(env, 'GET', repoPath);
  if (!getRes.ok) return { ok: false, status: getRes.status };
  const sha = getRes.data?.sha;
  if (!sha) return { ok: false };
  return ghApi(env, 'DELETE', repoPath, { message, sha, branch: REPO_BRANCH });
}

// ─── Email Parsing & Storage ────────────────────────────────────────────────

function generateEmailId(messageId) {
  // Use message-id as the unique key, fallback to hash of raw content
  if (messageId) {
    // Remove angle brackets and hash it for a clean ID
    const clean = messageId.replace(/[<>]/g, '');
    return clean.substring(0, 64) || clean;
  }
  return crypto.randomUUID();
}

async function storeEmail(env, email) {
  const emailId = generateEmailId(email.messageId);

  await env.DB.prepare(
    `INSERT OR IGNORE INTO emails (id, message_id, recipient, sender_name, sender_address, subject, text_content, html_content, received_at)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).bind(
    emailId,
    email.messageId || '',
    email.to?.[0]?.address || '',
    email.from?.name || '',
    email.from?.address || '',
    email.subject || '',
    email.text || '',
    email.html || '',
    email.date || new Date().toISOString()
  ).run();

  return emailId;
}

async function handleAttachments(env, emailId, attachments) {
  if (!attachments || attachments.length === 0) return [];

  const results = [];

  for (const att of attachments) {
    const attId = crypto.randomUUID();
    const size = att.content?.byteLength || att.content?.length || 0;
    if (size === 0) continue;

    if (size > ATTACHMENT_MAX_SIZE) {
      // Too large - mark in DB but don't store
      await env.DB.prepare(
        `INSERT INTO attachments (id, email_id, filename, content_type, size, stored_in_repo, too_large, repo_path)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
      ).bind(attId, emailId, att.filename || 'unnamed', att.mimeType || '', size, 0, 1, '').run();
      results.push({ id: attId, filename: att.filename, size, tooLarge: true });
      continue;
    }

    // Small enough - store in GitHub repo
    const safeName = att.filename.replace(/[^\w.\-\u4e00-\u9fa5]/g, '_');
    const repoPath = `${ATTACH_DIR}/${emailId}/${attId}_${safeName}`;
    const commitMsg = `Email attachment: ${safeName}`;

    try {
      const res = await uploadToRepo(env, repoPath, att.content, commitMsg);
      if (res.ok) {
        await env.DB.prepare(
          `INSERT INTO attachments (id, email_id, filename, content_type, size, stored_in_repo, too_large, repo_path)
           VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
        ).bind(attId, emailId, safeName, att.mimeType || '', size, 1, 0, repoPath).run();
        results.push({ id: attId, filename: safeName, size, repoPath });
      }
    } catch (err) {
      console.error(`Failed to upload attachment ${safeName}:`, err);
    }
  }

  return results;
}

async function deleteEmailAndAttachments(env, emailId) {
  const { results: atts } = await env.DB.prepare(
    'SELECT id, repo_path FROM attachments WHERE email_id = ? AND stored_in_repo = 1'
  ).bind(emailId).all();

  for (const att of atts) {
    if (att.repo_path) {
      try { await deleteFromRepo(env, att.repo_path, `Delete attachment: ${att.id}`); } catch {}
    }
  }

  await env.DB.prepare('DELETE FROM attachments WHERE email_id = ?').bind(emailId).run();
  await env.DB.prepare('DELETE FROM emails WHERE id = ?').bind(emailId).run();
}

async function cleanupOrphanAttachments(env) {
  // List all files in the Email/Attachments directory on GitHub
  const res = await ghApi(env, 'GET', ATTACH_DIR);
  if (!res.ok) return 0;

  const repoFiles = new Set();
  const collectFiles = async (items, prefix) => {
    for (const item of items) {
      if (item.type === 'file' && item.path.startsWith(ATTACH_DIR)) {
        repoFiles.add(item.path);
      } else if (item.type === 'dir') {
        const subRes = await ghApi(env, 'GET', item.path);
        if (subRes.ok && Array.isArray(subRes.data)) {
          await collectFiles(subRes.data, item.path);
        }
      }
    }
  };

  // GitHub Contents API returns array for directories
  if (Array.isArray(res.data)) {
    await collectFiles(res.data, ATTACH_DIR);
  }

  // Get all stored attachment paths from DB
  const { results: dbAtts } = await env.DB.prepare(
    'SELECT repo_path FROM attachments WHERE stored_in_repo = 1'
  ).all();
  const dbKeys = new Set(dbAtts.map(a => a.repo_path));

  // Delete repo files not in DB
  let deleted = 0;
  for (const filePath of repoFiles) {
    if (!dbKeys.has(filePath)) {
      try {
        await deleteFromRepo(env, filePath, `Cleanup orphan attachment: ${filePath}`);
        deleted++;
      } catch {}
    }
  }

  return deleted;
}

// ─── API Handlers ───────────────────────────────────────────────────────────

async function handleLogin(request, env) {
  try {
    const body = await request.json();
    const { username, password } = body;

    if (!username || !password) {
      return json({ ok: false, error: '请输入邮箱前缀和密码' }, 400);
    }

    const localPart = username.toLowerCase().trim().replace(/\s/g, '');
    const fullEmail = `${localPart}@${DOMAIN}`;

    if (!localPart || localPart.includes('@') || localPart.length < 1) {
      return json({ ok: false, error: '邮箱格式无效' }, 400);
    }

    if (env.ADMIN_PASSWORD_HASH) {
      const existing = await env.DB.prepare(
        'SELECT id FROM users WHERE username = ?'
      ).bind(fullEmail).first();

      if (!existing) {
        await env.DB.prepare(
          'INSERT OR IGNORE INTO users (username, password_hash) VALUES (?, ?)'
        ).bind(fullEmail, env.ADMIN_PASSWORD_HASH).run();
      }
    }

    const user = await env.DB.prepare(
      'SELECT id, password_hash FROM users WHERE username = ?'
    ).bind(fullEmail).first();

    if (!user) {
      return json({ ok: false, error: '邮箱或密码错误' }, 401);
    }

    const hash = await sha256(password);
    if (hash !== user.password_hash) {
      return json({ ok: false, error: '邮箱或密码错误' }, 401);
    }

    const token = await createSession(env, user.id);

    return new Response(JSON.stringify({ ok: true }), {
      status: 200,
      headers: {
        'Content-Type': 'application/json',
        'Set-Cookie': `token=${token}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${SESSION_TTL}`
      }
    });
  } catch (err) {
    return json({ ok: false, error: '请求格式错误' }, 400);
  }
}

async function handleLogout() {
  return new Response(JSON.stringify({ ok: true }), {
    status: 200,
    headers: {
      'Content-Type': 'application/json',
      'Set-Cookie': 'token=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0'
    }
  });
}

async function handleRegister(request, env) {
  try {
    const body = await request.json();
    const { username, password } = body;

    if (!username || !password) {
      return json({ ok: false, error: '请输入邮箱前缀和密码' }, 400);
    }

    const localPart = username.toLowerCase().trim().replace(/\s/g, '');
    const fullEmail = `${localPart}@${DOMAIN}`;

    if (!localPart || localPart.includes('@') || localPart.length < 1) {
      return json({ ok: false, error: '邮箱格式无效' }, 400);
    }

    if (password.length < 6) {
      return json({ ok: false, error: '密码至少6个字符' }, 400);
    }

    const existing = await env.DB.prepare(
      'SELECT id FROM users WHERE username = ?'
    ).bind(fullEmail).first();

    if (existing) {
      return json({ ok: false, error: '该邮箱已注册' }, 409);
    }

    const hash = await sha256(password);
    await env.DB.prepare(
      'INSERT INTO users (username, password_hash) VALUES (?, ?)'
    ).bind(fullEmail, hash).run();

    const user = await env.DB.prepare(
      'SELECT id FROM users WHERE username = ?'
    ).bind(fullEmail).first();

    const token = await createSession(env, user.id);

    return new Response(JSON.stringify({ ok: true }), {
      status: 200,
      headers: {
        'Content-Type': 'application/json',
        'Set-Cookie': `token=${token}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${SESSION_TTL}`
      }
    });
  } catch (err) {
    return json({ ok: false, error: '注册失败' }, 400);
  }
}

async function handleChangePassword(request, env) {
  const session = await requireAuth(request, env);
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  try {
    const body = await request.json();
    const { oldPassword, newPassword } = body;

    if (!oldPassword || !newPassword) {
      return json({ ok: false, error: '请填写旧密码和新密码' }, 400);
    }

    if (newPassword.length < 6) {
      return json({ ok: false, error: '新密码至少6个字符' }, 400);
    }

    const user = await env.DB.prepare(
      'SELECT id, username, password_hash FROM users WHERE id = ?'
    ).bind(session.userId).first();

    if (!user) {
      return json({ ok: false, error: '用户不存在' }, 404);
    }

    const oldHash = await sha256(oldPassword);
    if (oldHash !== user.password_hash) {
      return json({ ok: false, error: '旧密码错误' }, 401);
    }

    const newHash = await sha256(newPassword);
    await env.DB.prepare(
      'UPDATE users SET password_hash = ? WHERE id = ?'
    ).bind(newHash, user.id).run();

    return json({ ok: true, message: '密码修改成功' });
  } catch (err) {
    return json({ ok: false, error: '修改密码失败' }, 400);
  }
}

async function handleListEmails(request, env, session) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  const url = new URL(request.url);
  const page = Math.max(1, parseInt(url.searchParams.get('page') || '1'));
  const limit = Math.min(50, Math.max(1, parseInt(url.searchParams.get('limit') || String(PAGE_SIZE))));
  const offset = (page - 1) * limit;
  const search = url.searchParams.get('q') || '';

  let query, countQuery, params;
  if (search) {
    const like = `%${search}%`;
    query = `SELECT id, recipient, sender_name, sender_address, subject, received_at, is_read
             FROM emails
             WHERE subject LIKE ? OR sender_name LIKE ? OR sender_address LIKE ? OR text_content LIKE ?
             ORDER BY received_at DESC LIMIT ? OFFSET ?`;
    params = [like, like, like, like, limit, offset];
    countQuery = `SELECT COUNT(*) as total FROM emails
                  WHERE subject LIKE ? OR sender_name LIKE ? OR sender_address LIKE ? OR text_content LIKE ?`;
  } else {
    query = `SELECT id, recipient, sender_name, sender_address, subject, received_at, is_read
             FROM emails
             ORDER BY received_at DESC LIMIT ? OFFSET ?`;
    params = [limit, offset];
    countQuery = 'SELECT COUNT(*) as total FROM emails';
  }

  const [emails, countResult] = await Promise.all([
    env.DB.prepare(query).bind(...params).all(),
    search
      ? env.DB.prepare(countQuery).bind(...params.slice(0, 4)).first()
      : env.DB.prepare(countQuery).first()
  ]);

  const unreadCount = await env.DB.prepare(
    'SELECT COUNT(*) as count FROM emails WHERE is_read = 0'
  ).first();

  return json({
    ok: true,
    emails: emails.results || [],
    total: countResult?.total || 0,
    page,
    limit,
    unread: unreadCount?.count || 0
  });
}

async function handleGetEmail(request, env, session, emailId) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  const email = await env.DB.prepare(
    `SELECT id, message_id, recipient, sender_name, sender_address, subject,
            text_content, html_content, received_at, is_read
     FROM emails WHERE id = ?`
  ).bind(emailId).first();

  if (!email) return json({ ok: false, error: '邮件不存在' }, 404);

  // Mark as read
  if (!email.is_read) {
    await env.DB.prepare('UPDATE emails SET is_read = 1 WHERE id = ?').bind(emailId).run();
    email.is_read = 1;
  }

  // Get attachments
  const { results: attachments } = await env.DB.prepare(
    'SELECT id, filename, content_type, size, too_large FROM attachments WHERE email_id = ?'
  ).bind(emailId).all();

  return json({ ok: true, email, attachments: attachments || [] });
}

async function handleDeleteEmail(request, env, session, emailId) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  await deleteEmailAndAttachments(env, emailId);
  return json({ ok: true });
}

async function handleGetAttachment(request, env, session, attId) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  const att = await env.DB.prepare(
    'SELECT id, filename, content_type, size, stored_in_repo, too_large, repo_path FROM attachments WHERE id = ?'
  ).bind(attId).first();

  if (!att) return json({ ok: false, error: '附件不存在' }, 404);

  if (att.too_large) {
    return json({ ok: false, error: '附件过大已忽略', too_large: true }, 404);
  }

  if (!att.stored_in_repo || !att.repo_path) {
    return json({ ok: false, error: '附件不可用' }, 404);
  }

  // Redirect to GitHub raw URL
  const rawUrl = `https://raw.githubusercontent.com/${REPO_OWNER}/${REPO_NAME}/${REPO_BRANCH}/${att.repo_path}`;
  return Response.redirect(rawUrl, 302);
}

async function handleStats(request, env, session) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  const [total, unread, attCount] = await Promise.all([
    env.DB.prepare('SELECT COUNT(*) as count FROM emails').first(),
    env.DB.prepare('SELECT COUNT(*) as count FROM emails WHERE is_read = 0').first(),
    env.DB.prepare('SELECT COUNT(*) as count FROM attachments').first()
  ]);

  return json({
    ok: true,
    stats: {
      total: total?.count || 0,
      unread: unread?.count || 0,
      attachments: attCount?.count || 0
    }
  });
}

async function handleCleanup(request, env, session) {
  if (!session) return json({ ok: false, error: '未登录' }, 401);

  const deleted = await cleanupOrphanAttachments(env);
  return json({ ok: true, deleted });
}

// ─── Fetch Router ───────────────────────────────────────────────────────────

async function handleFetch(request, env) {
  const url = new URL(request.url);
  const method = request.method;
  const path = url.pathname;

  // API routes
  if (path === '/api/login' && method === 'POST') return handleLogin(request, env);
  if (path === '/api/register' && method === 'POST') return handleRegister(request, env);
  if (path === '/api/logout' && method === 'POST') {
    return handleLogout();
  }
  if (path === '/api/change-password' && method === 'POST') return handleChangePassword(request, env);
  if (path === '/api/stats' && method === 'GET') {
    const session = await requireAuth(request, env);
    return handleStats(request, env, session);
  }
  if (path === '/api/emails' && method === 'GET') {
    const session = await requireAuth(request, env);
    return handleListEmails(request, env, session);
  }
  if (path === '/api/cleanup' && method === 'POST') {
    const session = await requireAuth(request, env);
    return handleCleanup(request, env, session);
  }
  if (path.startsWith('/api/emails/')) {
    const rest = path.slice('/api/emails/'.length);
    if (method === 'GET' && rest.length > 0 && !rest.includes('/')) {
      const session = await requireAuth(request, env);
      return handleGetEmail(request, env, session, rest);
    }
    if (method === 'DELETE' && rest.length > 0 && !rest.includes('/')) {
      const session = await requireAuth(request, env);
      return handleDeleteEmail(request, env, session, rest);
    }
    if (method === 'GET' && rest.endsWith('/attachments')) {
      const emailId = rest.slice(0, -'/attachments'.length);
      const session = await requireAuth(request, env);
      const { results: attachments } = await env.DB.prepare(
        'SELECT id, filename, content_type, size, too_large FROM attachments WHERE email_id = ?'
      ).bind(emailId).all();
      return json({ ok: true, attachments: attachments || [] });
    }
  }
  if (path.startsWith('/api/attachments/')) {
    const attId = path.slice('/api/attachments/'.length);
    if (method === 'GET' && attId.length > 0) {
      const session = await requireAuth(request, env);
      return handleGetAttachment(request, env, session, attId);
    }
  }

  // Static files
  if (path === '/favicon.ico') {
    return new Response(null, { status: 204 });
  }

  // SPA - serve frontend for all other routes
  return html(renderFrontend(), 200);
}

// ─── Email Handler ──────────────────────────────────────────────────────────

async function handleEmail(email, env, ctx) {
  try {
    // Parse email using postal-mime
    const parser = new PostalMime.default();
    const rawResponse = new Response(email.raw);
    const parsed = await parser.parse(await rawResponse.arrayBuffer());

    // Extract recipient (to address)
    const recipient = (parsed.to && parsed.to.length > 0)
      ? parsed.to[0].address
      : (email.to || 'unknown@unknown');

    // Normalize recipient to only accept our domain
    if (!recipient.toLowerCase().endsWith(`@${DOMAIN}`)) {
      console.log(`Ignoring email for non-local domain: ${recipient}`);
      return;
    }

    // Create email object
    const emailData = {
      messageId: parsed.messageId || '',
      to: parsed.to || [{ address: recipient }],
      from: parsed.from || { address: '', name: '' },
      subject: parsed.subject || '',
      text: parsed.text || '',
      html: parsed.html || '',
      date: parsed.date || new Date().toISOString(),
    };

    // Store email in D1
    const emailId = await storeEmail(env, emailData);

    // Handle attachments
    if (parsed.attachments && parsed.attachments.length > 0) {
      ctx.waitUntil(handleAttachments(env, emailId, parsed.attachments));
    }

    console.log(`Email received: ${emailData.subject} from ${emailData.from.address} → stored as ${emailId}`);
  } catch (err) {
    console.error('Failed to process email:', err);
  }
}

// ─── Frontend ────────────────────────────────────────────────────────────────

function renderFrontend() {
  return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>DaaKuuLaa Mail</title>
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>✉</text></svg>">
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { width: 100%; height: 100%; overflow: hidden; }
body {
  font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
  background: linear-gradient(135deg, #f5f7fa 0%, #c3cfe2 100%);
  position: relative;
  display: flex;
  flex-direction: column;
}
body::before {
  content: ''; position: fixed; inset: 0;
  background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
  opacity: 0; pointer-events: none; transition: opacity 0.5s ease; z-index: -1;
}
html.dark-mode body::before { opacity: 1; }
.card, .theme-toggle, input, button, .email-item, .email-detail, .nav-bar {
  transition: all 0.4s ease;
}
.container {
  width: 100%; height: 100%; display: flex; flex-direction: column; overflow: hidden;
}
.control-bar {
  display: flex; justify-content: space-between; align-items: center;
  padding: 12px 20px; flex-shrink: 0;
}
.theme-toggle {
  background: rgba(255,255,255,0.25); backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  border: 1px solid rgba(255,255,255,0.18); border-radius: 25px;
  padding: 8px 18px; cursor: pointer; font-size: 13px; font-weight: 600;
  color: #2d3748; transition: all 0.3s ease; box-shadow: 0 4px 15px rgba(0,0,0,0.1);
}
.theme-toggle:hover { background: rgba(255,255,255,0.35); transform: translateY(-2px); }
html.dark-mode .theme-toggle { color: #f5f7fa; }
.card {
  background: rgba(255,255,255,0.25); backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  border-radius: 20px; border: 1px solid rgba(255,255,255,0.18);
  box-shadow: 0 8px 32px 0 rgba(31,38,135,0.15);
}
html.dark-mode .card { background: rgba(255,255,255,0.1); border-color: rgba(255,255,255,0.1); }
input, textarea, select {
  font-family: inherit; font-size: 14px;
  padding: 10px 14px; border-radius: 10px;
  border: 1px solid rgba(0,0,0,0.12);
  background: rgba(255,255,255,0.7); color: #2d3748;
  outline: none; transition: border-color 0.2s ease;
  width: 100%;
}
html.dark-mode input, html.dark-mode textarea {
  background: rgba(255,255,255,0.06);
  border-color: rgba(255,255,255,0.12); color: #f5f7fa;
}
input:focus, textarea:focus { border-color: #4299e1; }
.email-input-group { display:flex; align-items:stretch; }
.email-input-group input { border-radius:10px 0 0 10px !important; flex:1; }
.email-domain-suffix {
  display:flex; align-items:center; padding:0 14px;
  background:rgba(0,0,0,0.06);
  border:1px solid rgba(0,0,0,0.12); border-left:none;
  border-radius:0 10px 10px 0;
  color:#718096; font-size:14px; white-space:nowrap; flex-shrink:0;
}
html.dark-mode .email-domain-suffix {
  background:rgba(255,255,255,0.06);
  border-color:rgba(255,255,255,0.12);
  color:#a0aec0;
}
.btn {
  padding: 10px 20px; border-radius: 10px; border: none;
  font-size: 14px; font-weight: 600; cursor: pointer;
  transition: all 0.2s ease; white-space: nowrap;
}
.btn-primary {
  background: #4299e1; color: #fff;
}
.btn-primary:hover { background: #3182ce; transform: translateY(-1px); }
.btn-danger {
  background: #e53e3e; color: #fff;
}
.btn-danger:hover { background: #c53030; transform: translateY(-1px); }
.btn-ghost {
  background: rgba(255,255,255,0.2); color: #2d3748; border: 1px solid rgba(255,255,255,0.1);
}
html.dark-mode .btn-ghost { color: #f5f7fa; background: rgba(255,255,255,0.08); }
.btn-ghost:hover { background: rgba(255,255,255,0.3); }
.btn-sm { padding: 6px 14px; font-size: 13px; }

/* Login page */
.login-page {
  display: flex; align-items: center; justify-content: center;
  height: 100%; padding: 20px;
}
.login-card {
  width: 100%; max-width: 400px; padding: 40px;
}
.login-title { font-size: 24px; font-weight: 700; color: #2d3748; text-align: center; margin-bottom: 8px; }
html.dark-mode .login-title { color: #f5f7fa; }
.login-subtitle { font-size: 14px; color: #718096; text-align: center; margin-bottom: 28px; }
html.dark-mode .login-subtitle { color: #a0aec0; }
.form-group { margin-bottom: 16px; }
.form-label { display: block; font-size: 13px; font-weight: 600; color: #4a5568; margin-bottom: 6px; }
html.dark-mode .form-label { color: #a0aec0; }
.login-error { color: #e53e3e; font-size: 13px; text-align: center; margin-bottom: 16px; min-height: 20px; }
html.dark-mode .login-error { color: #fc8181; }

/* Inbox */
.main-content { flex: 1; padding: 0 20px 20px; overflow: hidden; display: flex; flex-direction: column; }
.nav-bar {
  background: rgba(255,255,255,0.25); backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  border-radius: 15px; border: 1px solid rgba(255,255,255,0.18);
  padding: 12px 20px; margin-bottom: 15px; display: flex; align-items: center;
  justify-content: space-between; flex-shrink: 0;
  box-shadow: 0 8px 32px 0 rgba(31,38,135,0.15);
}
html.dark-mode .nav-bar { background: rgba(255,255,255,0.1); border-color: rgba(255,255,255,0.1); }
.nav-left { display: flex; align-items: center; gap: 15px; }
.nav-title { font-size: 18px; font-weight: 700; color: #2d3748; }
html.dark-mode .nav-title { color: #f5f7fa; }
.nav-right { display: flex; align-items: center; gap: 10px; }
.search-box { display: flex; gap: 8px; align-items: center; }
.search-box input { width: 200px; padding: 8px 12px; font-size: 13px; }
.stats { font-size: 13px; color: #718096; }
html.dark-mode .stats { color: #a0aec0; }
.stats strong { color: #2d3748; }
html.dark-mode .stats strong { color: #f5f7fa; }
.email-list {
  flex: 1; overflow-y: auto; overflow-x: hidden;
  background: rgba(255,255,255,0.25); backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  border-radius: 20px; border: 1px solid rgba(255,255,255,0.18);
  padding: 12px; box-shadow: 0 8px 32px 0 rgba(31,38,135,0.15);
}
html.dark-mode .email-list { background: rgba(255,255,255,0.1); border-color: rgba(255,255,255,0.1); }
.email-item {
  display: flex; align-items: center; gap: 12px;
  padding: 14px 16px; border-radius: 12px; margin-bottom: 6px;
  cursor: pointer; text-decoration: none; color: inherit;
  background: rgba(255,255,255,0.2); border: 1px solid rgba(255,255,255,0.1);
  transition: all 0.2s ease;
}
html.dark-mode .email-item { background: rgba(255,255,255,0.05); }
.email-item:hover { background: rgba(255,255,255,0.35); transform: translateX(3px); }
html.dark-mode .email-item:hover { background: rgba(255,255,255,0.1); }
.email-item.unread { border-left: 3px solid #4299e1; }
.email-item.unread .email-subject { font-weight: 700; }
.email-avatar {
  width: 40px; height: 40px; border-radius: 50%;
  background: linear-gradient(135deg, #4299e1, #667eea);
  display: flex; align-items: center; justify-content: center;
  color: #fff; font-weight: 700; font-size: 16px; flex-shrink: 0;
}
.email-body { flex: 1; min-width: 0; }
.email-sender { font-size: 14px; font-weight: 600; color: #2d3748; margin-bottom: 2px; }
html.dark-mode .email-sender { color: #f5f7fa; }
.email-subject { font-size: 13px; color: #4a5568; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
html.dark-mode .email-subject { color: #a0aec0; }
.email-unread-dot { width: 8px; height: 8px; border-radius: 50%; background: #4299e1; flex-shrink: 0; }
.email-time { font-size: 12px; color: #718096; flex-shrink: 0; }
html.dark-mode .email-time { color: #a0aec0; }

/* Email Detail */
.detail-header {
  display: flex; align-items: center; gap: 12px; margin-bottom: 16px;
}
.detail-from { font-size: 16px; font-weight: 600; color: #2d3748; }
html.dark-mode .detail-from { color: #f5f7fa; }
.detail-to { font-size: 13px; color: #718096; margin-top: 2px; }
html.dark-mode .detail-to { color: #a0aec0; }
.detail-subject { font-size: 22px; font-weight: 700; color: #2d3748; margin-bottom: 12px; line-height: 1.3; }
html.dark-mode .detail-subject { color: #f5f7fa; }
.detail-meta { font-size: 13px; color: #718096; margin-bottom: 20px; }
html.dark-mode .detail-meta { color: #a0aec0; }
.detail-body {
  flex: 1; overflow-y: auto; padding: 20px; border-radius: 12px;
  background: rgba(255,255,255,0.3); line-height: 1.6; font-size: 14px; color: #2d3748;
}
html.dark-mode .detail-body { background: rgba(255,255,255,0.05); color: #e2e8f0; }
.detail-body pre { white-space: pre-wrap; word-break: break-word; font-family: inherit; }
.detail-body img { max-width: 100%; height: auto; border-radius: 8px; margin: 8px 0; }
.detail-body a { color: #4299e1; }
.attachments-section { margin-top: 16px; }
.attachments-title { font-size: 14px; font-weight: 600; color: #4a5568; margin-bottom: 8px; }
html.dark-mode .attachments-title { color: #a0aec0; }
.attachment-item {
  display: inline-flex; align-items: center; gap: 8px;
  padding: 8px 14px; border-radius: 8px; margin-right: 8px; margin-bottom: 8px;
  font-size: 13px; color: #2d3748; text-decoration: none;
  background: rgba(255,255,255,0.2); border: 1px solid rgba(255,255,255,0.1);
  transition: all 0.2s ease;
}
html.dark-mode .attachment-item { color: #f5f7fa; background: rgba(255,255,255,0.05); }
.attachment-item:hover { background: rgba(255,255,255,0.3); }

/* Pagination */
.pagination { display: flex; justify-content: center; gap: 8px; padding: 12px 0; flex-shrink: 0; }
.page-btn {
  padding: 6px 14px; border-radius: 8px; border: 1px solid rgba(255,255,255,0.18);
  background: rgba(255,255,255,0.2); color: #2d3748; font-size: 13px; cursor: pointer; transition: all 0.2s;
}
html.dark-mode .page-btn { color: #a0aec0; background: rgba(255,255,255,0.05); }
.page-btn:hover { background: rgba(255,255,255,0.3); }
.page-btn.active { background: #4299e1; color: #fff; border-color: #4299e1; }
.page-btn:disabled { opacity: 0.4; cursor: not-allowed; }

/* Empty state */
.empty-state { text-align: center; padding: 60px 20px; }
.empty-state .icon { font-size: 48px; margin-bottom: 16px; }
.empty-state .title { font-size: 18px; font-weight: 600; color: #2d3748; margin-bottom: 8px; }
html.dark-mode .empty-state .title { color: #f5f7fa; }
.empty-state .subtitle { font-size: 14px; color: #718096; }
html.dark-mode .empty-state .subtitle { color: #a0aec0; }

/* Loading */
.loading { text-align: center; padding: 40px; color: #718096; font-size: 14px; }
html.dark-mode .loading { color: #a0aec0; }
@keyframes spin { to { transform: rotate(360deg); } }
.loading-spinner {
  display: inline-block; width: 24px; height: 24px; border: 3px solid rgba(66,153,225,0.2);
  border-top-color: #4299e1; border-radius: 50%; animation: spin 0.8s linear infinite; margin-bottom: 12px;
}

/* Scrollbar */
::-webkit-scrollbar { width: 8px; }
::-webkit-scrollbar-track { background: rgba(255,255,255,0.1); border-radius: 10px; }
::-webkit-scrollbar-thumb { background: rgba(255,255,255,0.3); border-radius: 10px; }
::-webkit-scrollbar-thumb:hover { background: rgba(255,255,255,0.4); }
html.dark-mode ::-webkit-scrollbar-track { background: rgba(45,55,72,0.2); }
html.dark-mode ::-webkit-scrollbar-thumb { background: rgba(160,174,192,0.3); }
html.dark-mode ::-webkit-scrollbar-thumb:hover { background: rgba(160,174,192,0.4); }

@media (max-width: 768px) {
  .search-box input { width: 140px; }
  .nav-bar { flex-wrap: wrap; gap: 8px; }
  .detail-subject { font-size: 18px; }
  .login-card { padding: 24px; }
}

/* Toast */
.toast {
  position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%);
  padding: 12px 24px; border-radius: 12px; font-size: 14px; font-weight: 600;
  z-index: 9999; animation: toastIn 0.3s ease-out;
  backdrop-filter: blur(10px); box-shadow: 0 8px 32px rgba(0,0,0,0.2);
}
.toast.success { background: rgba(47,133,90,0.9); color: #fff; }
.toast.error { background: rgba(197,48,48,0.9); color: #fff; }
.toast.info { background: rgba(49,130,206,0.9); color: #fff; }
@keyframes toastIn { from { opacity: 0; transform: translateX(-50%) translateY(20px); } to { opacity: 1; transform: translateX(-50%) translateY(0); } }

</style>
<script>
// ─── Theme ──────────────────────────────────────────────────────────
(function(){
  try {
    if (localStorage.getItem('theme') === 'dark') {
      document.documentElement.classList.add('dark-mode');
    }
  } catch(e){}
})();

function toggleTheme() {
  const root = document.documentElement;
  root.classList.toggle('dark-mode');
  const isDark = root.classList.contains('dark-mode');
  document.getElementById('theme-btn').textContent = isDark ? '黑暗模式' : '明亮模式';
  try { localStorage.setItem('theme', isDark ? 'dark' : 'light'); } catch(e){}
}

// ─── Router ─────────────────────────────────────────────────────────
function getRoute() {
  const hash = location.hash.slice(1) || '/inbox';
  return hash;
}

function navigate(path) {
  location.hash = path;
}

// ─── API ────────────────────────────────────────────────────────────
async function api(path, options = {}) {
  const res = await fetch(path, {
    credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json', ...options.headers },
    ...options
  });
  return res;
}

function showToast(message, type = 'info') {
  const old = document.querySelector('.toast');
  if (old) old.remove();
  const t = document.createElement('div');
  t.className = 'toast ' + type;
  t.textContent = message;
  document.body.appendChild(t);
  setTimeout(() => t.remove(), 3000);
}

// ─── App ────────────────────────────────────────────────────────────
const App = {
  emails: [],
  total: 0,
  page: 1,
  limit: 20,
  search: '',
  unread: 0,

  async init() {
    // Check auth by trying to load emails
    const res = await api('/api/stats');
    if (!res.ok) {
      this.showLogin();
      return;
    }
    const data = await res.json();
    if (!data.ok) {
      this.showLogin();
      return;
    }
    this.render();
  },

  showLogin() {
    document.getElementById('app').innerHTML = this.loginHTML();
    document.getElementById('login-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const err = document.getElementById('login-error');
      const username = document.getElementById('login-email').value.trim();
      const password = document.getElementById('login-password').value;
      if (!username || !password) {
        err.textContent = '请输入邮箱前缀和密码';
        return;
      }
      try {
        const res = await api('/api/login', {
          method: 'POST',
          body: JSON.stringify({ username, password })
        });
        const data = await res.json();
        if (data.ok) {
          this.render();
        } else {
          err.textContent = data.error || '登录失败';
        }
      } catch(e) {
        err.textContent = '网络错误，请重试';
      }
    });
    document.getElementById('show-register').addEventListener('click', (e) => {
      e.preventDefault();
      this.showRegister();
    });
  },

  showRegister() {
    document.getElementById('app').innerHTML = this.registerHTML();
    document.getElementById('register-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const err = document.getElementById('register-error');
      const username = document.getElementById('reg-email').value.trim();
      const password = document.getElementById('reg-password').value;
      const confirm = document.getElementById('reg-confirm').value;
      if (!username || !password) {
        err.textContent = '请输入邮箱前缀和密码';
        return;
      }
      if (password !== confirm) {
        err.textContent = '两次密码不一致';
        return;
      }
      if (password.length < 6) {
        err.textContent = '密码至少6个字符';
        return;
      }
      try {
        const res = await api('/api/register', {
          method: 'POST',
          body: JSON.stringify({ username, password })
        });
        const data = await res.json();
        if (data.ok) {
          this.render();
        } else {
          err.textContent = data.error || '注册失败';
        }
      } catch(e) {
        err.textContent = '网络错误，请重试';
      }
    });
    document.getElementById('show-login').addEventListener('click', (e) => {
      e.preventDefault();
      this.showLogin();
    });
  },

  loginHTML() {
    return \`
    <div class="login-page">
      <div class="card login-card">
        <div class="login-title">✉ DaaKuuLaa Mail</div>
        <div class="login-subtitle">登录以查看邮件</div>
        <form id="login-form">
          <div id="login-error" class="login-error"></div>
          <div class="form-group">
            <label class="form-label" for="login-email">邮箱地址</label>
            <div class="email-input-group">
              <input type="text" id="login-email" placeholder="请输入邮箱前缀" autocomplete="username" required>
              <span class="email-domain-suffix">@${DOMAIN}</span>
            </div>
          </div>
          <div class="form-group">
            <label class="form-label" for="login-password">密码</label>
            <input type="password" id="login-password" placeholder="请输入密码" autocomplete="current-password" required>
          </div>
          <button type="submit" class="btn btn-primary" style="width:100%;padding:12px;">登 录</button>
        </form>
        <div style="text-align:center;margin-top:16px;font-size:13px;color:#718096;">
          没有账号？<a href="#" id="show-register" style="color:#4299e1;text-decoration:none;font-weight:600;">注册</a>
        </div>
      </div>
    </div>\`;
  },

  registerHTML() {
    return \`
    <div class="login-page">
      <div class="card login-card">
        <div class="login-title">✉ 注册新账号</div>
        <div class="login-subtitle">创建你的 DaaKuuLaa 邮箱账号</div>
        <form id="register-form">
          <div id="register-error" class="login-error"></div>
          <div class="form-group">
            <label class="form-label" for="reg-email">邮箱地址</label>
            <div class="email-input-group">
              <input type="text" id="reg-email" placeholder="请输入邮箱前缀" autocomplete="username" required>
              <span class="email-domain-suffix">@${DOMAIN}</span>
            </div>
          </div>
          <div class="form-group">
            <label class="form-label" for="reg-password">密码</label>
            <input type="password" id="reg-password" placeholder="至少6个字符" autocomplete="new-password" required>
          </div>
          <div class="form-group">
            <label class="form-label" for="reg-confirm">确认密码</label>
            <input type="password" id="reg-confirm" placeholder="再次输入密码" autocomplete="new-password" required>
          </div>
          <button type="submit" class="btn btn-primary" style="width:100%;padding:12px;">注 册</button>
        </form>
        <div style="text-align:center;margin-top:16px;font-size:13px;color:#718096;">
          已有账号？<a href="#" id="show-login" style="color:#4299e1;text-decoration:none;font-weight:600;">登录</a>
        </div>
      </div>
    </div>\`;
  },

  showChangePassword() {
    document.getElementById('app').innerHTML = this.changePasswordHTML();
    document.getElementById('change-pw-form').addEventListener('submit', async (e) => {
      e.preventDefault();
      const err = document.getElementById('change-pw-error');
      const oldPassword = document.getElementById('cpw-old').value;
      const newPassword = document.getElementById('cpw-new').value;
      const confirm = document.getElementById('cpw-confirm').value;
      if (!oldPassword || !newPassword) {
        err.textContent = '请填写旧密码和新密码';
        return;
      }
      if (newPassword !== confirm) {
        err.textContent = '两次新密码不一致';
        return;
      }
      if (newPassword.length < 6) {
        err.textContent = '新密码至少6个字符';
        return;
      }
      try {
        const res = await api('/api/change-password', {
          method: 'POST',
          body: JSON.stringify({ oldPassword, newPassword })
        });
        const data = await res.json();
        if (data.ok) {
          showToast('密码修改成功', 'success');
          this.render();
        } else {
          err.textContent = data.error || '修改密码失败';
        }
      } catch(e) {
        err.textContent = '网络错误，请重试';
      }
    });
  },

  changePasswordHTML() {
    return \`
    <div class="nav-bar">
      <div class="nav-left">
        <button class="btn btn-ghost btn-sm" onclick="App.render()">← 返回收件箱</button>
        <div class="nav-title">修改密码</div>
      </div>
    </div>
    <div class="login-page" style="height:auto;padding-top:20px;">
      <div class="card login-card">
        <form id="change-pw-form">
          <div id="change-pw-error" class="login-error"></div>
          <div class="form-group">
            <label class="form-label" for="cpw-old">当前密码</label>
            <input type="password" id="cpw-old" placeholder="请输入当前密码" autocomplete="current-password" required>
          </div>
          <div class="form-group">
            <label class="form-label" for="cpw-new">新密码</label>
            <input type="password" id="cpw-new" placeholder="至少6个字符" autocomplete="new-password" required>
          </div>
          <div class="form-group">
            <label class="form-label" for="cpw-confirm">确认新密码</label>
            <input type="password" id="cpw-confirm" placeholder="再次输入新密码" autocomplete="new-password" required>
          </div>
          <button type="submit" class="btn btn-primary" style="width:100%;padding:12px;">保存修改</button>
        </form>
      </div>
    </div>\`;
  },

  async render() {
    const route = getRoute();
    if (route === '/login') { this.showLogin(); return; }
    if (route === '/register') { this.showRegister(); return; }
    if (route === '/change-password') { this.showChangePassword(); return; }
    await this.loadEmails();
    this.renderInbox();
  },

  async loadEmails() {
    try {
      const params = new URLSearchParams({ page: this.page, limit: this.limit });
      if (this.search) params.set('q', this.search);
      const res = await api('/api/emails?' + params);
      const data = await res.json();
      if (data.ok) {
        this.emails = data.emails || [];
        this.total = data.total || 0;
        this.unread = data.unread || 0;
      }
    } catch(e) {
      this.emails = [];
    }
  },

  renderInbox() {
    const totalPages = Math.ceil(this.total / this.limit) || 1;
    let html = \`
    <div class="nav-bar">
      <div class="nav-left">
        <div class="nav-title">✉ 收件箱</div>
        <div class="stats"><strong>\${this.unread}</strong> 封未读 · 共 <strong>\${this.total}</strong> 封</div>
      </div>
      <div class="nav-right">
        <div class="search-box">
          <input type="text" id="search-input" placeholder="搜索邮件..." value="\${this.search}" onkeydown="if(event.key==='Enter'){App.search=this.value;App.page=1;App.render();}">
          <button class="btn btn-ghost btn-sm" onclick="App.search=document.getElementById('search-input').value;App.page=1;App.render();">搜索</button>
        </div>
        <button class="btn btn-ghost btn-sm" onclick="App.showChangePassword()">修改密码</button>
        <button class="btn btn-ghost btn-sm" onclick="App.logout()">退出</button>
      </div>
    </div>
    <div class="email-list">\`;

    if (this.emails.length === 0) {
      html += \`
        <div class="empty-state">
          <div class="icon">📭</div>
          <div class="title">暂无邮件</div>
          <div class="subtitle">发送邮件到 dkl@dkl.cc.cd 测试收件功能</div>
        </div>\`;
    } else {
      for (const email of this.emails) {
        const time = this.formatTime(email.received_at);
        const initial = (email.sender_name || email.sender_address || '?')[0].toUpperCase();
        html += \`
        <div class="email-item \${email.is_read ? '' : 'unread'}" onclick="App.showEmail('\${email.id}')">
          <div class="email-avatar">\${initial}</div>
          <div class="email-body">
            <div class="email-sender">\${this.escape(email.sender_name || email.sender_address || '未知')}</div>
            <div class="email-subject">\${this.escape(email.subject || '(无主题)')}</div>
          </div>
          \${email.is_read ? '' : '<div class="email-unread-dot"></div>'}
          <div class="email-time">\${time}</div>
        </div>\`;
      }
    }

    html += '</div>';

    // Pagination
    if (totalPages > 1) {
      html += '<div class="pagination">';
      html += \`<button class="page-btn" onclick="App.page=Math.max(1,App.page-1);App.render();" \${this.page <= 1 ? 'disabled' : ''}>上一页</button>\`;
      for (let i = Math.max(1, this.page - 2); i <= Math.min(totalPages, this.page + 2); i++) {
        html += \`<button class="page-btn \${i === this.page ? 'active' : ''}" onclick="App.page=\${i};App.render();">\${i}</button>\`;
      }
      html += \`<button class="page-btn" onclick="App.page=Math.min(\${totalPages},App.page+1);App.render();" \${this.page >= totalPages ? 'disabled' : ''}>下一页</button>\`;
      html += '</div>';
    }

    document.getElementById('app').innerHTML = html;
  },

  async showEmail(id) {
    try {
      document.getElementById('app').innerHTML = '<div class="loading" style="padding:40px;text-align:center"><div class="loading-spinner"></div><br>加载中...</div>';
      const res = await api('/api/emails/' + id);
      const data = await res.json();
      if (!data.ok) { showToast('加载失败', 'error'); this.render(); return; }

      const e = data.email;
      const atts = data.attachments || [];
      const time = this.formatTime(e.received_at);
      const initial = (e.sender_name || e.sender_address || '?')[0].toUpperCase();

      let html = \`
      <div class="nav-bar">
        <div class="nav-left">
          <button class="btn btn-ghost btn-sm" onclick="App.render()">← 返回收件箱</button>
        </div>
        <div class="nav-right">
          <button class="btn btn-danger btn-sm" onclick="App.deleteEmail('\${e.id}')">删除</button>
        </div>
      </div>
      <div class="email-list" style="display:flex;flex-direction:column;">
        <div class="detail-header">
          <div class="email-avatar">\${initial}</div>
          <div>
            <div class="detail-from">\${this.escape(e.sender_name || e.sender_address)}</div>
            <div class="detail-to">收件人: \${this.escape(e.recipient)}</div>
          </div>
        </div>
        <div class="detail-subject">\${this.escape(e.subject || '(无主题)')}</div>
        <div class="detail-meta">\${time} · \${e.sender_address}</div>
        <div class="detail-body">\`;

      if (e.html_content) {
        html += e.html_content;
      } else {
        html += '<pre>' + this.escape(e.text_content || '') + '</pre>';
      }

      html += '</div>';

      if (atts.length > 0) {
        html += '<div class="attachments-section"><div class="attachments-title">📎 附件 (' + atts.length + ')</div>';
        for (const att of atts) {
          const sizeStr = att.size > 1024 ? (att.size / 1024).toFixed(1) + ' KB' : att.size + ' B';
          if (att.too_large) {
            html += \`<span class="attachment-item" style="opacity:0.6;cursor:default;" title="附件过大已忽略">📄 \${this.escape(att.filename)} (\${sizeStr}) ⚠ 附件过大已忽略</span>\`;
          } else {
            html += \`<a href="/api/attachments/\${att.id}" class="attachment-item" target="_blank">📄 \${this.escape(att.filename)} (\${sizeStr})</a>\`;
          }
        }
        html += '</div>';
      }

      html += '</div>';
      document.getElementById('app').innerHTML = html;
    } catch(e) {
      showToast('加载失败', 'error');
      this.render();
    }
  },

  async deleteEmail(id) {
    if (!confirm('确定删除此邮件？')) return;
    try {
      const res = await api('/api/emails/' + id, { method: 'DELETE' });
      const data = await res.json();
      if (data.ok) {
        showToast('已删除', 'success');
        this.render();
      } else {
        showToast('删除失败', 'error');
      }
    } catch(e) {
      showToast('删除失败', 'error');
    }
  },

  async logout() {
    await api('/api/logout', { method: 'POST' });
    this.showLogin();
  },

  formatTime(dateStr) {
    if (!dateStr) return '';
    const d = new Date(dateStr);
    const now = new Date();
    const pad = n => String(n).padStart(2, '0');
    if (d.toDateString() === now.toDateString()) {
      return pad(d.getHours()) + ':' + pad(d.getMinutes());
    }
    const yesterday = new Date(now);
    yesterday.setDate(yesterday.getDate() - 1);
    if (d.toDateString() === yesterday.toDateString()) {
      return '昨天';
    }
    if (d.getFullYear() === now.getFullYear()) {
      return (d.getMonth() + 1) + '/' + d.getDate();
    }
    return d.getFullYear() + '/' + (d.getMonth() + 1) + '/' + d.getDate();
  },

  escape(str) {
    if (!str) return '';
    const div = document.createElement('div');
    div.textContent = str;
    return div.innerHTML;
  }
};
</script>
</head>
<body>
<div class="container">
  <div class="control-bar">
    <div style="font-weight:600;color:#2d3748;font-size:15px;">✉ DaaKuuLaa Mail</div>
    <button class="theme-toggle" onclick="toggleTheme()" id="theme-btn">明亮模式</button>
  </div>
  <div class="main-content" id="app">
    <div class="loading" style="padding:60px;text-align:center;">
      <div class="loading-spinner"></div>
      <br>加载中...
    </div>
  </div>
</div>
<script>
// Initialize theme button text
(function(){
  const isDark = document.documentElement.classList.contains('dark-mode');
  document.getElementById('theme-btn').textContent = isDark ? '黑暗模式' : '明亮模式';
})();

window.addEventListener('hashchange', () => App.render());
App.init();
</script>
</body>
</html>`;
}

// ─── Worker Entry Point ─────────────────────────────────────────────────────

export default {
  async fetch(request, env, ctx) {
    try {
      return await handleFetch(request, env);
    } catch (err) {
      console.error('Fetch error:', err);
      return new Response('Internal Error', { status: 500 });
    }
  },

  async email(message, env, ctx) {
    await handleEmail(message, env, ctx);
  }
};