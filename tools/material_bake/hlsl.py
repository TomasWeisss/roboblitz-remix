"""Evaluates the HLSL expressions UE3 generates for material functions (GetMaterialDiffuseColor,
...) over a grid of texels, with numpy.

Values are float32 arrays of shape (..., n): n components (1 for scalars), leading dimensions
broadcastable to the output grid (H, W). The subset parsed is what UE3's material compiler emits:
numbers, + - * /, comparisons and ?:, casts, swizzles, constructors and a few intrinsics.
"""

import re
from dataclasses import dataclass, field
from typing import Callable

import numpy as np


class Unsupported(Exception):
    """The expression uses something that cannot be baked (screen-space input, unknown name, ...)."""


# --- Parsing ---

_TOKEN = re.compile(
    r"\s*(?:(?P<num>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?[fFhH]?)"
    r"|(?P<id>[A-Za-z_]\w*)"
    r"|(?P<op>>=|<=|==|!=|&&|\|\||[-+*/()\[\],.?:<>!]))"
)

_TYPES = {"float": 1, "float2": 2, "float3": 3, "float4": 4, "half": 1, "half2": 2, "half3": 3, "half4": 4}


@dataclass
class Node:
    kind: str  # num, name, member, index, call, cast, unary, binary, ternary
    value: object = None
    args: list = field(default_factory=list)


def _tokenize(text):
    tokens, pos = [], 0
    text = text.strip()
    while pos < len(text):
        match = _TOKEN.match(text, pos)
        if not match or match.end() == pos:
            raise Unsupported(f"cannot tokenize at {text[pos:pos + 20]!r}")
        kind = match.lastgroup
        tokens.append((kind, match.group(kind)))
        pos = match.end()
        while pos < len(text) and text[pos].isspace():
            pos += 1
    return tokens


class _Parser:
    def __init__(self, text):
        self.tokens = _tokenize(text)
        self.pos = 0

    def peek(self, offset=0):
        i = self.pos + offset
        return self.tokens[i] if i < len(self.tokens) else (None, None)

    def take(self, expected=None):
        token = self.peek()
        if token[0] is None or (expected is not None and token[1] != expected):
            raise Unsupported(f"expected {expected!r}, got {token[1]!r}")
        self.pos += 1
        return token

    def parse(self):
        node = self.ternary()
        if self.peek()[0] is not None:
            raise Unsupported(f"unexpected {self.peek()[1]!r}")
        return node

    def ternary(self):
        condition = self.comparison()
        if self.peek()[1] != "?":
            return condition
        self.take("?")
        yes = self.ternary()
        self.take(":")
        return Node("ternary", args=[condition, yes, self.ternary()])

    def comparison(self):
        node = self.additive()
        while self.peek()[1] in (">", "<", ">=", "<=", "==", "!="):
            op = self.take()[1]
            node = Node("binary", op, [node, self.additive()])
        return node

    def additive(self):
        node = self.multiplicative()
        while self.peek()[1] in ("+", "-"):
            op = self.take()[1]
            node = Node("binary", op, [node, self.multiplicative()])
        return node

    def multiplicative(self):
        node = self.unary()
        while self.peek()[1] in ("*", "/"):
            op = self.take()[1]
            node = Node("binary", op, [node, self.unary()])
        return node

    def unary(self):
        if self.peek()[1] in ("-", "+"):
            op = self.take()[1]
            return Node("unary", op, [self.unary()])
        # Cast: (float3)x
        if self.peek()[1] == "(" and self.peek(1)[1] in _TYPES and self.peek(2)[1] == ")":
            self.take("(")
            type_name = self.take()[1]
            self.take(")")
            return Node("cast", _TYPES[type_name], [self.unary()])
        return self.postfix()

    def postfix(self):
        node = self.primary()
        while True:
            if self.peek()[1] == ".":
                self.take(".")
                node = Node("member", self.take()[1], [node])
            elif self.peek()[1] == "[":
                self.take("[")
                index = self.ternary()
                self.take("]")
                node = Node("index", None, [node, index])
            else:
                return node

    def primary(self):
        kind, text = self.take()
        if kind == "num":
            return Node("num", float(text.rstrip("fFhH")))
        if kind == "id":
            if self.peek()[1] == "(":
                self.take("(")
                args = []
                if self.peek()[1] != ")":
                    args.append(self.ternary())
                    while self.peek()[1] == ",":
                        self.take(",")
                        args.append(self.ternary())
                self.take(")")
                return Node("call", text, args)
            return Node("name", text)
        if text == "(":
            node = self.ternary()
            self.take(")")
            return node
        raise Unsupported(f"unexpected {text!r}")


def parse(text):
    return _Parser(text).parse()


# --- Evaluation ---

_SWIZZLE = {c: i for i, c in enumerate("xyzw")} | {c: i for i, c in enumerate("rgba")}


def _const(*values):
    return np.asarray(values, dtype=np.float32).reshape(1, 1, -1)


def _width(value):
    return value.shape[-1]


def _resize(value, n):
    """HLSL implicit conversion: scalars splat, longer vectors truncate."""
    if _width(value) == n:
        return value
    if _width(value) == 1:
        return np.repeat(value, n, axis=-1)
    if _width(value) > n:
        return value[..., :n]
    raise Unsupported(f"cannot widen a {_width(value)}-vector to {n}")


def _pair(a, b):
    n = max(_width(a), _width(b)) if 1 in (_width(a), _width(b)) else min(_width(a), _width(b))
    return _resize(a, n), _resize(b, n)


@dataclass
class Context:
    """Inputs of a material function.

    sample: (sampler name, uv array (..., 2)) -> RGBA array (..., 4), linear
    uniforms: UniformVector_N / UniformScalar_N -> 4 floats
    uv: TexCoords grid, (H, W, 2)
    """

    sample: Callable
    uniforms: dict
    uv: np.ndarray
    vertex_color: tuple = (1.0, 1.0, 1.0, 1.0)

    def parameter(self, name):
        if name == "VertexColor":
            return _const(*self.vertex_color)
        # Baked as seen head-on with a flat normal: no parallax offset, no view-dependent terms
        if name in ("TangentCameraVector", "TangentNormal", "TangentReflectionVector"):
            return _const(0.0, 0.0, 1.0)
        raise Unsupported(f"Parameters.{name}")


def _is_parameter(node, name):
    return (node.kind == "member" and node.value == name and node.args[0].kind == "name"
            and node.args[0].value == "Parameters")


def evaluate(node, context):
    kind = node.kind
    if kind == "num":
        return _const(node.value)
    if kind == "name":
        value = context.uniforms.get(node.value)
        if value is None:
            raise Unsupported(f"unknown name {node.value}")
        # Constants come as whole registers; a scalar is the register's x
        return _const(value[0]) if node.value.startswith("UniformScalar") else _const(*value)
    if kind == "member":
        target = node.args[0]
        if target.kind == "name" and target.value == "Parameters":
            return context.parameter(node.value)
        value = evaluate(target, context)
        indices = [_SWIZZLE.get(c) for c in node.value]
        if None in indices:
            raise Unsupported(f"member {node.value}")
        if _width(value) == 1:
            return np.repeat(value, len(indices), axis=-1)
        if max(indices) >= _width(value):
            raise Unsupported(f"swizzle .{node.value} of a {_width(value)}-vector")
        return value[..., indices]
    if kind == "index":
        target, index = node.args
        if _is_parameter(target, "TexCoords") and index.kind == "num":
            # Every texcoord set bakes in UV0, the set Remix maps the replacement with
            return context.uv
        raise Unsupported("indexing")
    if kind == "cast":
        return _resize(evaluate(node.args[0], context), node.value)
    if kind == "unary":
        value = evaluate(node.args[0], context)
        return -value if node.value == "-" else value
    if kind == "binary":
        a, b = _pair(evaluate(node.args[0], context), evaluate(node.args[1], context))
        op = node.value
        if op == "+":
            return a + b
        if op == "-":
            return a - b
        if op == "*":
            return a * b
        if op == "/":
            with np.errstate(divide="ignore", invalid="ignore"):
                return np.nan_to_num(a / b)
        comparisons = {">": np.greater, "<": np.less, ">=": np.greater_equal, "<=": np.less_equal,
                       "==": np.equal, "!=": np.not_equal}
        return comparisons[op](a, b).astype(np.float32)
    if kind == "ternary":
        condition = evaluate(node.args[0], context)
        yes, no = _pair(evaluate(node.args[1], context), evaluate(node.args[2], context))
        return np.where(_resize(condition, _width(yes)) != 0, yes, no).astype(np.float32)
    if kind == "call":
        return _call(node, context)
    raise Unsupported(kind)


def _call(node, context):
    name, args = node.value, node.args
    if name in _TYPES:
        parts = [evaluate(arg, context) for arg in args]
        n = _TYPES[name]
        if len(parts) == 1:
            return _resize(parts[0], n)
        value = np.concatenate(_broadcast_parts(parts), axis=-1)
        if _width(value) != n:
            raise Unsupported(f"{name} with {_width(value)} components")
        return value
    if name in ("tex2D", "tex2Dbias", "tex2Dlod"):
        sampler = args[0]
        if sampler.kind != "name":
            raise Unsupported("sampler expression")
        uv = evaluate(args[1], context)
        return context.sample(sampler.value, _resize(uv, 2) if _width(uv) <= 2 else uv[..., :2])
    if name == "texCUBE":
        # Reflections come from the ray tracer
        return _const(0.0, 0.0, 0.0, 0.0)
    if name == "dot":
        # dot(ReflectionVector, CameraVector) drives fresnel-like terms; baking head-on would make it
        # 1. Use 0, as seen at 45 degrees, so (1 + dot(R, C)) and the like stay neutral.
        view = ("TangentReflectionVector", "TangentCameraVector")
        if len(args) == 2 and any(_is_parameter(args[0], v) and _is_parameter(args[1], w)
                                  for v, w in (view, view[::-1])):
            return _const(0.0)
        a, b = _pair(evaluate(args[0], context), evaluate(args[1], context))
        return np.sum(a * b, axis=-1, keepdims=True)
    if name in ("MulMatrix", "mul"):
        # The matrices (LocalToWorldMatrix, TangentBasisInverse) only turn vectors that are
        # baked as constants anyway
        return evaluate(args[-1], context)
    if name == "DepthBiasedBlend":
        return evaluate(args[1], context)
    if name == "DepthBiasedAlpha":
        return evaluate(args[1], context)
    values = [evaluate(arg, context) for arg in args]
    if name == "lerp":
        a, b = _pair(values[0], values[1])
        t = values[2]
        if _width(t) != 1:
            a, t = _pair(a, t)
            b, t = _pair(b, t)
        return a + (b - a) * t
    if name == "saturate":
        return np.clip(values[0], 0.0, 1.0)
    if name == "clamp":
        x, lo = _pair(values[0], values[1])
        x, hi = _pair(x, values[2])
        return np.minimum(np.maximum(x, _resize(lo, _width(x))), hi)
    if name in ("min", "max"):
        a, b = _pair(values[0], values[1])
        return np.minimum(a, b) if name == "min" else np.maximum(a, b)
    if name == "pow":
        a, b = _pair(values[0], values[1])
        # HLSL compiles pow to exp2(log2(x) * y): undefined for x < 0
        return np.power(np.maximum(a, 0.0), b)
    if name == "abs":
        return np.abs(values[0])
    if name == "sin":
        return np.sin(values[0])
    if name == "cos":
        return np.cos(values[0])
    if name == "normalize":
        length = np.sqrt(np.sum(values[0] ** 2, axis=-1, keepdims=True))
        return values[0] / np.maximum(length, 1e-8)
    raise Unsupported(f"function {name}")


def _broadcast_parts(parts):
    shape = np.broadcast_shapes(*(p.shape[:-1] for p in parts))
    return [np.broadcast_to(p, shape + (p.shape[-1],)) for p in parts]


def evaluate_text(text, context):
    return evaluate(parse(text), context)
