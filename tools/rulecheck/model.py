"""共享 C 模型：词法、token、复合类型扫描。

规则只消费这里的 token / document，不再各自复制一套词法。
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path


TOKEN_RE = re.compile(r"[A-Za-z_]\w*|[{}();,]")
@dataclass
class Token:
    value: str
    line: int


@dataclass
class CompositeDefinition:
    tag: str | None
    alias: str | None
    kind: str
    file: str
    line: int
    typedef_form: bool
    anonymous: bool = False


@dataclass
class CompositeForward:
    tag: str
    alias: str
    kind: str
    file: str
    line: int


@dataclass
class CompositeScan:
    path: Path
    definitions: list[CompositeDefinition] = field(default_factory=list)
    forwards: list[CompositeForward] = field(default_factory=list)
    errors: list[tuple[int, str]] = field(default_factory=list)


def tokenize(text: str) -> list[Token]:
    """把源码切成复合类型扫描需要的 token；注释 / 字符串先抹掉。"""
    stripped = strip_comments_and_literals(text)
    line_starts = [0]
    for match in re.finditer("\n", stripped):
        line_starts.append(match.end())

    def line_at(pos: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if line_starts[mid] <= pos:
                lo = mid + 1
            else:
                hi = mid - 1
        return hi + 1

    return [Token(match.group(0), line_at(match.start())) for match in TOKEN_RE.finditer(stripped)]


def matching_brace(tokens: list[Token], open_index: int) -> int | None:
    depth = 0
    for index in range(open_index, len(tokens)):
        if tokens[index].value == "{":
            depth += 1
        elif tokens[index].value == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def skip_attributes(tokens: list[Token], index: int) -> int:
    """跳过 `}` 后的 GCC 属性（如 `__attribute__((packed))`）。"""
    while index < len(tokens) and tokens[index].value.startswith("__") and tokens[index].value != "__":
        name = tokens[index].value
        index += 1
        if index < len(tokens) and tokens[index].value == "(":
            depth = 0
            while index < len(tokens):
                if tokens[index].value == "(":
                    depth += 1
                elif tokens[index].value == ")":
                    depth -= 1
                    if depth == 0:
                        index += 1
                        break
                index += 1
        # 常见的是 __attribute__；其他 __foo 保守按属性前缀处理。
        if name != "__attribute__":
            break
    return index


def scan_composites(path: Path, root: Path) -> CompositeScan:
    """扫描一个文件的复合类型定义 / 前置声明；不做命名规则判断。"""
    scan = CompositeScan(path=path)
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        scan.errors.append((0, f"读取失败：{error}"))
        return scan

    tokens = tokenize(text)
    rel = str(path.relative_to(root))
    index = 0
    depth = 0

    while index < len(tokens):
        token = tokens[index].value

        if token == "{":
            # `extern "C" {` 是链接包装，不是复合类型体。
            if index > 0 and tokens[index - 1].value == "extern":
                index += 1
                continue
            depth += 1
            index += 1
            continue
        if token == "}":
            if depth > 0:
                depth -= 1
            index += 1
            continue
        if depth != 0:
            index += 1
            continue

        if token == "typedef":
            next_index = index + 1
            if next_index >= len(tokens) or tokens[next_index].value not in COMPOSITE:
                index += 1
                continue
            kind = tokens[next_index].value
            tag_index = next_index + 1
            if tag_index >= len(tokens):
                index += 1
                continue

            if tokens[tag_index].value == "{":
                close = matching_brace(tokens, tag_index)
                if close is None:
                    scan.errors.append((tokens[index].line, "匿名复合类型未闭合"))
                    return scan
                alias_index = skip_attributes(tokens, close + 1)
                alias = tokens[alias_index].value if alias_index < len(tokens) else None
                scan.definitions.append(
                    CompositeDefinition(None, alias, kind, rel, tokens[index].line, True, anonymous=True)
                )
                index = close + 1
                continue

            tag = tokens[tag_index].value
            if tag_index + 1 < len(tokens) and tokens[tag_index + 1].value == "{":
                close = matching_brace(tokens, tag_index + 1)
                if close is None:
                    scan.errors.append((tokens[index].line, f"{tag} 定义未闭合"))
                    return scan
                alias_index = skip_attributes(tokens, close + 1)
                alias = tokens[alias_index].value if alias_index < len(tokens) else None
                scan.definitions.append(CompositeDefinition(tag, alias, kind, rel, tokens[index].line, True))
                index = close + 1
                continue

            # typedef struct TAG alias;（前置声明）
            if tag_index + 2 < len(tokens) and tokens[tag_index + 2].value == ";":
                scan.forwards.append(
                    CompositeForward(tag, tokens[tag_index + 1].value, kind, rel, tokens[index].line)
                )
                index = tag_index + 2
                continue

            index += 1
            continue

        if token in COMPOSITE:
            kind = token
            next_index = index + 1
            if next_index >= len(tokens):
                index += 1
                continue

            if tokens[next_index].value == "{":
                close = matching_brace(tokens, next_index)
                if close is None:
                    scan.errors.append((tokens[index].line, "匿名复合类型未闭合"))
                    return scan
                scan.definitions.append(
                    CompositeDefinition(None, None, kind, rel, tokens[index].line, False, anonymous=True)
                )
                index = close + 1
                continue

            tag = tokens[next_index].value
            if next_index + 1 < len(tokens) and tokens[next_index + 1].value == "{":
                close = matching_brace(tokens, next_index + 1)
                if close is None:
                    scan.errors.append((tokens[index].line, f"{tag} 定义未闭合"))
                    return scan
                after = skip_attributes(tokens, close + 1)
                alias = None
                if after < len(tokens) and tokens[after].value != ";":
                    # `struct TAG {...} name;` 是变量声明，不是 typedef。
                    alias = tokens[after].value
                scan.definitions.append(CompositeDefinition(tag, alias, kind, rel, tokens[index].line, False))
                index = close + 1
                continue

            # 裸前置声明 `struct TAG;` 不是 H50 形态。
            if next_index + 1 < len(tokens) and tokens[next_index + 1].value == ";":
                scan.errors.append(
                    (tokens[index].line, f"bare forward declaration `{kind} {tag};`；改用 `typedef {kind} {tag} alias;`")
                )
                index = next_index + 1
                continue

            index += 1
            continue

        index += 1

    return scan

# ============================================================================
# 第 0 层：策略参数（规则只改这里，不改扫描器）
# ============================================================================
@dataclass(frozen=True)
class LayoutPolicy:
    scan_dirs: tuple[str, ...] = ("src",)
    entry_suffixes: tuple[str, ...] = (
        "_init",
        "_service",
        "_handle",
        "_step",
        "_submit",
        "_begin",
        "_run",
        "_pump",
    )
    composite_keywords: frozenset[str] = frozenset({"struct", "enum", "union"})
    registry_name_re: re.Pattern[str] = re.compile(
        r"(table|handlers|commands|callbacks|tasks|desc|ops)\w*$"
    )


POLICY = LayoutPolicy()
SCAN_DIRS = POLICY.scan_dirs
ENTRY_SUFFIXES = POLICY.entry_suffixes
COMPOSITE = set(POLICY.composite_keywords)
REGISTRY_NAME_RE = POLICY.registry_name_re


@dataclass
class Item:
    kind: str
    line: int
    text: str
    nbnc: int = 0
    name: str = ""
    start_off: int = 0
    end_off: int = 0
    end_line: int = 0

    def describe(self) -> str:
        text = " ".join(self.text.split())
        if len(text) > 72:
            text = text[:69] + "..."
        return f"{self.line:5d}  {self.kind:11s}  {text}"


@dataclass
class Result:
    path: Path
    items: list[Item] = field(default_factory=list)
    violations: list[tuple[int, str]] = field(default_factory=list)
    warnings: list[tuple[int, str]] = field(default_factory=list)

    def violation(self, line: int, message: str) -> None:
        self.violations.append((line, message))

    def warning(self, line: int, message: str) -> None:
        self.warnings.append((line, message))


@dataclass
class Document:
    """一份已扫描的 C 文件：源文本 + 顶层条目。"""

    path: Path
    source: str
    items: list[Item]

    def functions(self) -> list[Item]:
        return [item for item in self.items if item.kind == "func"]

    def validate(self) -> None:
        """检查扫描器不变量；违例即工具缺陷，不静默继续。"""
        code_items = [item for item in self.items if item.end_off > 0]
        previous_end = 0
        for item in sorted(code_items, key=lambda value: value.start_off):
            if item.start_off < previous_end:
                raise ValueError(
                    f"{self.path}:{item.line}: item overlaps previous item "
                    f"({item.start_off} < {previous_end})"
                )
            if item.end_off < item.start_off or item.end_off > len(self.source):
                raise ValueError(f"{self.path}:{item.line}: invalid offsets")
            previous_end = item.end_off


# ============================================================================
# 第 1 层：词法 / 预处理
#   只负责把源码变成“条目 + 偏移”，不判断 H51。
# ============================================================================
def strip_comments_and_literals(text: str) -> str:
    """抹掉注释 / 字符串，但保留换行与字节偏移（后续按 offset 定位）。"""
    out = list(text)
    i = 0
    n = len(text)
    state = "normal"

    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if state == "normal":
            if c == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line"
                continue
            if c == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block"
                continue
            if c == '"':
                out[i] = " "
                i += 1
                state = "string"
                continue
            if c == "'":
                out[i] = " "
                i += 1
                state = "char"
                continue
            i += 1
            continue

        if state == "line":
            if c == "\n":
                state = "normal"
            else:
                out[i] = " "
            i += 1
            continue

        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "normal"
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue

        if state in ("string", "char"):
            quote = '"' if state == "string" else "'"
            if c == "\\":
                out[i] = " "
                if i + 1 < n and text[i + 1] != "\n":
                    out[i + 1] = " "
                i += 2
                continue
            if c == quote:
                out[i] = " "
                i += 1
                state = "normal"
                continue
            if c != "\n":
                out[i] = " "
            i += 1
            continue

    return "".join(out)


def mask_directive_lines(text: str) -> tuple[str, list[Item]]:
    """把 #... 指令行变成等长占位，并返回指令类条目。"""
    lines = text.splitlines(keepends=True)
    items: list[Item] = []
    masked: list[str] = []
    continuation = False

    for idx, raw in enumerate(lines, start=1):
        stripped = raw.lstrip()
        is_directive = continuation or stripped.startswith("#")
        if is_directive:
            if not continuation:
                body = stripped[1:].strip()
                if body.startswith("include"):
                    items.append(Item("include", idx, body))
                elif body.startswith("define"):
                    items.append(Item("macro", idx, body))
                elif body.startswith("pragma") and "once" in body:
                    items.append(Item("pragma_once", idx, body))
            continuation = raw.rstrip().endswith("\\")
            # 顶层指令是布局解析器的语句边界：否则 #if 块末尾的宏调用会吞掉
            # 下一个定义（曾在 g_op_callbacks 上出现）。
            # 占位长度必须与原行一致，后续 Item offset 才对得上。
            if raw.endswith("\n"):
                masked.append(";" + " " * (len(raw) - 2) + "\n")
            else:
                masked.append(";" + " " * (len(raw) - 1))
        else:
            continuation = False
            masked.append(raw)
    return "".join(masked), items


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def skip_space(text: str, i: int) -> int:
    n = len(text)
    while i < n and text[i].isspace():
        i += 1
    return i


def find_matching_brace(text: str, start: int) -> int:
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


def find_matching_paren(text: str, start: int) -> int:
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n - 1


# ============================================================================
# 第 2 层：条目分类 / 扫描
#   只回答“这是什么条目、从哪到哪”，不判断位置是否符合 H51。
# ============================================================================
def function_name(header: str) -> str:
    """从函数头里取函数名（启发式；宏包裹的声明可能取不到）。"""
    names = re.findall(r"([A-Za-z_]\w*)\s*\(", header)
    if not names:
        return ""
    for reserved in ("if", "for", "while", "switch", "sizeof", "return"):
        if names[-1] == reserved:
            return ""
    return names[-1]


def is_pointer_declaration(statement: str) -> bool:
    return re.search(r"\(\s*\*", statement) is not None


def _strip_gcc_attributes(text: str) -> str:
    """剔除 `__attribute__((...))`：属性实参不是函数参数表。"""
    pattern = re.compile(r"__attribute__\s*\(\(\s*[^()]*(?:\([^()]*\)[^()]*)*\)\)")
    while True:
        match = pattern.search(text)
        if not match:
            return text
        text = text[: match.start()] + " " + text[match.end() :]


def is_prototype(statement: str) -> bool:
    statement = _strip_gcc_attributes(statement)
    if "(" not in statement or is_pointer_declaration(statement):
        return False
    tail = statement.rstrip()
    if not tail.endswith(")"):
        # 允许参数表后跟 __attribute__((...))
        if not re.search(r"\)\s*(?:__attribute__\s*\(\(.*\)\)\s*)*$", tail):
            return False
    return True


def classify_header(header: str) -> str:
    text = " ".join(header.split())
    if not text:
        return "other"
    if "=" in text:
        return "var"
    if "(" in text:
        return "func"
    words = set(re.findall(r"[A-Za-z_]\w*", text))
    if "typedef" in words or words & COMPOSITE:
        return "type"
    return "var"


def classify_statement(statement: str) -> str:
    text = " ".join(statement.split())
    if not text:
        return "other"
    if text.startswith("_Static_assert") or text.startswith("static_assert"):
        return "assert"
    # 语句级全大写调用是宏调用，不是原型
    # （STATS_CHECK_FIELD / FTL_STATS_LIST / ...）。
    if re.match(r"^[A-Z_][A-Z0-9_]*\s*\(", text):
        return "other"
    if text.startswith("typedef"):
        return "type"
    words = set(re.findall(r"[A-Za-z_]\w*", text))
    if "extern" in words:
        if is_prototype(text) and not is_pointer_declaration(text):
            return "proto"
        return "extern_var"
    if is_prototype(text):
        return "proto"
    return "var"


def count_nbnc(text: str) -> int:
    count = 0
    for line in text.splitlines():
        stripped = line.strip()
        if stripped and stripped not in ("{", "}"):
            count += 1
    return count


def parse_items(clean: str) -> list[Item]:
    items: list[Item] = []
    i = 0
    n = len(clean)
    item_start = 0

    while i < n:
        c = clean[i]

        if c == "{":
            header = clean[item_start:i]
            kind = classify_header(header)
            end = find_matching_brace(clean, i)
            if kind == "func":
                start_off = skip_space(clean, item_start)
                start_line = line_of(clean, start_off)
                items.append(
                    Item(
                        "func",
                        start_line,
                        header,
                        nbnc=count_nbnc(clean[i : end + 1]),
                        name=function_name(header),
                        start_off=start_off,
                        end_off=end + 1,
                        end_line=line_of(clean, end),
                    )
                )
                i = end + 1
            else:
                # 复合类型定义 / 初始化器：吃掉右花括号之后到语句分号的部分
                # （typedef 别名或变量名在那里）。
                j = end + 1
                depth = 0
                while j < n:
                    if clean[j] == "(":
                        depth += 1
                    elif clean[j] == ")":
                        depth = max(0, depth - 1)
                    elif clean[j] == ";" and depth == 0:
                        j += 1
                        break
                    j += 1
                start_off = skip_space(clean, item_start)
                start_line = line_of(clean, start_off)
                tail = clean[item_start:j]
                items.append(
                    Item(
                        kind,
                        start_line,
                        tail,
                        name=function_name(tail),
                        start_off=start_off,
                        end_off=j,
                        end_line=line_of(clean, j - 1),
                    )
                )
                i = j
            item_start = i
            continue

        if c == ";":
            statement = clean[item_start:i]
            kind = classify_statement(statement)
            if kind != "other":
                start_off = skip_space(clean, item_start)
                start_line = line_of(clean, start_off)
                items.append(
                    Item(
                        kind,
                        start_line,
                        statement,
                        name=function_name(statement),
                        start_off=start_off,
                        end_off=i + 1,
                        end_line=line_of(clean, i),
                    )
                )
            i += 1
            item_start = i
            continue

        i += 1

    return items


def load_document(path: Path) -> Document:
    """读取并扫描一个文件；扫描器不变量在此统一校验。"""
    source = path.read_text(encoding="utf-8", errors="replace")
    clean = strip_comments_and_literals(source)
    code, directive_items = mask_directive_lines(clean)
    items = directive_items + parse_items(code)
    items.sort(key=lambda item: item.line)
    document = Document(path=path, source=source, items=items)
    document.validate()
    return document



