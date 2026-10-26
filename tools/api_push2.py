# -*- coding: utf-8 -*-
"""API push v2：从 git 索引/提交对象读取内容（非工作区），目标复现本地提交 c7b8b50"""
import base64, json, subprocess, sys, time, urllib.request, urllib.error

REPO, BRANCH = sys.argv[1], sys.argv[2]
TOKEN = subprocess.check_output(
    ["C:\\Program Files\\GitHub CLI\\gh.exe", "auth", "token"]).decode().strip()
API = "https://api.github.com"
LOCAL_COMMIT = subprocess.check_output(
    ["git", "rev-parse", "HEAD"]).decode().strip()

def req(method, path, payload=None):
    data = json.dumps(payload).encode() if payload is not None else None
    r = urllib.request.Request(API + path, data=data, method=method)
    r.add_header("Authorization", "Bearer " + TOKEN)
    r.add_header("Accept", "application/vnd.github+json")
    r.add_header("User-Agent", "api-push2")
    for i in range(3):
        try:
            with urllib.request.urlopen(r, timeout=60) as resp:
                body = resp.read()
                return json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            if e.code in (409, 422, 500, 502, 503) and i < 2:
                time.sleep(2 * (i + 1)); continue
            raise
        except (urllib.error.URLError, TimeoutError, OSError):
            if i < 2:
                time.sleep(2 * (i + 1)); continue
            raise

# 1. 本地提交元数据
raw = subprocess.check_output(["git", "cat-file", "commit", LOCAL_COMMIT]).decode()
lines = raw.split("\n")
tree = [l for l in lines if l.startswith("tree ")][0].split()[1]
author_line = [l for l in lines if l.startswith("author ")][0]
committer_line = [l for l in lines if l.startswith("committer ")][0]
msg = raw.split("\n\n", 1)[1]
def parse_person(line):
    # "author NAME <email> 1793006543 +0800"
    body = line.split(" ", 1)[1]
    lt = body.rindex("<"); gt = body.rindex(">")
    name, email = body[:lt].strip(), body[lt+1:gt]
    ts, tz = body[gt+1:].split()
    import datetime
    hh, mm = int(tz[1:3]), int(tz[3:5])
    delta = datetime.timedelta(hours=hh, minutes=mm)
    if tz[0] == "-":
        delta = -delta
    dt = datetime.datetime.fromtimestamp(int(ts), datetime.timezone(delta))
    iso = dt.strftime("%Y-%m-%dT%H:%M:%S") + tz[:1] + tz[1:3] + ":" + tz[3:5]
    return {"name": name, "email": email, "date": iso}
author = parse_person(author_line)
committer = parse_person(committer_line)
print("target commit:", LOCAL_COMMIT, "tree:", tree)

# 2. 上传 blobs（从提交对象读取，内容寻址必然与本地 sha 一致）
out = subprocess.check_output(
    ["git", "-c", "core.quotepath=off", "ls-tree", "-r", LOCAL_COMMIT]).decode()
tree_entries = []
for line in out.splitlines():
    meta, path = line.split("\t", 1)
    mode, typ, sha = meta.split()
    blob = subprocess.check_output(["git", "cat-file", "blob", sha])
    r = req("POST", f"/repos/{REPO}/git/blobs",
            {"content": base64.b64encode(blob).decode(), "encoding": "base64"})
    assert r["sha"] == sha, f"sha mismatch {path}: {r['sha']} != {sha}"
    tree_entries.append({"path": path.replace("\\", "/"),
                         "mode": mode, "type": "blob", "sha": sha})
print("blobs uploaded:", len(tree_entries))

# 3. 树 + 提交（元数据与本地逐字节一致）
tr = req("POST", f"/repos/{REPO}/git/trees", {"tree": tree_entries})
assert tr["sha"] == tree, f"tree mismatch: {tr['sha']} != {tree}"
cm = req("POST", f"/repos/{REPO}/git/commits", {
    "message": msg, "tree": tree,
    "author": author, "committer": committer})
print("created commit:", cm["sha"])
if cm["sha"] == LOCAL_COMMIT:
    print("EXACT MATCH — 复现本地提交成功")
req("PATCH", f"/repos/{REPO}/git/refs/heads/{BRANCH}", {"sha": cm["sha"], "force": True})
print("branch", BRANCH, "->", cm["sha"])
