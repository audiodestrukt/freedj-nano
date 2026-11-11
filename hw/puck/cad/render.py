# SPDX-FileCopyrightText: 2026 Dan Newcome
# SPDX-License-Identifier: CERN-OHL-S-2.0
"""render.py: photoreal render of build/deck*.glb in headless Blender (from freedj-esp's deck_render.py).

LC_ALL=C LANG=C blender -b -P render.py -- IN.glb OUT.png [--az 35] [--el 32]
    [--dist 2.6] [--res 1600x1100] [--samples 160] [--fstop 16] [--target x,y,z (m, Blender frame)]
Adapted from the product-design skill's blender_render.py: dark studio, real floor,
emission boosted so the LCD and light rings read as lit.
"""
import bpy, sys, math
from mathutils import Vector

def args():
    a = sys.argv[sys.argv.index("--") + 1:]
    o = dict(inp=a[0], out=a[1], az=35.0, el=32.0, dist=2.6, res="1600x1100", samples=160, target=None, lens=60.0, exposure=-1.4)
    i = 2
    while i < len(a):
        k = a[i][2:]; v = a[i + 1]; i += 2
        o[k] = v if k in ("res", "target") else float(v)
    return o

def aim(ob, target):
    ob.rotation_euler = (Vector(target) - ob.location).to_track_quat("-Z", "Y").to_euler()

def area(name, loc, energy, size, target, color=(1, 1, 1)):
    d = bpy.data.lights.new(name, "AREA"); d.energy = energy; d.size = size; d.color = color
    ob = bpy.data.objects.new(name, d); ob.location = loc
    bpy.context.collection.objects.link(ob); aim(ob, target)

def main():
    o = args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    bpy.ops.import_scene.gltf(filepath=o["inp"])
    model = [ob for ob in sc.objects if ob.type == "MESH"]
    lo = Vector((1e9,) * 3); hi = Vector((-1e9,) * 3)
    for ob in model:
        for c in ob.bound_box:
            w = ob.matrix_world @ Vector(c); lo = Vector(map(min, lo, w)); hi = Vector(map(max, hi, w))
    center = (lo + hi) / 2; r = (hi - lo).length / 2
    target = Vector(tuple(float(t) for t in o["target"].split(","))) if o["target"] else center

    # emission: the glTF importer brings it in at strength 1, too dim next to studio lights
    for mat in bpy.data.materials:
        if not mat.use_nodes: continue
        for n in mat.node_tree.nodes:
            if n.type == "BSDF_PRINCIPLED" and "Emission Strength" in n.inputs:
                # only materials the GLB marked emissive (strength > 0; the default colour is white)
                if n.inputs["Emission Strength"].default_value > 0 and (
                        n.inputs["Emission Color"].is_linked or any(c > 0.01 for c in n.inputs["Emission Color"].default_value[:3])):
                    n.inputs["Emission Strength"].default_value = 2.2 if mat.name == "screen" else 6.0
            if mat.name == "touch" and n.type == "BSDF_PRINCIPLED" and "Anisotropic" in n.inputs:
                n.inputs["Anisotropic"].default_value = 0.6   # brushed look

    w = bpy.data.worlds.new("W"); sc.world = w; w.use_nodes = True
    bg = w.node_tree.nodes["Background"]; bg.inputs[0].default_value = (0.035, 0.037, 0.042, 1); bg.inputs[1].default_value = 1.0

    area("key", center + Vector((r * 1.2, -r * 1.4, r * 2.2)), 260 * r * r, r * 2.0, center)
    area("fill", center + Vector((-r * 1.8, -r * 0.6, r * 1.0)), 70 * r * r, r * 3.0, center, (0.85, 0.9, 1.0))
    area("rim", center + Vector((-r * 0.4, r * 2.0, r * 1.4)), 180 * r * r, r * 1.6, center, (1.0, 0.92, 0.85))
    area("top", center + Vector((0, 0, r * 3.0)), 60 * r * r, r * 4.0, center)

    bpy.ops.mesh.primitive_plane_add(size=r * 40, location=(center.x, center.y, lo.z))
    floor = bpy.context.active_object
    fm = bpy.data.materials.new("floor"); fm.use_nodes = True
    p = fm.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (0.045, 0.046, 0.05, 1); p.inputs["Roughness"].default_value = 0.35
    floor.data.materials.append(fm)

    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam")); sc.collection.objects.link(cam); sc.camera = cam
    cam.data.lens = o["lens"]; cam.data.clip_start = 0.001; cam.data.clip_end = 100
    az, el, d = math.radians(o["az"]), math.radians(o["el"]), r * o["dist"]
    cam.location = target + Vector((d * math.cos(el) * math.sin(az), -d * math.cos(el) * math.cos(az), d * math.sin(el)))
    aim(cam, target)
    cam.data.dof.use_dof = True; cam.data.dof.focus_distance = (cam.location - target).length; cam.data.dof.aperture_fstop = o.get("fstop", 16)

    sc.render.engine = "CYCLES"; sc.cycles.samples = int(o["samples"]); sc.cycles.use_denoising = True
    prefs = bpy.context.preferences.addons["cycles"].preferences
    chosen = None
    for backend in ("OPTIX", "CUDA", "METAL"):   # a backend this build lacks raises on assignment
        try: prefs.compute_device_type = backend; prefs.refresh_devices()
        except Exception: continue
        if any(dv.type == backend for dv in prefs.devices):
            for dv in prefs.devices: dv.use = (dv.type == backend)
            chosen = backend; break
    sc.cycles.device = "GPU" if chosen else "CPU"
    print("CYCLES device:", chosen or "CPU")
    rx, ry = (int(v) for v in o["res"].split("x"))
    sc.render.resolution_x, sc.render.resolution_y = rx, ry
    sc.view_settings.view_transform = "AgX"; sc.view_settings.exposure = o.get("exposure", -1.4)
    try: sc.view_settings.look = "AgX - Medium High Contrast"
    except Exception: pass
    sc.render.filepath = o["out"]
    bpy.ops.render.render(write_still=True)
    print("WROTE", o["out"])

main()
