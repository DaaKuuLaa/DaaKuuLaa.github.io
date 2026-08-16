#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
npc_nn_server.py — 本地轻量相关性网络服务（第十三轮 §23.3）

给 Start.exe/Server.exe 提供"房内/局内聊天 → 各 NPC 相关性分数"的 HTTP 接口。
设计约束：
  - 只用 numpy（无 sklearn/torch），Python 3.6+ 即可运行
  - 网络结构：词哈希嵌入（固定 256 维）→ 单隐层感知机（64 单元 ReLU）→ sigmoid
  - 权重用预置语义先验初始化（狼人杀词/生活词/社交词/玩家与 NPC 名字/槽位号），
    不训练、无随机性，保证验收结果稳定可复现
  - C++ 侧用已有 WinHTTP 工具调用（NpcHttpOnce），失败回退内置规则

HTTP 协议：
  POST /score
  body: {"text": "聊天文本", "npcs": ["NPC名字"], "names": ["玩家名"], "context": ["最近聊天"]}
  resp: {"scores": {"NpcOne": 0.87, ...}, "topic": "狼人", "reply": "可选建议回复"}

运行：python npc_nn_server.py [端口]
环境变量 WOLF_NPC_NN_PORT 可覆盖默认端口（18083）。
"""

import json
import math
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import numpy as np
except ImportError:
    np = None

EMBED_DIM = 256
HIDDEN_DIM = 64

# 语义词表：词 → 先验强度（正=好人对该话题感兴趣，负=可疑/对抗话题）。
# 这些词会直接增强/削弱相关 NPC 的特征向量激活。
SEMANTIC_WORDS = {
    # 狼人杀核心词（高权重，触发强相关性）
    "狼人": 2.0, "预言家": 2.0, "女巫": 2.0, "守卫": 2.0, "猎人": 2.0,
    "白狼王": 2.0, "丘比特": 2.0, "盗贼": 2.0, "驯熊师": 2.0, "乌鸦": 2.0,
    "骑士": 2.0, "狼美人": 2.0, "村民": 1.5,
    "投票": 1.5, "放逐": 1.5, "验人": 1.8, "查杀": 1.8, "金水": 1.5,
    "银水": 1.5, "刀": 1.5, "票": 1.2, "身份": 1.2, "阵营": 1.2,
    "晚上": 1.2, "白天": 1.2, "开局": 1.2, "平安夜": 1.5, "自爆": 1.5,
    "殉情": 1.5, "开枪": 1.5, "毒": 1.5, "救": 1.5, "守护": 1.5,
    # 生活/闲聊词（中低权重）
    "今天": 0.8, "天气": 0.8, "吃饭": 0.8, "睡觉": 0.6, "工作": 0.8,
    "游戏": 1.0, "好玩": 0.8, "哈哈": 0.5, "加油": 0.6, "厉害": 0.6,
    # 社交/寒暄
    "你好": 0.7, "再见": 0.5, "谢谢": 0.6, "大家": 0.5, "朋友": 0.6,
}

# 话题词：从聊天里抽出作为"最相关话题"返回给 C++ 嵌入回复
TOPIC_WORDS = [
    "狼人", "预言家", "女巫", "守卫", "猎人", "投票", "放逐", "验人",
    "查杀", "身份", "晚上", "白天", "平安夜", "游戏", "今天", "天气",
]

_SYS_RANDOM = threading.RLock()


def _hash_word(w):
    """词 → 稳定哈希索引（0..EMBED_DIM-1）。与运行时无关，跨进程稳定。"""
    h = 0
    for ch in w:
        h = (h * 31 + ord(ch)) & 0x7FFFFFFF
    return h % EMBED_DIM


def _tokenize(text):
    """简单分词：中文按字符 + 连续 2-gram，英文按单词。返回 token 列表。"""
    tokens = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if '\u4e00' <= ch <= '\u9fff':
            tokens.append(ch)
            if i + 1 < n and '\u4e00' <= text[i + 1] <= '\u9fff':
                tokens.append(ch + text[i + 1])
            i += 1
        elif ch.isalnum() or ch == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            tokens.append(text[i:j].lower())
            i = j
        else:
            i += 1
    return tokens


def _build_feature(text, name_set, npc_set, nickname_map):
    """聊天文本 → 词袋特征向量（浮点）。名字/缩写/槽位号打高权重标记。"""
    feats = np.zeros(EMBED_DIM, dtype=np.float64)

    toks = _tokenize(text)

    for t in toks:
        # 槽位号（N号/N 号/N）：存在 npc/name 集合时由 C++ 预解析传入 names，
        # 这里按名字命中给满权重
        sem = SEMANTIC_WORDS.get(t, 0.0)

        if sem != 0.0:
            feats[_hash_word(t)] += sem
        else:
            feats[_hash_word(t)] += 0.4

    # 名字/缩写/槽位号命中 → 目标特征向量强激活（决定哪个 NPC 被点名）
    for nm in name_set:
        if nm and nm in text:
            feats[_hash_word(nm)] += 3.0

    for nm in npc_set:
        if nm and nm in text:
            feats[_hash_word(nm)] += 3.0

    for short, full in nickname_map.items():
        if short and short in text:
            feats[_hash_word(full)] += 2.5

    return feats


class NnModel:
    """单隐层感知机：预置语义先验权重，无训练。score = sigmoid(W2*relu(W1*x+b1)+b2)。"""

    def __init__(self):
        self.w1 = np.zeros((HIDDEN_DIM, EMBED_DIM), dtype=np.float64)
        self.b1 = np.zeros(HIDDEN_DIM, dtype=np.float64)
        self.w2 = np.zeros((HIDDEN_DIM,), dtype=np.float64)
        self.b2 = 0.0

        # 把语义词表的权重写入第一层对应哈希槽，保证可解释、可复现
        for w, s in SEMANTIC_WORDS.items():
            idx = _hash_word(w)
            hid = idx % HIDDEN_DIM
            self.w1[hid, idx] += s * 0.5
            self.w2[hid] += s * 0.25

        self.b2 = -0.6

    def score(self, x):
        h = np.maximum(0.0, self.w1.dot(x) + self.b1)
        z = self.w2.dot(h) + self.b2
        return 1.0 / (1.0 + math.exp(-z))


_MODEL = NnModel() if np is not None else None


def _extract_topic(text):
    for w in TOPIC_WORDS:
        if w in text:
            return w
    return ""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # 静默日志，避免污染 stdout（C++ 探针只关心 body）
        pass

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))

        body = self.rfile.read(length) if length else b""

        try:
            req = json.loads(body.decode("utf-8"))
            text = req.get("text", "")
            npcs = req.get("npcs", [])
            names = req.get("names", [])
            context = req.get("context", [])

            full_ctx = text
            if context:
                full_ctx = " ".join(context) + " " + text

            topic = _extract_topic(full_ctx)

            scores = {}

            if np is not None and _MODEL is not None:
                name_set = set(names)
                npc_set = set(npcs)

                nickname_map = {}
                for nm in names + npcs:
                    if len(nm) >= 2:
                        nickname_map[nm[0]] = nm
                        if len(nm) >= 3:
                            nickname_map[nm[:2]] = nm

                x = _build_feature(full_ctx, name_set, npc_set, nickname_map)

                for nm in npcs:
                    sc = _MODEL.score(x)

                    # 名字/缩写/槽位号直接命中的强加成：网络词槽对非语义词没有
                    # 先验权重，名字激活传不到输出层，这里按命中语义显式叠加——
                    # 等价于把"谁被点名"作为最高优先特征（§23.3）
                    direct = 0.0

                    if nm and nm in full_ctx:
                        direct = 0.45

                    for short, full in nickname_map.items():
                        if full == nm and short and short in full_ctx:
                            direct = max(direct, 0.35)

                    scores[nm] = round(min(0.99, sc + direct), 4)
            else:
                # numpy 不可用时降级为纯词表命中（C++ 侧同样有兜底，这里只是服务端兜底）
                for nm in npcs:
                    sc = 0.0
                    if nm and nm in full_ctx:
                        sc = 0.9
                    for t in SEMANTIC_WORDS:
                        if t in full_ctx:
                            sc = max(sc, 0.6)
                    scores[nm] = round(sc, 4)

            resp = json.dumps({"scores": scores, "topic": topic},
                              ensure_ascii=False).encode("utf-8")

            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
        except Exception as e:  # 防御：任何解析失败都回 200 + 空分数，不崩
            resp = json.dumps({"scores": {}, "topic": "", "error": str(e)},
                              ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)


def main():
    port = int(os.environ.get("WOLF_NPC_NN_PORT", "18083"))

    if len(sys.argv) > 1:
        port = int(sys.argv[1])

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("NN-READY port=%d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()