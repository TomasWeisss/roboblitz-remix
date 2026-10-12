"""Bakes PBR textures for the materials of Remix captures from proxy material dumps.

Remix keys a material by the texture in stage 0. The proxy's material dump (Ctrl+F9) records, for
each material drawn, that texture plus everything the game's shader combines into the final look:
material code, all bound textures and the constant values. This script matches each capture
material to its dumped material by the stage 0 texture's pixels, evaluates the material functions
over the texture's UV space and writes, per material hash:

    <hash>_albedo.png     diffuse colour (sRGB), alpha = opacity or opacity mask
    <hash>_normal.png     tangent-space normal, DirectX convention (as UE3 uses)
    <hash>_roughness.png  from specular power and specular colour
    <hash>_emissive.png   emissive colour (sRGB), only if the material emits; scale in material.json
    material.json  where it came from and what was approximated

Texture files are prefixed with the material hash (<hash>_albedo.png, ...): the Toolkit's ingestion
groups its inputs and names its outputs by file name, so names must be unique across materials.
Ingest the normal map with the DirectX convention.

plus report.html to review them side by side with the game's stage 0 texture.

It bakes every material of all captures, matched against all dumps: replacements apply wherever
their hash appears in the game, not only in the capture the Toolkit project shows. A material found
in several dumps is baked from the newest. So every area needs one capture and one dump, taken at
the same spot; both are made by playing (Remix's capture, Ctrl+F9 for the dump).

Usage: python bake.py [--captures capture.usd ...] [--dumps dump.json ...] [--out dir]
Defaults: all captures and dumps in the game install (local.config.psd1), output to
build/material_bake.
"""

import argparse
import html
import io
import json
import os
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import hlsl

REPO = Path(__file__).resolve().parents[2]
MATCH_THRESHOLD = 3.0  # mean absolute difference (0-255) of 64x64 RGB thumbnails
MAX_SIZE = 2048
NORMAL_STRENGTH = 0.5  # the game's normal maps read too strong under path tracing


# --- Inputs ---

def game_dir():
    config = (REPO / "local.config.psd1").read_text(encoding="utf-8")
    match = re.search(r"GameDir\s*=\s*'([^']+)'", config)
    if not match:
        sys.exit("GameDir not found in local.config.psd1")
    return Path(match.group(1))


def all_files(directory, pattern):
    """Matching files, oldest first."""
    files = sorted(Path(directory).glob(pattern), key=lambda p: p.stat().st_mtime)
    if not files:
        sys.exit(f"No {pattern} in {directory}")
    return files


def load_dump(path):
    # Dumps from before the NaN fix wrote "-nan"
    text = re.sub(r"-?\b(nan|inf)\b", "null", Path(path).read_text(encoding="utf-8"))
    return json.loads(text)


def capture_materials(capture):
    """Material hash -> texture path of a Remix capture."""
    from pxr import Usd

    # The root layer references each material's own layer; compose to read them
    stage = Usd.Stage.Open(str(capture))
    looks = stage.GetPrimAtPath("/RootNode/Looks")
    materials = {}
    for prim in looks.GetChildren() if looks else []:
        name = prim.GetName()
        attribute = prim.GetChild("Shader").GetAttribute("inputs:diffuse_texture")
        texture = attribute.Get() if name.startswith("mat_") and attribute else None
        if texture and texture.resolvedPath:
            materials[name[4:]] = Path(texture.resolvedPath)
    return materials


_images = {}


def load_image(path):
    """RGBA float32 in [0, 1], as stored (no colour space conversion)."""
    key = str(path)
    if key not in _images:
        with Image.open(path) as image:
            _images[key] = np.asarray(image.convert("RGBA"), dtype=np.float32) / 255.0
    return _images[key]


_thumbnails = {}


def file_thumbnail(path):
    key = str(path)
    if key not in _thumbnails:
        _thumbnails[key] = thumbnail(load_image(path))
    return _thumbnails[key]


def thumbnail(image):
    pixels = Image.fromarray((image[..., :3] * 255 + 0.5).astype(np.uint8))
    return np.asarray(pixels.resize((64, 64), Image.BILINEAR), dtype=np.float32)


# --- Colour ---

def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def save_png(path, pixels):
    """Writes only if the content changed: apply.py re-ingests files newer than their ingested copy."""
    buffer = io.BytesIO()
    Image.fromarray((np.clip(pixels, 0, 1) * 255 + 0.5).astype(np.uint8)).save(buffer, format="PNG")
    data = buffer.getvalue()
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


# --- Sampling ---

def _address(index, size, mode):
    if mode == "clamp" or mode == "border":
        return np.clip(index, 0, size - 1)
    if mode in ("mirror", "mirroronce"):
        m = np.mod(index, 2 * size)
        return np.where(m >= size, 2 * size - 1 - m, m)
    return np.mod(index, size)


def sample_bilinear(texture, uv, address_u, address_v):
    h, w = texture.shape[:2]
    x = uv[..., 0] * w - 0.5
    y = uv[..., 1] * h - 0.5
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    x0, y0 = x0.astype(np.int64), y0.astype(np.int64)
    xa, xb = _address(x0, w, address_u), _address(x0 + 1, w, address_u)
    ya, yb = _address(y0, h, address_v), _address(y0 + 1, h, address_v)
    top = texture[ya, xa] * (1 - fx) + texture[ya, xb] * fx
    bottom = texture[yb, xa] * (1 - fx) + texture[yb, xb] * fx
    return (top * (1 - fy) + bottom * fy).astype(np.float32)


class Material:
    """A dumped material, ready to evaluate."""

    def __init__(self, entry, dump_dir):
        self.entry = entry
        self.code = entry.get("material") or {}
        self.dump_dir = dump_dir
        self.notes = []
        self._textures = {}

    def get(self, name):
        return self.code.get(name, "")

    def srgb(self, sampler_name, sampler):
        if "srgb" in sampler:
            return sampler["srgb"]
        # Dumps without the sampler state: normal maps are read linear, everything else as sRGB
        normal = self.get("normal")
        others = " ".join(v for k, v in self.code.items() if k != "normal")
        return not (sampler_name in normal and sampler_name not in others)

    def texture(self, name):
        if name not in self._textures:
            sampler = self.entry["samplers"].get(name)
            if not sampler or not sampler["texture"] or not sampler["texture"].get("file"):
                raise hlsl.Unsupported(f"no data for {name}")
            pixels = load_image(self.dump_dir / sampler["texture"]["file"])
            if self.srgb(name, sampler):
                pixels = np.concatenate([srgb_to_linear(pixels[..., :3]), pixels[..., 3:]], axis=-1)
            self._textures[name] = (pixels, sampler.get("addressU", "wrap"), sampler.get("addressV", "wrap"))
        return self._textures[name]

    def size(self, fallback):
        """Output size: the largest texture the material functions sample, capped."""
        used = " ".join(self.code.get(k, "") for k in ("diffuse", "emissive", "normal", "specular",
                                                       "specularpower", "opacity", "mask"))
        width, height = fallback
        for name, sampler in self.entry["samplers"].items():
            texture = sampler.get("texture") or {}
            if name in used and texture.get("file") and texture.get("width"):
                width, height = max(width, texture["width"]), max(height, texture["height"])
        return min(width, MAX_SIZE), min(height, MAX_SIZE)

    def context(self, width, height):
        u = (np.arange(width, dtype=np.float32) + 0.5) / width
        v = (np.arange(height, dtype=np.float32) + 0.5) / height
        uv = np.stack(np.meshgrid(u, v), axis=-1)

        def sample(name, coordinates):
            pixels, address_u, address_v = self.texture(name)
            return sample_bilinear(pixels, coordinates, address_u, address_v)

        uniforms = {k: tuple(0.0 if x is None else x for x in v) for k, v in self.entry["constants"].items()}
        return hlsl.Context(sample=sample, uniforms=uniforms, uv=uv)

    def evaluate(self, name, context, shape):
        """The material function over the grid, broadcast to shape (H, W, n); None if absent."""
        text = self.get(name)
        if not text:
            return None
        value = hlsl.evaluate_text(text, context)
        return np.broadcast_to(value, shape[:2] + (value.shape[-1],)).astype(np.float32)

    def evaluate_optional(self, name, context, shape):
        """Like evaluate, but None with a note if it cannot be baked. The dump comes from the base
        pass shader, which compiles out what only lighting uses: constants of the specular
        functions, for instance, are often missing."""
        try:
            return self.evaluate(name, context, shape)
        except hlsl.Unsupported as error:
            self.notes.append(f"{name} not baked ({error}); default used")
            return None


# --- PBR conversion ---

def roughness_from_phong(power, specular):
    """UE3 lights with Phong (reflection vector) highlights. A Phong exponent n is about a Blinn
    exponent 4n, which maps to a Beckmann/GGX alpha of sqrt(2 / (4n + 2)); Remix's roughness is
    perceptual (alpha = roughness^2). Surfaces with little specular are made rough: their highlight
    was hardly visible, whatever the exponent."""
    alpha = np.sqrt(2.0 / (4.0 * np.maximum(power, 0.0) + 2.0))
    roughness = np.sqrt(alpha)
    t = np.clip(specular / 0.2, 0.0, 1.0)
    t = t * t * (3 - 2 * t)
    return np.clip(roughness * t + 1.0 * (1 - t), 0.0, 1.0)


def luminance(rgb):
    if rgb.shape[-1] == 1:
        return rgb[..., 0]
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def texture_name(texture_hash, kind):
    return f"{texture_hash}_{kind}.png"


def scale_normals(normal, strength):
    """Scales the slope of unit tangent-space normals: 0 is flat, 1 unchanged."""
    scaled = np.concatenate([normal[..., :2] * strength, np.maximum(normal[..., 2:], 0.0)], axis=-1)
    return scaled / np.maximum(np.linalg.norm(scaled, axis=-1, keepdims=True), 1e-8)


def bake(material, size, out_dir, texture_hash, normal_strength=NORMAL_STRENGTH):
    width, height = size
    name = lambda kind: texture_name(texture_hash, kind)
    shape = (height, width)
    context = material.context(width, height)
    blend = material.get("blend") or "solid"
    lighting = material.get("lighting") or "phong"
    info = {"blend": blend, "lighting": lighting, "twosided": material.get("twosided") == "1"}

    # Albedo (linear), and what the material emits
    emissive = (material.evaluate if lighting == "unlit" else material.evaluate_optional)("emissive", context, shape)
    if blend == "modulate":
        # Multiplies what is behind it (decals, grime): its colour is a darkening layer, not light
        albedo = emissive if lighting == "unlit" else material.evaluate("diffuse", context, shape)
        emissive = None
        material.notes.append("modulate: multiplies the surface behind it; baked as albedo, needs a decal setup")
    elif lighting == "unlit":
        # Not lit by the game at all: all of its colour is emitted
        albedo = np.zeros(shape + (3,), np.float32)
        material.notes.append("unlit: colour baked as emissive, albedo black")
    else:
        albedo = material.evaluate("diffuse", context, shape)

    alpha = np.ones(shape, np.float32)
    if blend == "masked" and material.get("mask"):
        alpha = (material.evaluate("mask", context, shape)[..., 0] >= 0).astype(np.float32)
        info["alphaTest"] = True
    elif blend != "solid" and material.get("opacity"):
        alpha = np.clip(material.evaluate("opacity", context, shape)[..., 0], 0.0, 1.0)

    info["albedo"] = name("albedo")
    save_png(out_dir / name("albedo"), np.concatenate([linear_to_srgb(albedo), alpha[..., None]], axis=-1))

    if lighting != "unlit" and material.get("normal"):
        normal = material.evaluate_optional("normal", context, shape)
        if normal is not None:
            normal = normal / np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-8)
            normal = scale_normals(normal, normal_strength)
            save_png(out_dir / name("normal"), normal * 0.5 + 0.5)
            info["normal"] = name("normal")

    if lighting != "unlit":
        power = material.evaluate_optional("specularpower", context, shape)
        specular = material.evaluate_optional("specular", context, shape)
        power = power[..., 0] if power is not None else np.full(shape, 15.0, np.float32)
        # UE3's default specular colour is a mid grey
        specular = luminance(specular) if specular is not None else np.full(shape, 0.2, np.float32)
        roughness = roughness_from_phong(power, specular)
        save_png(out_dir / name("roughness"), roughness)
        info["roughness"] = name("roughness")
        info["roughnessRange"] = [round(float(roughness.min()), 3), round(float(roughness.max()), 3)]

    if emissive is not None:
        emissive = np.maximum(emissive, 0.0)
        peak = float(emissive.max())
        if peak > 1.0 / 255:
            # PNG holds [0, 1]; values above 1 go into the intensity
            scale = max(peak, 1.0)
            save_png(out_dir / name("emissive"), linear_to_srgb(emissive / scale))
            info["emissive"] = name("emissive")
            info["emissiveIntensity"] = round(scale, 4)
    return info


# --- Matching and the report ---

def match(captured, dumps):
    """Capture material hash -> (dump path, dumped materials whose stage 0 texture it is, most draws
    first), from the newest dump that has it. dumps: [(path, dump)], newest first."""
    thumbs = {h: file_thumbnail(p) for h, p in captured.items() if p.exists()}
    matches = {}
    unmatched = []
    for path, dump in dumps:
        found = {}
        for index, entry in enumerate(dump["materials"]):
            texture = entry.get("stage0Texture") or {}
            if not texture.get("file"):
                unmatched.append((path.name, index, entry, "stage 0 texture has no data"))
                continue
            thumb = file_thumbnail(path.parent / texture["file"])
            scored = sorted((float(np.abs(thumb - t).mean()), h) for h, t in thumbs.items())
            if scored and scored[0][0] <= MATCH_THRESHOLD:
                found.setdefault(scored[0][1], []).append(entry)
            else:
                unmatched.append((path.name, index, entry, "in no capture"))
        for texture_hash, entries in found.items():
            if texture_hash not in matches:
                matches[texture_hash] = (path, sorted(entries, key=lambda e: -e["draws"]))
    return matches, unmatched


def signature(entry):
    """What makes two dumped materials look the same: code, textures, constants it uses."""
    code = entry.get("material") or {}
    text = " ".join(code.values())
    textures = {k: (s["texture"] or {}).get("file") for k, s in entry["samplers"].items()
                if k in text}
    constants = {k: v for k, v in entry["constants"].items() if re.search(rf"\b{k}\b", text)}
    return json.dumps([code, textures, constants], sort_keys=True)


def write_report(out, rows, unmatched):
    def image(path, label):
        if path and path.exists():
            rel = os.path.relpath(path, out).replace("\\", "/")
            return f'<figure><img src="{html.escape(rel)}" loading="lazy"><figcaption>{label}</figcaption></figure>'
        return f'<figure class="none"><div></div><figcaption>{label}: none</figcaption></figure>'

    parts = []
    for row in rows:
        directory = out / row["hash"]
        figures = "".join([
            image(directory / "stage0.png", "game stage 0"),
            image(directory / texture_name(row["hash"], "albedo"), "albedo"),
            image(directory / texture_name(row["hash"], "normal"), "normal"),
            image(directory / texture_name(row["hash"], "roughness"), "roughness"),
            image(directory / texture_name(row["hash"], "emissive"), "emissive"),
        ])
        notes = "".join(f"<li>{html.escape(n)}</li>" for n in row["notes"])
        parts.append(
            f'<section class="{row["status"]}"><h2>mat_{row["hash"]} <span>{html.escape(row["summary"])}</span></h2>'
            f'<div class="figures">{figures}</div><ul>{notes}</ul></section>')
    lost = "".join(f"<li>{html.escape(name)} #{i} shader {e['shader']}: {html.escape(reason)}</li>"
                   for name, i, e, reason in unmatched)
    page = f"""<!doctype html>
<meta charset="utf-8">
<title>Material bake</title>
<style>
:root {{ color-scheme: light dark; --bg: #f6f6f4; --fg: #1d1d1b; --muted: #6b6b66; --card: #fff;
        --line: #deded8; --warn: #b4690e; --bad: #b42318; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg: #161615; --fg: #ececea; --muted: #9a9a94;
        --card: #1f1f1e; --line: #33332f; --warn: #e3a34b; --bad: #f2776b; }} }}
body {{ background: var(--bg); color: var(--fg); font: 14px/1.45 system-ui, sans-serif; margin: 0; padding: 16px; }}
h1 {{ font-size: 20px; margin: 0 0 4px; }}
p.summary {{ color: var(--muted); margin: 0 0 16px; }}
section {{ background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 12px; margin-bottom: 12px; }}
section.collision {{ border-left: 4px solid var(--warn); }}
section.failed {{ border-left: 4px solid var(--bad); }}
h2 {{ font-size: 14px; margin: 0 0 8px; font-family: ui-monospace, monospace; }}
h2 span {{ font-family: system-ui, sans-serif; font-weight: normal; color: var(--muted); }}
.figures {{ display: flex; flex-wrap: wrap; gap: 8px; }}
figure {{ margin: 0; width: 160px; }}
figure img, figure div {{ width: 160px; height: 160px; object-fit: contain; display: block;
  background: repeating-conic-gradient(#8884 0 25%, transparent 0 50%) 0 0 / 16px 16px; border-radius: 4px; }}
figcaption {{ color: var(--muted); font-size: 12px; }}
ul {{ margin: 8px 0 0; padding-left: 18px; color: var(--muted); }}
</style>
<h1>Material bake</h1>
<p class="summary">{len(rows)} capture materials. Dumped materials not used: {len(unmatched)}.</p>
{''.join(parts)}
<section><h2>Dumped materials without a capture material</h2><ul>{lost or '<li>none</li>'}</ul></section>
"""
    (out / "report.html").write_text(page, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--captures", type=Path, nargs="+")
    parser.add_argument("--dumps", type=Path, nargs="+")
    parser.add_argument("--out", type=Path, default=REPO / "build" / "material_bake")
    parser.add_argument("--normal-strength", type=float, default=NORMAL_STRENGTH,
                        help=f"scale of normal map slopes, 0 = flat, 1 = as in the game (default {NORMAL_STRENGTH})")
    args = parser.parse_args()

    if not args.captures or not args.dumps:
        binaries = game_dir() / "Binaries"
        args.captures = args.captures or all_files(binaries / "rtx-remix" / "captures", "capture_*.usd")
        args.dumps = args.dumps or all_files(binaries / "roboblitz_remix_material_dumps", "*.json")
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    print(f"{len(args.captures)} captures, {len(args.dumps)} dumps -> {out}")

    # Later captures win: same hash, same texture, but a newer file
    captured = {}
    for capture in sorted(args.captures, key=lambda p: p.stat().st_mtime):
        captured.update(capture_materials(capture))
    dumps = [(path, load_dump(path)) for path in sorted(args.dumps, key=lambda p: -p.stat().st_mtime)]
    matches, unmatched = match(captured, dumps)

    rows, counts = [], {"baked": 0, "collision": 0, "failed": 0, "missing": 0}
    for texture_hash, texture_path in sorted(captured.items()):
        directory = out / texture_hash
        directory.mkdir(exist_ok=True)
        stage0 = load_image(texture_path)
        save_png(directory / "stage0.png", stage0)
        dump_path, entries = matches.get(texture_hash, (None, []))
        row = {"hash": texture_hash, "notes": [], "status": "ok"}
        (directory / "material.json").unlink(missing_ok=True)  # apply.py takes what this run baked
        if not entries:
            row.update(status="failed", summary="no dumped material")
            counts["missing"] += 1
            rows.append(row)
            continue

        material = Material(entries[0], dump_path.parent)
        variants = {signature(e) for e in entries}
        if len(variants) > 1:
            row["status"] = "collision"
            others = ", ".join(e["shader"] for e in entries[1:])
            material.notes.append(f"{len(variants)} different materials share this stage 0 texture; baked the one "
                                  f"with most draws ({entries[0]['draws']}), also: {others}")
            counts["collision"] += 1
        try:
            info = bake(material, material.size(stage0.shape[1::-1]), directory, texture_hash, args.normal_strength)
        except hlsl.Unsupported as error:
            row.update(status="failed", summary=f"shader {entries[0]['shader']}: {error}")
            row["notes"] = material.notes
            counts["failed"] += 1
            rows.append(row)
            continue
        counts["baked"] += 1
        info.update(dump=dump_path.name, shader=entries[0]["shader"], draws=entries[0]["draws"],
                    notes=material.notes,
                    stage0=entries[0].get("stage0"), sharedWith=[e["shader"] for e in entries[1:]])
        (directory / "material.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
        row["summary"] = f"shader {entries[0]['shader']}, {info['blend']}, {info['lighting']}"
        row["notes"] = material.notes
        rows.append(row)

    write_report(out, rows, unmatched)
    print(f"{len(captured)} capture materials: {counts['baked']} baked ({counts['collision']} shared by "
          f"different materials), {counts['failed']} failed, {counts['missing']} without a dumped material; "
          f"{len(unmatched)} dumped materials unused")
    print(f"Report: {out / 'report.html'}")


if __name__ == "__main__":
    main()
