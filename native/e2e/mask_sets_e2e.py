#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageStat

import mask_e2e
import vts_e2e

SIZE = 1440
FRAMES = 10
UNDER = ["Hiyori", "Mao", "akari"]
CROPS = {"face": (560, 840, 890, 1140), "shoulders": (400, 1140, 1040, 1440), "headdress": (360, 360, 900, 540)}
REFERENCE_LIMIT = 2.0
TOOL_SOURCE = r"""
#import <Foundation/Foundation.h>
#include <cstdio>
#include <cstdlib>
#include "engine.h"

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        engine* instance = engine_create(argv[1], atoi(argv[2]), atoi(argv[2]));
        if (instance == NULL)
        {
            fprintf(stderr, "engine_create failed\n");
            return 1;
        }
        engine_breath_enable(instance, false);
        engine_idle_motions_enable(instance, false);
        engine_expressions_stop(instance);
        const uint8_t* pixels = NULL;
        for (int frame = 0; frame < atoi(argv[3]); frame++)
        {
            engine_update(instance, 1000.0f / 30.0f, NULL, NULL, NULL);
            pixels = engine_render(instance, 0);
        }
        FILE* output = fopen(argv[4], "wb");
        fwrite(pixels, 1, (size_t)atoi(argv[2]) * atoi(argv[2]) * 4, output);
        fclose(output);
        engine_destroy(instance);
    }
    return 0;
}
"""


def stage(binary, side_dir):
    os.makedirs(side_dir)
    source = os.path.join(side_dir, "tool.mm")
    with open(source, "w") as handle:
        handle.write(TOOL_SOURCE)
    tool_path = os.path.join(side_dir, "tool")
    libs = mask_e2e.engine_libs(binary)
    frameworks = sum([["-framework", name] for name in ["Metal", "Foundation", "ImageIO", "CoreGraphics", "AppKit", "QuartzCore", "IOSurface"]], [])
    subprocess.run(["clang++", "-O2", "-std=c++17", "-fobjc-arc", "-I" + os.path.join(mask_e2e.REPO, "native", "engine"), source] + libs + frameworks + ["-o", tool_path], check=True, capture_output=True, timeout=300)
    return tool_path


def render(tool_path, model_path, out_path):
    done = subprocess.run([tool_path, model_path, str(SIZE), str(FRAMES), out_path], capture_output=True, text=True, timeout=300)
    if done.returncode != 0:
        raise RuntimeError("tool failed %d on %s: %s" % (done.returncode, model_path, done.stderr.strip()))
    with open(out_path, "rb") as reader:
        return reader.read()


def run(args, result, scratch):
    tools = {side: stage(binary, os.path.join(scratch, side)) for side, binary in (("main", args.main_bin), ("branch", args.branch_bin))}
    models = []
    for name in UNDER:
        try:
            models.append((name, mask_e2e.find_model(name), False))
        except RuntimeError as error:
            print("skip %s: %s" % (name, error))
    result.add("under36_controls_found", (len(models) > 0, "%d of %d under-36 models found" % (len(models), len(UNDER))))
    models.append(("over", args.over, True))
    reference = Image.open(args.reference).convert("RGBA")
    for name, path, over in models:
        frames = [render(tools[side], path, os.path.join(scratch, "%s_%s.rgba" % (name, side))) for side in ("main", "branch")]
        changed = sum(1 for index in range(0, len(frames[0]), 4) if frames[0][index:index + 4] != frames[1][index:index + 4])
        if args.out:
            for side, pixels in zip(("main", "branch"), frames):
                mask_e2e.png(os.path.join(args.out, "%s_%s.png" % (name, side)), pixels, SIZE, SIZE)
        if over:
            result.add("over36_masks_change", (changed > 0, "%d pixels differ from main" % changed))
            image = Image.frombytes("RGBA", (SIZE, SIZE), frames[1])
            for crop, box in CROPS.items():
                mean = sum(ImageStat.Stat(ImageChops.difference(image.crop(box), reference.crop(box))).mean) / 4
                result.add("over36_matches_reference_%s" % crop, (mean <= REFERENCE_LIMIT, "mean diff %.2f (limit %.1f)" % (mean, REFERENCE_LIMIT)))
        else:
            result.add("under36_identical_%s" % name, (changed == 0, "%d pixels differ from main" % changed))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--main-bin", required=True)
    parser.add_argument("--branch-bin", required=True)
    parser.add_argument("--over", required=True, help="model3.json with more than 36 mask sets")
    parser.add_argument("--reference", required=True, help="approved %dx%d png of the --over model" % (SIZE, SIZE))
    parser.add_argument("--out")
    args = parser.parse_args()
    for path in (args.over, args.reference):
        if not os.path.exists(path):
            parser.error("%s does not exist" % path)
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    result = vts_e2e.Result(args.out and os.path.join(args.out, "result.json"))
    scratch = tempfile.mkdtemp()
    try:
        run(args, result, scratch)
    finally:
        shutil.rmtree(scratch)
    return vts_e2e.finish(result)


if __name__ == "__main__":
    sys.exit(main())
