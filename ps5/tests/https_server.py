# A small HTTP/1.1 file server for the helper tests: keep-alive, TLS with a certificate,
# /chunked/<file> sent in chunks, /moved/<file> a 302 to the file.
import http.server, os, ssl, sys
root, port = sys.argv[1], int(sys.argv[2])
cert = sys.argv[3] if len(sys.argv) > 3 else None
key = sys.argv[4] if len(sys.argv) > 4 else None
drop = len(sys.argv) > 5 and sys.argv[5] == 'drop'
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a):
        sys.stderr.write('%s %s\n' % (self.command, self.path)); sys.stderr.flush()
    def do_GET(self):
        from urllib.parse import unquote
        path = unquote(self.path)
        if path.startswith('/moved/'):
            self.send_response(302); self.send_header('Location', '/' + self.path[len('/moved/'):])
            self.send_header('Content-Length', '0'); self.end_headers(); return
        chunked = path.startswith('/chunked/')
        if chunked: path = path[len('/chunked'):]
        f = os.path.join(root, path.lstrip('/'))
        if not os.path.isfile(f):
            body = b'404: Not Found'
            self.send_response(404); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body); return
        data = open(f, 'rb').read()
        self.send_response(200); self.send_header('Content-Type', 'image/png')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            for i in range(0, len(data), 1000):
                c = data[i:i+1000]; self.wfile.write(b'%x\r\n' % len(c) + c + b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
        if drop: self.close_connection = True
s = http.server.ThreadingHTTPServer(('127.0.0.1', port), H)
if cert and cert != '-':
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
