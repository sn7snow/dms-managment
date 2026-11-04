#!/usr/bin/env bash
set -euo pipefail

server_binary=${1:?server binary path is required}
python=${2:?Python interpreter path is required}
curl=${3:?curl path is required}
work=$(mktemp -d)
server_pid=

cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT

port=$("$python" -c \
    'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
data_dir="$work/data"
mkdir -p "$data_dir/etc"

"$python" - "$data_dir" "$port" <<'PY'
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
port = int(sys.argv[2])
salt = "00112233445566778899aabbccddeeff"
password = "dms-e2e-password"
password_hash = hashlib.pbkdf2_hmac(
    "sha256", password.encode(), bytes.fromhex(salt), 600000
).hex()
config = {
    "root": str(root),
    "listen_address": "127.0.0.1",
    "port": port,
    "refresh_token": "a" * 64,
    "users": [{
        "user": "qa",
        "pass_salt": salt,
        "pass_hash": password_hash,
        "groups": ["qa"],
    }],
}
(root / "etc" / "config.json").write_text(json.dumps(config), encoding="utf-8")
(root / "etc" / "permissions.csv").write_text(
    "路径,对象(部门/用户),读,写,删,手动增版,手动删版,保存策略,上传,重命名/移动\n"
    "/,@all,0,0,0,0,0,0,0,0\n"
    "/,qa,1,1,1,0,0,0,1,1\n"
    "/new/,qa,1,1,1,0,0,1,1,1\n",
    encoding="utf-8",
)
(root / "etc" / "upload-allowlist.txt").write_text("dwg\n", encoding="utf-8")
(root / "etc" / "upload-denylist.txt").write_text("", encoding="utf-8")
(root / "admin").mkdir()
(root / "admin" / "pending_delete.csv").write_text("", encoding="utf-8")
(root / "payload.dwg").write_bytes(b"dms end-to-end payload\n")
(root / "overwrite.dwg").write_bytes(b"overwritten current revision\n")
(root / "create-new.dwg").write_bytes(b"new revision content\n")
PY

"$server_binary" --config "$data_dir/etc/config.json" >"$work/server.log" 2>&1 &
server_pid=$!
base_url="http://127.0.0.1:$port"

ready=false
for _ in $(seq 1 50); do
    if "$curl" --silent --show-error --max-time 1 \
        --output /dev/null "$base_url/api/upload-policy" 2>/dev/null; then
        ready=true
        break
    fi
    sleep 0.1
done
if [[ "$ready" != true ]]; then
    cat "$work/server.log" >&2
    echo "DMS server did not become ready" >&2
    exit 1
fi

login_response=$("$curl" --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    --data '{"user":"qa","pass":"dms-e2e-password"}' \
    "$base_url/api/login")
token=$("$python" -c \
    'import json,sys; print(json.load(sys.stdin)["token"])' <<<"$login_response")
auth_header="Authorization: Bearer $token"

list_response=$("$curl" --fail --silent --show-error \
    -H "$auth_header" "$base_url/api/list?path=%2F")
"$python" -c \
    'import json,sys; assert json.load(sys.stdin)["nodes"] == []' <<<"$list_response"

"$curl" --fail --silent --show-error \
    -H "$auth_header" \
    --data-binary "@$data_dir/payload.dwg" \
    "$base_url/api/upload?path=%2F&name=sample.dwg" >/dev/null
list_response=$("$curl" --fail --silent --show-error \
    -H "$auth_header" "$base_url/api/list?path=%2F")
"$python" -c \
    'import json,sys; n=json.load(sys.stdin)["nodes"][0]; assert n["name"] == "sample.dwg" and n["save_mode"] == 0' \
    <<<"$list_response"

open_response=$("$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"path":"/sample.dwg"}' "$base_url/api/file/open")
"$python" -c \
    'import json,sys; r=json.load(sys.stdin); assert r["status"] == "editable" and r["version"] == 1' \
    <<<"$open_response"
"$curl" --fail --silent --show-error -X PUT \
    -H "$auth_header" -H 'If-Match: "rev_1"' \
    --data-binary "@$data_dir/overwrite.dwg" \
    "$base_url/api/file?path=%2Fsample.dwg" |
    "$python" -c \
        'import json,sys; assert json.load(sys.stdin)["version"] == 1'
versions=$("$curl" --fail --silent --show-error \
    -H "$auth_header" "$base_url/api/file/versions?path=%2Fsample.dwg")
"$python" -c \
    'import json,sys; assert [v["rev"] for v in json.load(sys.stdin)["versions"]] == [1]' \
    <<<"$versions"
"$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"path":"/sample.dwg"}' "$base_url/api/file/close" >/dev/null

"$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"path":"/new"}' "$base_url/api/mkdir" >/dev/null
"$curl" --fail --silent --show-error \
    -H "$auth_header" \
    --data-binary "@$data_dir/payload.dwg" \
    "$base_url/api/upload?path=%2Fnew&name=sample.dwg" >/dev/null
list_response=$("$curl" --fail --silent --show-error \
    -H "$auth_header" "$base_url/api/list?path=%2Fnew")
"$python" -c \
    'import json,sys; r=json.load(sys.stdin); assert r["directory_save_mode"] == 1 and r["nodes"][0]["save_mode"] == 1' \
    <<<"$list_response"
open_response=$("$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"path":"/new/sample.dwg"}' "$base_url/api/file/open")
"$python" -c \
    'import json,sys; r=json.load(sys.stdin); assert r["status"] == "editable" and r["version"] == 1' \
    <<<"$open_response"
"$curl" --fail --silent --show-error -X PUT \
    -H "$auth_header" -H 'If-Match: "rev_1"' \
    --data-binary "@$data_dir/create-new.dwg" \
    "$base_url/api/file?path=%2Fnew%2Fsample.dwg" |
    "$python" -c \
        'import json,sys; assert json.load(sys.stdin)["version"] == 2'
versions=$("$curl" --fail --silent --show-error \
    -H "$auth_header" "$base_url/api/file/versions?path=%2Fnew%2Fsample.dwg")
"$python" -c \
    'import json,sys; assert [v["rev"] for v in json.load(sys.stdin)["versions"]] == [1, 2]' \
    <<<"$versions"
"$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"path":"/new/sample.dwg"}' "$base_url/api/file/close" >/dev/null

"$curl" --fail --silent --show-error \
    -H "$auth_header" -H 'Content-Type: application/json' \
    --data '{"from":"/sample.dwg","to":"/renamed.dwg"}' \
    "$base_url/api/rename" >/dev/null

"$curl" --fail --silent --show-error -X DELETE \
    -H "$auth_header" "$base_url/api/file?path=%2Frenamed.dwg" >/dev/null
if [[ -e "$data_dir/filestore/renamed.dwg" ]]; then
    echo "E2E delete did not remove the renamed file" >&2
    exit 1
fi

echo "DMS login/list/upload/rename/delete E2E passed"
