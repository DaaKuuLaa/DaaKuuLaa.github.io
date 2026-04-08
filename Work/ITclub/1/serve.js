const http = require('http');
const fs = require('fs');
const path = require('path');

const ROOT = 'a:/Work';
const PORT = 8787;

http.createServer((req, res) => {
  let url = req.url === '/' ? '/index.html' : req.url;
  // strip query string
  url = url.split('?')[0];
  const filePath = path.join(ROOT, url);
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end('Not found');
      return;
    }
    const ext = path.extname(filePath).slice(1);
    const mime = { html: 'text/html', css: 'text/css', js: 'application/javascript', png: 'image/png', jpg: 'image/jpeg', svg: 'image/svg+xml', ico: 'image/x-icon' };
    res.writeHead(200, { 'Content-Type': mime[ext] || 'text/plain' });
    res.end(data);
  });
}).listen(PORT, () => {
  console.log('Server running at http://localhost:' + PORT);
});
