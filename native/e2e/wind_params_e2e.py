#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import sys
import time

import vts_e2e

PORT = 9327
SOURCE = "Mao"
FPS = vts_e2e.FPS
SETTLE_FRAMES = FPS
WIND_FRAMES = FPS * 8
HEAD_FRAMES = int(6.5345 * FPS)
HEAD_SETTLE = FPS // 2
HEAD_ORDER = (30, -30, -30, 30)
HAIR_ROWS = (40, 120)
HAIR_COLS = (40, 120)
HEAD_ROWS = (30, 90)
HEAD_COLS = (40, 120)
CONFIGS = {"calm": (50, 0), "windy": (50, 100), "max": (100, 100)}
WIND_GAIN = 2.5
PUSH_GAIN = 1.0
STRENGTH_GAIN = 0.5
SAME_TOLERANCE = 0.1
HEAD_MARGIN = 5.0
HOLD = {"ParamMouthOpenY": 0.6, "ParamEyeLOpen": 1, "ParamEyeROpen": 1, "ParamEyeLSmile": 0, "ParamEyeRSmile": 0, "ParamBrowLY": 0, "ParamBrowRY": 0, "ParamCheek": 0, "ParamAngleX": 0, "ParamAngleY": 0, "ParamAngleZ": 0, "ParamBodyAngleX": 0, "ParamBreath": 0}
FACE_ROWS = (80, 110)
FACE_COLS = (64, 98)
HOLD_FRAMES = FPS * 2
HOLD_LIMIT = 1.0
HOLD_MARGIN = 2.0
WAKE_FRAMES = FPS // 2
WAKE_RATIO = 4.0
GRAVITY_MARGIN = 2.0
FLOOD_CALLS = 2000
FLOOD_RSS_KB = 2048


def crop(image, rows, cols):
    stride = vts_e2e.WIDTH * 3
    pixels = []
    for y in range(*rows):
        pixels.extend(image[y * stride + cols[0] * 3:y * stride + cols[1] * 3])
    return pixels


def crop_drift(images):
    crops = [crop(image, HAIR_ROWS, HAIR_COLS) for image in images]
    pairs = list(zip(crops, crops[FPS // 2:]))
    if not pairs:
        return 0.0
    return sum(sum(abs(a - b) for a, b in zip(first, second)) for first, second in pairs) / (len(pairs) * len(crops[0]))


def make_models(targets):
    settings = {key: {"PhysicsSettings": {"Use": True, "PhysicsStrength": strength, "WindStrength": wind}} for key, (strength, wind) in CONFIGS.items()}
    settings["still"] = {"PhysicsSettings": {"Use": True, "PhysicsStrength": 0, "WindStrength": 0}, "ParameterSettings": [{"Name": output, "Input": vts, "OutputLive2D": output} for vts, output in vts_e2e.ROUTES]}
    for key, vtube in settings.items():
        target = os.path.join(vts_e2e.REPO, "models", "windparams_" + key)
        os.mkdir(target)
        targets.append(target)
        vts_e2e.make_copy(target, "windparams_" + key, SOURCE, vtube, still=True)


def frames_for(port, key, count):
    vts_e2e.switch_model(port, "windparams_" + key)
    vts_e2e.idle_frames(port, SETTLE_FRAMES)
    return vts_e2e.idle_frames(port, count)[1]


def post_params(port, body):
    status, payload = vts_e2e.http_call(port, "POST", "/params", json.dumps(body).encode())
    text = payload.decode(errors="replace").strip()
    return status, json.loads(text) if status == 200 else text


def post_physics(port, body):
    status, payload = vts_e2e.http_call(port, "POST", "/physics", body if isinstance(body, bytes) else json.dumps(body).encode())
    text = payload.decode(errors="replace").strip()
    return status, json.loads(text) if status == 200 else text


def physics_frames(port, body, count):
    post_physics(port, body)
    vts_e2e.idle_frames(port, SETTLE_FRAMES)
    return vts_e2e.idle_frames(port, count)[1]


def check_physics_errors(port):
    cases = [(b"[1]", "needs a JSON object"), (b"nope", "needs a JSON object"), (b'{"wind": "high"}', "wind must be a number"), (b'{"strength": null}', "strength must be a number"), (b'{"gravity": true}', "gravity must be a number"), (b'{"wind": 1e39}', "wind must be a number"), (b'{"tilt": 1}', "unknown physics key tilt")]
    replies = [post_physics(port, body) for body, _ in cases]
    ok = all(reply[0] == 400 and want in reply[1] for reply, (_, want) in zip(replies, cases))
    return ok, "400 cases=%d statuses=%s" % (len(cases), [reply[0] for reply in replies])


def check_physics_partial(port):
    first = post_physics(port, {"wind": 100})
    second = post_physics(port, {"gravity": 90})
    ok = first[0] == 200 and second == (200, {"strength": 50, "wind": 100, "gravity": 90})
    return ok, "after wind=100 then gravity=90 reply=%s" % (second,)


def check_physics_visible(port):
    vts_e2e.switch_model(port, "windparams_calm")
    most = crop_drift(physics_frames(port, {"strength": 100, "wind": 100, "gravity": 0}, WIND_FRAMES))
    windy = crop_drift(physics_frames(port, {"strength": 50}, WIND_FRAMES))
    calm = crop_drift(physics_frames(port, {"wind": 0}, WIND_FRAMES))
    down = [crop(image, HAIR_ROWS, HAIR_COLS) for image in physics_frames(port, {"gravity": 0}, FPS * 2)]
    side = [crop(image, HAIR_ROWS, HAIR_COLS) for image in physics_frames(port, {"gravity": 90}, FPS * 2)]
    post_physics(port, {"gravity": 0})
    tilt = vts_e2e.image_diff(vts_e2e.mean_image(down), vts_e2e.mean_image(side))
    control = vts_e2e.image_diff(vts_e2e.mean_image(down[:FPS]), vts_e2e.mean_image(down[FPS:]))
    runs = []
    for strength in (100, 100, 300):
        vts_e2e.switch_model(port, "windparams_calm")
        vts_e2e.switch_model(port, "windparams_windy")
        runs.append(vts_e2e.mean_image([crop(image, HAIR_ROWS, HAIR_COLS) for image in physics_frames(port, {"strength": strength}, WIND_FRAMES)]))
    repeat = vts_e2e.image_diff(runs[0], runs[1])
    past = vts_e2e.image_diff(runs[0], runs[2])
    vts_e2e.switch_model(port, "windparams_calm")
    wind_ok = most > calm + WIND_GAIN and windy > calm and past > repeat + PUSH_GAIN
    gravity_ok = tilt > control + GRAVITY_MARGIN
    return (wind_ok, "hair crop drift post wind=100=%.3f wind=0=%.3f strength=100+wind=100=%.3f need wind100 > wind0 and max > wind0 + %.2f; hair crop strength 300 vs 100=%.3f repeat 100 vs 100=%.3f need > repeat + %.1f" % (windy, calm, most, WIND_GAIN, past, repeat, PUSH_GAIN)), (gravity_ok, "hair crop mean diff gravity 90 vs 0=%.3f control=%.3f need > control + %.1f" % (tilt, control, GRAVITY_MARGIN))


def check_physics_switch(port):
    vts_e2e.switch_model(port, "windparams_calm")
    set_reply = post_physics(port, {"strength": 80, "wind": 30, "gravity": 45})
    vts_e2e.switch_model(port, "windparams_windy")
    vts_e2e.idle_frames(port, 2)
    reset = post_physics(port, {})
    ok = set_reply[0] == 200 and reset == (200, {"strength": 50, "wind": 100, "gravity": 0})
    return ok, "set=%s after switch to windparams_windy reply=%s" % (set_reply, reset)


def check_physics_flood(port, pid):
    vts_e2e.idle_frames(port, 2)
    for _ in range(50):
        post_physics(port, {"wind": 0})
    before = vts_e2e.process_usage(pid)[0]
    statuses = []
    started = time.monotonic()
    for index in range(FLOOD_CALLS):
        statuses.append(post_physics(port, {"wind": index % 100, "gravity": index % 360})[0])
    elapsed = time.monotonic() - started
    vts_e2e.idle_frames(port, 2)
    after = vts_e2e.process_usage(pid)[0]
    ok = statuses.count(200) == FLOOD_CALLS and after - before <= FLOOD_RSS_KB
    return ok, "calls=%d ok=%d rps=%.0f rss_kb before=%d after=%d delta=%d need <=%d" % (FLOOD_CALLS, statuses.count(200), FLOOD_CALLS / elapsed, before, after, after - before, FLOOD_RSS_KB)


def check_physics(port, pid, result):
    vts_e2e.switch_model(port, "windparams_calm")
    vts_e2e.idle_frames(port, 2)
    result.add("h_physics_400s", check_physics_errors(port))
    result.add("i_physics_partial_keeps", check_physics_partial(port))
    wind, gravity = check_physics_visible(port)
    result.add("j_physics_wind_visible", wind)
    result.add("k_physics_gravity_visible", gravity)
    result.add("l_physics_switch_resets", check_physics_switch(port))
    result.add("m_physics_flood_cheap", check_physics_flood(port, pid))


def head_frames(port, value):
    post_params(port, {"ParamAngleX": value})
    vts_e2e.idle_frames(port, HEAD_SETTLE)
    return vts_e2e.idle_frames(port, HEAD_FRAMES)[1]


def check_api(port):
    unknown = post_params(port, {"NoSuchParam": 1, "ParamAngleX": 1})
    word = post_params(port, {"ParamAngleX": "left"})
    clamped = post_params(port, {"ParamAngleX": 999, "ParamAngleY": 5})
    released = post_params(port, {"ParamAngleY": None, "ParamAngleX": None})
    ok = unknown[0] == 400 and "NoSuchParam" in unknown[1] and "ParamAngleX" not in unknown[1] and word[0] == 400 and clamped[0] == 200 and 1 < clamped[1].get("ParamAngleX", 999) < 999 and clamped[1].get("ParamAngleY") == 5 and released == (200, {})
    return ok, "unknown=%s word=%s clamped=%s released=%s" % (unknown, word, clamped, released)


def face_frames(port, face, hold):
    post_params(port, hold)
    vts_e2e.http_call(port, "POST", "/state?face=" + face)
    vts_e2e.idle_frames(port, HEAD_SETTLE)
    if face == "neutral":
        return vts_e2e.idle_frames(port, HOLD_FRAMES)[1]
    return vts_e2e.render_images(port, vts_e2e.synth_pcm())[1]


def check_hold(port):
    vts_e2e.switch_model(port, "windparams_still")
    released = {key: None for key in HOLD}
    spreads = []
    for hold in (HOLD, released):
        still = vts_e2e.mean_image([crop(image, FACE_ROWS, FACE_COLS) for image in face_frames(port, "neutral", hold)])
        talking = [crop(image, FACE_ROWS, FACE_COLS) for image in face_frames(port, "laughing", hold)]
        spreads.append(sum(vts_e2e.image_diff(image, still) for image in talking) / max(len(talking), 1))
    vts_e2e.http_call(port, "POST", "/state?face=neutral")
    ok = spreads[0] < HOLD_LIMIT and spreads[1] > spreads[0] + HOLD_MARGIN
    return ok, "face crop laughing+talking vs neutral idle diff held=%.3f need <%.2f released=%.3f need > held + %.1f" % (spreads[0], HOLD_LIMIT, spreads[1], HOLD_MARGIN)


def check_wake_hold(port):
    post_params(port, dict(HOLD, ParamAngleX=-30))
    vts_e2e.http_call(port, "POST", "/action?action=sleep")
    asleep = vts_e2e.idle_frames(port, FPS)[1]
    vts_e2e.http_call(port, "POST", "/state?face=neutral")
    post_params(port, {"ParamAngleX": 30})
    waking = [crop(image, HEAD_ROWS, HEAD_COLS) for image in vts_e2e.idle_frames(port, WAKE_FRAMES)[1]]
    held = vts_e2e.mean_image([crop(image, HEAD_ROWS, HEAD_COLS) for image in vts_e2e.idle_frames(port, HOLD_FRAMES)[1][-FPS:]])
    post_params(port, {key: None for key in HOLD})
    turn = vts_e2e.image_diff(vts_e2e.mean_image([crop(image, HEAD_ROWS, HEAD_COLS) for image in asleep[-FPS // 2:]]), held)
    worst = max(vts_e2e.image_diff(image, held) for image in waking)
    ok = len(waking) == WAKE_FRAMES and turn > HEAD_MARGIN and worst < turn / WAKE_RATIO
    return ok, "head crop ParamAngleX -30 asleep vs 30 held=%.3f worst wake-carry frame vs held=%.3f need < turn / %.1f and turn > %.1f" % (turn, worst, WAKE_RATIO, HEAD_MARGIN)


def serve(binary, port, log_path, keys, params, result):
    handle = vts_e2e.start_server(binary, port, log_path, model="windparams_calm", settings={"idle_actions": []})
    try:
        frames = {key: frames_for(port, key, WIND_FRAMES) for key in keys}
        if not params:
            return frames, None, None, None, None
        check_physics(port, handle["proc"].pid, result)
        api = check_api(port)
        heads = [head_frames(port, value) for value in HEAD_ORDER]
        post_params(port, {"ParamAngleX": None})
        return frames, api, heads, check_hold(port), check_wake_hold(port)
    finally:
        vts_e2e.stop_server(handle)


def run(args, result):
    targets = []
    try:
        make_models(targets)
        frames, api, heads, hold, wake = serve(args.binary, args.port, args.out + ".log", list(CONFIGS), True, result)
        main_frames, _, _, _, _ = serve(args.main_binary, args.port, args.out + ".main.log", ["calm"], False, result)
    finally:
        for target in targets:
            shutil.rmtree(target, ignore_errors=True)
    drift = {key: crop_drift(images) for key, images in frames.items()}
    main_calm = crop_drift(main_frames["calm"])
    detail = "hair crop drift calm=%.3f windy=%.3f max=%.3f main_calm=%.3f" % (drift["calm"], drift["windy"], drift["max"], main_calm)
    result.add("a_wind_blows_hair", (drift["max"] > drift["calm"] + WIND_GAIN and drift["windy"] > drift["calm"], "%s need max > calm + %.2f" % (detail, WIND_GAIN)))
    result.add("b_strength_max_moves_more", (drift["max"] > drift["windy"] + STRENGTH_GAIN, "%s need max > windy + %.2f" % (detail, STRENGTH_GAIN)))
    same = vts_e2e.image_diff(vts_e2e.mean_image(frames["calm"]), vts_e2e.mean_image(main_frames["calm"]))
    result.add("c_wind0_matches_main", (same < SAME_TOLERANCE and abs(drift["calm"] - main_calm) < SAME_TOLERANCE, "whole-frame mean diff=%.4f %s need both under %.2f" % (same, detail, SAME_TOLERANCE)))
    result.add("d_params_api", api)
    crops = [[crop(image, HEAD_ROWS, HEAD_COLS) for image in images] for images in heads]
    right = vts_e2e.mean_image(crops[0] + crops[3])
    left = vts_e2e.mean_image(crops[1] + crops[2])
    turn = vts_e2e.image_diff(right, left)
    control = vts_e2e.image_diff(vts_e2e.mean_image(crops[0]), vts_e2e.mean_image(crops[3]))
    result.add("e_params_turn_head", (all(len(images) == HEAD_FRAMES for images in heads) and turn > control + HEAD_MARGIN, "head crop diff ParamAngleX 30 vs -30=%.3f 30 first vs last window=%.3f need turn > control + %.1f" % (turn, control, HEAD_MARGIN)))
    result.add("f_params_hold_over_face_and_mouth", hold)
    result.add("g_params_hold_during_wake_carry", wake)
    result.payload["drift"] = dict(drift, main_calm=main_calm)
    result.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--main-binary", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    if args.port == vts_e2e.LIVE_PORT:
        parser.error("port %d is the live service" % vts_e2e.LIVE_PORT)
    result = vts_e2e.Result(args.out)
    try:
        run(args, result)
    except BaseException as error:
        result.payload["error"] = "%s: %s" % (type(error).__name__, error)
        result.flush()
        raise
    return vts_e2e.finish(result)


if __name__ == "__main__":
    sys.exit(main())
