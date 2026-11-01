#!/usr/bin/env python3
# docs/GUIDE.md as the app's Guide page reads it (app/src/app.cpp,
# App::set_guide): one block a line, its kind, a tab, then its text.
#
#   S title        a section (## heading); "Contents" is left out: the page
#                  has its own list
#   P text         a paragraph
#   B text         a bullet; b: one inside a bullet
#   N n<TAB>text   a numbered step
#   T a<TAB>b<TAB>c  a table row (the header row is left out)
#
# Markdown is flattened: links keep their words, ** and ` go. The first
# paragraphs, before Contents, become the section "About this guide".
#
#   python3 app/src/make_guide.py docs/GUIDE.md OUT
import re
import sys

src, out = sys.argv[1], sys.argv[2]


def flat(s):
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r"\1", s)
    s = s.replace("**", "").replace("`", "")
    s = s.replace("⋯", "...").replace("\t", " ")
    return re.sub(r"\s+", " ", s).strip()


blocks = [("S", "About this guide")]
skip = False
cur = None  # (kind, text) being continued over several lines
table_header = False


def flush():
    global cur
    if cur:
        blocks.append((cur[0], flat(cur[1])))
    cur = None


for raw in open(src, encoding="utf-8"):
    line = raw.rstrip("\n")
    s = line.strip()
    if s.startswith("# ") and not s.startswith("## "):
        continue
    if s.startswith("## "):
        flush()
        title = s[3:].strip()
        skip = title == "Contents"
        if not skip:
            blocks.append(("S", flat(title)))
        continue
    if skip:
        continue
    if s == "" or s.startswith("[Back to contents]"):
        flush()
        table_header = False
        continue
    if s.startswith("|"):
        flush()
        cells = [flat(c) for c in s.strip("|").split("|")]
        if all(re.fullmatch(r"-+", c.replace(" ", "")) for c in cells):
            continue
        if not table_header:
            table_header = True  # the header row
            continue
        blocks.append(("T", "\t".join(cells)))
        continue
    m = re.match(r"^(\s*)- (.*)$", line)
    n = re.match(r"^\s*(\d+)\. (.*)$", line)
    if m:
        flush()
        cur = ["b" if len(m.group(1)) >= 2 else "B", m.group(2)]
    elif n:
        flush()
        cur = ["N", n.group(1) + "\x00" + n.group(2)]
    elif cur and line.startswith(" "):
        cur[1] += " " + s
    else:
        if cur and cur[0] == "P":
            cur[1] += " " + s
        else:
            flush()
            cur = ["P", s]
flush()

with open(out, "w", encoding="utf-8") as f:
    for kind, text in blocks:
        f.write(kind + "\t" + text.replace("\x00", "\t") + "\n")
