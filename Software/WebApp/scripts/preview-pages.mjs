import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
const root = resolve('dist');
const base = process.env.EXPO_BASE_PATH || '/KappaBetaEngineeringProject';
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.ttf': 'font/ttf', '.ico': 'image/x-icon' };
createServer(async (req, res) => {
  const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  if (pathname === '/') { res.writeHead(302, { Location: `${base}/` }); res.end(); return; }
  if (!pathname.startsWith(`${base}/`)) { res.writeHead(404); res.end(); return; }
  const file = resolve(root, `.${pathname.slice(base.length)}`, pathname === `${base}/` ? 'index.html' : '');
  if (!file.startsWith(root + sep)) { res.writeHead(403); res.end(); return; }
  try { const bytes = await readFile(file); res.writeHead(200, { 'Content-Type': mime[extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-store' }); res.end(bytes); }
  catch { res.writeHead(404); res.end('Not found'); }
}).listen(8085, '127.0.0.1', () => console.log(`Preview: http://localhost:8085${base}/`));
