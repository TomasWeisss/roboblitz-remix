"""Applies a material bake to the Remix Toolkit project.

1. Ingests the baked textures through the Toolkit's REST API (the Toolkit must be running with the
   project open): the same conversion as its Ingest tab, into the project's assets/ingested.
2. Writes generated_materials.usda next to the project: an override of each baked capture material
   (AperturePBR_Opacity inputs) pointing at the ingested textures.
3. Makes that layer a sublayer of the mod layer (mod.usda). A layer's own opinions are stronger than
   its sublayers', so anything set by hand in the Toolkit wins over the generated values. Materials
   the mod layer already overrides are left out of the generated layer altogether.

Remix ray traces a draw as a decal (layered over the surface under it instead of z-fighting with
it) only if it is tagged in rtx.decalTextures and alpha blended. Tagged materials the game draws
opaque (solid or masked) get an alpha blend in their material instead of the game's.

Rerunning regenerates generated_materials.usda only. Textures whose ingested copy was made from
the same file are not ingested again.

Usage: python apply.py [--bake build/material_bake] [--project remix-project] [--api URL]
                       [--rtx-conf game/Binaries/rtx.conf]
"""

import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from pxr import Sdf

REPO = Path(__file__).resolve().parents[2]
API = "http://127.0.0.1:8011"
MEDIA_TYPE = "application/lightspeed.remix.service+json; version=1.0"
GENERATED_LAYER = "generated_materials.usda"
# Remix emission is brighter-scaled than the game's; tune if glowing surfaces look off
EMISSIVE_SCALE = 1.0

# Baked texture kind -> (ingestion texture type, AperturePBR_Opacity input)
KINDS = {
    "albedo": ("DIFFUSE", "diffuse_texture"),
    "normal": ("NORMAL_DX", "normalmap_texture"),
    "roughness": ("ROUGHNESS", "reflectionroughness_texture"),
    "emissive": ("EMISSIVE", "emissive_mask_texture"),
}


# --- Toolkit REST API ---

def request(api, method, path, body=None, timeout=60):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": MEDIA_TYPE, "Accept": MEDIA_TYPE} if not path.startswith("/openapi") else {}
    req = urllib.request.Request(api + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            text = response.read()
            return json.loads(text) if text else None
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"{method} {path}: HTTP {error.code}: {error.read()[:500]!r}") from None


def material_queue_template(api):
    """The default request of the material ingestion queue, from the API's own schema."""
    schema = request(api, "GET", "/openapi.json")["components"]["schemas"]["AddMaterialItemToQueue"]
    return {name: field["default"] for name, field in schema["properties"].items() if "default" in field}


def ingest(api, template, files, output_directory):
    """files: [(path, texture type)]"""
    body = json.loads(json.dumps(template))
    data = body["context_plugin"]["data"]
    data["input_files"] = [[str(path).replace("\\", "/"), kind] for path, kind in files]
    data["output_directory"] = str(output_directory).replace("\\", "/")
    request(api, "POST", "/ingestcraft/mass-validator/queue/material", body, timeout=900)


def ingested_file(output_directory, baked):
    """The ingestion output of a baked PNG: <name>.<x>.rtex.dds, normals with _OTH_Normal added."""
    matches = sorted(output_directory.glob(baked.stem + "*.rtex.dds"))
    return matches[0] if matches else None


class IngestRecord:
    """Which baked file each ingested texture was made from, kept in the bake directory. File times
    do not tell (ingestion skips rewriting identical output), and the src_hash ingestion writes to
    its .meta is of an intermediate file for normal maps."""

    def __init__(self, path):
        self.path = path
        self.hashes = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}

    def is_current(self, output_directory, baked):
        return (ingested_file(output_directory, baked) is not None
                and self.hashes.get(baked.name) == hashlib.md5(baked.read_bytes()).hexdigest())

    def add(self, baked):
        self.hashes[baked.name] = hashlib.md5(baked.read_bytes()).hexdigest()
        self.path.write_text(json.dumps(self.hashes, indent=1, sort_keys=True), encoding="utf-8")


# --- rtx.conf ---

def texture_list(rtx_conf, option):
    """The texture hashes of a hash-list option (e.g. rtx.decalTextures), as 16 upper-case hex digits.
    Remix writes hashes with the top bit set as negative numbers."""
    if not rtx_conf.exists():
        return set()
    for line in rtx_conf.read_text(encoding="utf-8").splitlines():
        name, _, value = line.partition("=")
        if name.strip() == option:
            return {f"{int(item.strip(), 16) % (1 << 64):016X}" for item in value.split(",") if item.strip()}
    return set()


# --- Layers ---

def hand_overridden(mod_layer):
    """Material hashes the mod layer overrides itself."""
    looks = mod_layer.GetPrimAtPath("/RootNode/Looks")
    return {prim.name[4:] for prim in looks.nameChildren if prim.name.startswith("mat_")} if looks else set()


def write_generated_layer(path, materials):
    """materials: [(hash, {input name: (Sdf type, value)})]"""
    layer = Sdf.Layer.CreateNew(str(path)) if not path.exists() else Sdf.Layer.FindOrOpen(str(path))
    layer.Clear()
    layer.customLayerData = {"lightspeed_layer_type": "replacement", "lightspeed_game_name": "RoboBlitz"}
    layer.comment = "Generated by tools/material_bake/apply.py; rerunning overwrites it. Edit in mod.usda instead."
    for texture_hash, inputs in materials:
        shader = Sdf.CreatePrimInLayer(layer, f"/RootNode/Looks/mat_{texture_hash}/Shader")
        for prim_path in (shader.path, shader.path.GetParentPath(), shader.path.GetParentPath().GetParentPath(),
                          Sdf.Path("/RootNode")):
            layer.GetPrimAtPath(prim_path).specifier = Sdf.SpecifierOver
        for name, (value_type, value) in inputs.items():
            attribute = Sdf.AttributeSpec(shader, f"inputs:{name}", value_type)
            attribute.default = value
    layer.Save()


def insert_sublayer(api, mod_path, generated_path, toolkit_running):
    """Adds the generated layer below the mod layer's own opinions, once."""
    mod_layer = Sdf.Layer.FindOrOpen(str(mod_path))
    if any(Path(mod_layer.ComputeAbsolutePath(p)).resolve() == generated_path.resolve()
           for p in mod_layer.subLayerPaths):
        return "already a sublayer of mod.usda"
    if toolkit_running:
        request(api, "POST", "/stagecraft/layers/", {
            "layer_path": str(generated_path),
            "layer_type": "replacement",
            "parent_layer_id": str(mod_path),
            "create_or_insert": False,
            "sublayer_position": -1,
        })
        request(api, "POST", f"/stagecraft/layers/{urllib.parse.quote(str(mod_path), safe='')}/save")
        return "added as a sublayer of mod.usda (through the Toolkit)"
    mod_layer.subLayerPaths.append("./" + generated_path.name)
    mod_layer.Save()
    return "added as a sublayer of mod.usda"


# --- Main ---

def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--bake", type=Path, default=REPO / "build" / "material_bake")
    parser.add_argument("--project", type=Path, default=REPO / "remix-project")
    parser.add_argument("--api", default=API)
    parser.add_argument("--rtx-conf", type=Path, default=REPO / "game" / "Binaries" / "rtx.conf",
                        help="for rtx.decalTextures")
    parser.add_argument("--skip-ingest", action="store_true", help="only (re)write the generated layer")
    args = parser.parse_args()

    bake = args.bake
    if not (bake / "report.html").exists():
        sys.exit(f"No bake in {bake}; run bake.py first")
    project = args.project.resolve()
    mod_path = project / "mod.usda"
    output_directory = project / "assets" / "ingested"
    if not mod_path.exists():
        sys.exit(f"No mod.usda in {project}")
    print(f"Bake    {bake}\nProject {project}")

    try:
        loaded = request(args.api, "GET", "/stagecraft/project/", timeout=5)
        toolkit_running = True
        if loaded and Path(loaded["layer_id"]).resolve().parent != project:
            sys.exit(f"The Toolkit has another project open: {loaded['layer_id']}")
    except (urllib.error.URLError, OSError):
        toolkit_running = False
    if not toolkit_running and not args.skip_ingest:
        sys.exit(f"The Toolkit is not reachable at {args.api}: open the project in it, or use --skip-ingest")

    skipped = hand_overridden(Sdf.Layer.FindOrOpen(str(mod_path)))
    decals = texture_list(args.rtx_conf, "rtx.decalTextures")
    blended_decals = []
    materials, missing = [], []
    template = material_queue_template(args.api) if not args.skip_ingest else None
    record = IngestRecord(bake / "ingested.json")
    directories = sorted(d for d in bake.iterdir() if (d / "material.json").exists())
    for number, directory in enumerate(directories, 1):
        texture_hash = directory.name
        if texture_hash in skipped:
            continue
        info = json.loads((directory / "material.json").read_text(encoding="utf-8"))
        baked = {kind: directory / info[kind] for kind in KINDS if info.get(kind)}

        stale = [(path, KINDS[kind][0]) for kind, path in baked.items()
                 if not record.is_current(output_directory, path)]
        if stale and not args.skip_ingest:
            started = time.time()
            ingest(args.api, template, stale, output_directory)
            for path, _ in stale:
                record.add(path)
            print(f"[{number}/{len(directories)}] {texture_hash}: ingested {len(stale)} textures "
                  f"({time.time() - started:.0f} s)")

        inputs = {}
        for kind, path in baked.items():
            out = ingested_file(output_directory, path)
            if not out:
                missing.append(f"{texture_hash} {kind}")
                continue
            relative = "./" + out.relative_to(project).as_posix()
            inputs[KINDS[kind][1]] = (Sdf.ValueTypeNames.Asset, Sdf.AssetPath(relative))
        if "emissive_mask_texture" in inputs:
            inputs["enable_emission"] = (Sdf.ValueTypeNames.Bool, True)
            inputs["emissive_intensity"] = (Sdf.ValueTypeNames.Float,
                                            float(info.get("emissiveIntensity", 1.0)) * EMISSIVE_SCALE)
        if inputs and texture_hash in decals and info.get("blend") in ("solid", "masked"):
            # blend_type defaults to Alpha: the albedo alpha (1, or the mask) is the opacity
            inputs["use_legacy_alpha_state"] = (Sdf.ValueTypeNames.Bool, False)
            inputs["blend_enabled"] = (Sdf.ValueTypeNames.Bool, True)
            blended_decals.append(texture_hash)
        if inputs:
            materials.append((texture_hash, inputs))

    generated_path = project / GENERATED_LAYER
    write_generated_layer(generated_path, materials)
    print(f"Wrote {generated_path.name}: {len(materials)} materials")
    if blended_decals:
        print(f"Alpha blended as decals: {', '.join(blended_decals)}")
    print(insert_sublayer(args.api, mod_path, generated_path, toolkit_running))
    if skipped:
        print(f"Left to mod.usda (overridden by hand): {', '.join(sorted(skipped))}")
    if missing:
        print(f"Not ingested, left out: {', '.join(missing)}")
    if toolkit_running:
        print("If the Toolkit already had generated_materials.usda loaded, reload it there to see the changes.")


if __name__ == "__main__":
    main()
