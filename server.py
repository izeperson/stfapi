import os
import json
from flask import Flask, jsonify, request, send_from_directory

app = Flask(__name__)

# Folder where you store your game zip files (e.g. downloads from SteamRIP)
FILES_DIR = "./downloads"
PORT = 35061

# Simple credentials for the launcher's login screen
AUTH_USER = "admin"
AUTH_PASS = "password"

if not os.path.exists(FILES_DIR):
    os.makedirs(FILES_DIR)

def build_manifest():
    """Scans the FILES_DIR for .zip files and creates the manifest JSON."""
    games = []
    for i, filename in enumerate(sorted(os.listdir(FILES_DIR))):
        if filename.endswith((".zip", ".rar", ".7z")):
            games.append({
                "id": i + 1,
                "title": filename.rsplit('.', 1)[0].replace("_", " ").replace("-", " "),
                "filename": filename
            })
    return games

@app.route('/games.json', methods=['GET'])
def games_manifest():
    return jsonify(build_manifest())

@app.route('/api/login', methods=['POST'])
def login():
    data = request.get_json()
    if data and data.get("username") == AUTH_USER and data.get("password") == AUTH_PASS:
        return jsonify({"message": "OK"}), 200
    return jsonify({"message": "Invalid username or password"}), 401

@app.route('/files/<path:filename>', methods=['GET'])
def download_file(filename):
    return send_from_directory(FILES_DIR, filename)

if __name__ == '__main__':
    print(f"Direct link server running on port {PORT}")
    print(f"Place your zip/rar files in: {os.path.abspath(FILES_DIR)}")
    app.run(host='0.0.0.0', port=PORT)