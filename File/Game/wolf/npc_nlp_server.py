# -*- coding: utf-8 -*-
# npc_nlp_server.py — 离线 NPC 智能化增强服务（§25）
#   - 独立 Python 进程（http.server + jieba 分词），C++ 侧用 NpcHttpOnce 调用，
#     失败自动回退 C++ 内置模板（Start 启动时自动拉起本服务，缺失静默跳过）
#   - 请求：POST /reply  {"npc","sender","content","at","recent":[...],"facts":[...],"persona":0-5}
#   - 响应：{"reply":"..."}  错误/异常：{"ok":false}
#   - 模板池与 npc_bot.h 完全一致（round15 断言兼容），jieba 增强：分词
#     提取关键词嵌入、更准的语义分类、名字提及感知
#   - 铁律：只引用请求携带的文本（facts/recent/content），零幻觉
import json
import random
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import jieba
    _jieba_ok = True
except Exception:
    _jieba_ok = False

# ============ 模板池（与 npc_bot.h NpcRoomReplySmart 逐字对齐） ============

QA = [
    "我觉得{x}这事，还是得看证据说话。",
    "要我说，{x}的问题不简单，我的意见是先看看。",
    "我同意{x}的看法，目前没有更合理的解释。",
    "不同意{x}这说法，理由我晚点说清楚。",
    "我的判断是{x}相关的人先观察，不急。",
    "关于{x}，我的答案很明确：先观望是对的。",
    "我倾向于{x}那边是没问题的，但也不排除。",
    "这个问题问得好，我的想法是{x}值得跟进。",
    "说实话，{x}我没想好，但绝不是乱说。",
    "我认为{x}的事再议，先看下一轮动静。",
    "可以，{x}这个说法我基本同意。",
    "不好说，{x}的真相要等更多信息，这是我的判断。",
]

TA = [
    "就这？{x}你要不先自己理理思路。",
    "呵，{x}这种话我见多了，来点新鲜的。",
    "你这话说得，我可不敢苟同，{x}先放一边。",
    "水平不行就说{x}，怎么跟小学生似的。",
    "笑死，{x}你也好意思说出口。",
    "行了行了，{x}的事你说了不算。",
    "我懒得跟你争，{x}你自己品品。",
    "怼我？那你倒是说说{x}哪里站得住。",
]

JO = [
    "哈哈{x}，这个梗我接住了。",
    "笑死我了{x}，你太有才了。",
    "好活{x}，下次还玩这个。",
    "{x}哈哈哈哈，笑不活了。",
    "这波{x}属实逗，我记下了。",
    "乐了乐了，{x}可真有你的。",
]

ME = [
    "我记得{f}，这事值得说道。",
    "{f}？我当时也听到这句了。",
    "刚才{f}，大家记一下这条线。",
    "不用提醒我也记得{f}。",
    "{f}——这条信息我留了个心眼。",
    "说到这个，{f}就是证据。",
    "我把{f}记进小本本了。",
    "慢着，{f}，这个细节别漏了。",
    "{f}，我觉得这很关键。",
    "对，{f}，这个我得表个态。",
    "谁再提{f}，我就站谁这边。",
    "别扯远了，{f}才是重点。",
]

EX = [
    "顺便一说，我刚才也在想这个事。",
    "对了，晚点我再补充两句。",
    "话说回来，我还挺好奇后续的。",
    "反正我就先这么说了，你们自己琢磨。",
]

CHAT = [
    "收到，{x}这话题有点意思，大家接着聊。",
    "嗯，{x}说得有道理，我听着呢。",
    "我在听，{x}继续，别停。",
    "行，{x}我先记一笔，等会儿再说。",
    "哈哈，{x}我倒想听听别人怎么看。",
    "这话有点东西，{x}展开讲讲？",
    "我跟你讲，{x}这事急不来。",
    "慢慢来，{x}我这边先观望。",
    "那我也插一句，{x}值得留意。",
    "行吧，{x}我先表个态：不站队。",
    "有意思，{x}让我想起之前的事。",
    "说到{x}，我其实有个小想法。",
]

MENTION = [
    "{n}提到了我，那我必须说两句：{x}。",
    "被{n}点名了，我的看法是{x}值得再看看。",
    "{n}你问我？{x}这事我还没想透。",
    "既然{n}说了{x}，那我也接个话。",
]

# ============ 语义分类 ============

QUESTION_WORDS = ["?", "？", "吗", "么", "怎么", "多少", "几", "等于", "为什么", "啥", "哪", "谁", "是不是", "如何", "咋", "能不能", "你觉得", "怎么看", "怎么办"]
TAUNT_WORDS = ["就这", "呵呵", "垃圾", "菜", "弱", "笨", "无语", "真行", "你", "怼", "呵呵呵", "不行", "拉了", "就这点"]
JOKE_WORDS = ["哈哈", "嘻嘻", "笑死", "233", "好玩", "乐", "梗", "摸鱼", "搞怪", "整活"]
IDENTITY_WORDS = ["我是", "预言家", "女巫", "守卫", "猎人", "丘比特", "狼人", "村民", "怀疑", "查杀", "是狼", "不是", "同意", "支持", "反对"]
STOP_WORDS = {"的", "了", "吗", "呢", "啊", "呀", "吧", "你", "我", "他", "她", "它", "这", "那", "是", "在", "和", "与", "就", "都", "也", "很", "还", "不", "没", "有", "什么", "怎么", "一个", "那个", "大家", "我们", "你们", "说", "看", "问", "人", "事", "下", "上", "中", "里", "给", "对", "把", "让"}


def classify(content, at):
    """纯规则语义分类：问句 > 挑衅 > 玩笑 > 陈述（与 C++ NpcClassifyAt 对齐）"""
    for w in QUESTION_WORDS:
        if w in content:
            return "QA"
    ta = 0
    for w in TAUNT_WORDS:
        if w in content:
            ta += 1
    if ta >= 1:
        return "TA"
    for w in JOKE_WORDS:
        if w in content:
            return "JO"
    return "ST"


def extract_keyword(content):
    """jieba 分词取关键词：优先身份/游戏词，其次最长非停用词；失败回退整句。
    先剥掉 @提及词（@NpcName 是称呼不是话题词，嵌进回复观感差）"""
    import re
    content = re.sub(r"@[^\s，。！？]+", "", content)
    if _jieba_ok:
        words = [w.strip() for w in jieba.lcut(content) if w.strip()]
        for w in words:
            if w in IDENTITY_WORDS:
                return w
        best = ""
        for w in words:
            if w in STOP_WORDS or len(w) < 2:
                continue
            if len(w) > len(best):
                best = w
        if best:
            return best
    # 无 jieba/无候选：取整句前 12 字（不截断在 UTF-8 码点中间）
    s = content.strip()
    return s[:12] if s else "这件事"


def _cut_utf8(s, limit):
    """按字节截断且不切断 UTF-8 码点（与 C++ 的 30 字节截断同规则）"""
    b = s.encode("utf-8")
    if len(b) <= limit:
        return s
    i = limit
    while i > 0 and (b[i] & 0xC0) == 0x80:
        i -= 1
    return b[:i].decode("utf-8", "ignore")


def persona_post(text, persona):
    """性格后处理（与 C++ NpcPersonaOf 后处理一致）：
    0 话痨 40% 追加 EX；1 高冷恒截断 30 字节+句号；5 天然呆 30% 语气符"""
    if persona == 0 and random.random() < 0.4:
        text += random.choice(EX)
    elif persona == 1:
        text = _cut_utf8(text, 30)
        if not text.endswith(("。", "！", "？", ".", "!", "?")):
            text += "。"
    elif persona == 5 and random.random() < 0.3:
        text += random.choice(["~", "……"])
    return text


def build_reply(data):
    """生成回复。data = 请求体 dict。只使用请求携带的文本（零幻觉铁律）"""
    npc = data.get("npc", "")
    sender = data.get("sender", "")
    content = data.get("content", "")
    at = bool(data.get("at", False))
    recent = data.get("recent", []) or []
    facts = data.get("facts", []) or []
    try:
        persona = int(data.get("persona", 0))
    except (TypeError, ValueError):
        persona = 0

    names = set()
    for r in recent:
        nm = r.split("：", 1)[0].strip()
        if nm:
            names.add(nm)
    for f in facts:
        nm = f.split("：", 1)[0].strip()
        if nm:
            names.add(nm)

    x = extract_keyword(content)

    # 记忆引用：facts 含身份/关键信息行 → 50% 引用（ME 池）
    mem_hit = None
    for f in facts:
        if any(w in f for w in IDENTITY_WORDS):
            mem_hit = f
            break

    if mem_hit is not None and random.random() < 0.5:
        text = random.choice(ME).replace("{f}", mem_hit)
        return persona_post(text, persona)

    # 名字提及（recent 里出现过的说话人点名本 NPC 或聊天对象）
    if not at:
        for nm in names:
            if nm and nm != npc and nm in content:
                text = random.choice(MENTION).replace("{n}", nm).replace("{x}", x)
                return persona_post(text, persona)

    kind = classify(content, at)

    if kind == "QA" and at:
        pool = QA
    elif kind == "TA":
        # 毒舌（persona=4）恒走怼人；其余被挑衅 60% 概率（round15 U3 断言）
        if persona == 4 or random.random() < 0.6:
            pool = TA
        else:
            pool = QA if at else CHAT
    elif kind == "JO":
        pool = JO
    else:
        pool = QA if at else CHAT

    return persona_post(random.choice(pool).replace("{x}", x), persona)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length > 0 else b""
            data = json.loads(body.decode("utf-8"))
            reply = build_reply(data)
            resp = json.dumps({"reply": reply}, ensure_ascii=False).encode("utf-8")
            try:
                with open("npc_nlp.log", "a", encoding="utf-8") as f:
                    f.write(data.get("content", "")[:40] + " -> " + reply[:40] + "\n")
            except Exception:
                pass
        except Exception:
            resp = b'{"ok":false}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        try:
            self.wfile.write(resp)
        except Exception:
            pass

    def do_GET(self):
        resp = b'{"ok":true,"name":"npc_nlp_server"}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        try:
            self.wfile.write(resp)
        except Exception:
            pass


def main():
    port = 18082
    try:
        server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    except OSError:
        sys.exit(1)  # 端口被占（已有实例）静默退出
    server.serve_forever()


if __name__ == "__main__":
    main()