// A two-file static server: fetch() from a file:// page is blocked, so the
// replay page needs an origin.
const http = require('http'), fs = require('fs'), path = require('path');
const dir = process.argv[2] || '.';
http.createServer((req, res) => {
  const p = path.join(dir, decodeURIComponent(req.url.split('?')[0]).replace(/^\/+/, '') || 'play.html');
  fs.readFile(p, (e, b) => {
    if (e) { res.writeHead(404); return res.end('no'); }
    const t = p.endsWith('.html') ? 'text/html' : p.endsWith('.txt') ? 'text/plain' : 'application/octet-stream';
    res.writeHead(200, { 'content-type': t, 'content-length': b.length });
    res.end(b);
  });
}).listen(8731, '127.0.0.1', () => console.log('serving ' + dir + ' on 8731'));
