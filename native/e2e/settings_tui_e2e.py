#!/usr/bin/env python3
import argparse
import struct
import json
import os
import pty
import re
import select
import shutil
import signal
import subprocess
import sys
import zlib
import time

import vts_e2e

PORT = 9371
MODELS = ("Haru", "Hiyori")
UP = b"\x1b[A"
DOWN = b"\x1b[B"
RIGHT = b"\x1b[C"
ESCAPE = b"\x1b"
ENTER = b"\r"
SCREEN_SECONDS = 20
EXIT_SECONDS = 10
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


class Terminal:
    def __init__(self, binary, port, transcript):
        self.leader, follower = pty.openpty()
        try:
            self.proc = subprocess.Popen([binary, "--settings", "--port", str(port)], stdin=follower, stdout=follower, stderr=follower, start_new_session=True)
        except BaseException:
            os.close(self.leader)
            raise
        finally:
            os.close(follower)
        self.screen = b""
        self.transcript = transcript

    def read(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            ready = select.select([self.leader], [], [], max(0.0, deadline - time.monotonic()))[0]
            if not ready:
                return
            try:
                chunk = os.read(self.leader, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.transcript.write(chunk)
            self.screen = (self.screen + chunk)[-65536:]

    def wait_for(self, text, seconds=SCREEN_SECONDS):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if text.encode() in ANSI.sub(b"", self.screen.rsplit(b"\x1b[2J", 1)[-1]):
                return True
            self.read(0.2)
        return False

    def send(self, *keys):
        for key in keys:
            os.write(self.leader, key)
            self.read(0.15)

    def wait_exit(self, seconds):
        deadline = time.monotonic() + seconds
        while self.proc.poll() is None and time.monotonic() < deadline:
            self.read(0.2)
        self.read(0.5)
        return self.proc.poll()

    def stop(self):
        if self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.proc.wait(timeout=10)
        if self.leader >= 0:
            os.close(self.leader)
            self.leader = -1


def settings_processes(handle, binary, port):
    run_binary = os.path.join(handle["run_dir"], os.path.basename(binary))
    pattern = "^" + re.escape(run_binary) + " --settings --port " + str(port) + "$"
    deadline = time.monotonic() + 3
    while True:
        found = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True)
        if found.returncode not in (0, 1):
            raise RuntimeError(found.stderr)
        children = [int(pid) for pid in found.stdout.split()]
        if children or time.monotonic() >= deadline:
            return children
        time.sleep(0.1)


def check_auto_settings(args, result):
    for window_args, auto_settings, name in [((), True, "plain_launch_opens_settings"), ((), False, "headless_no_settings"), (("--window",), False, "window_no_settings")]:
        handle = vts_e2e.start_server(args.binary, args.port, args.out + "." + name + ".log", args=window_args, model=MODELS[0], auto_settings=auto_settings)
        children = []
        try:
            children = settings_processes(handle, args.binary, args.port)
            result.add(name, (bool(children) == auto_settings, "settings processes within 3s: %s" % children))
        finally:
            for child in children:
                try:
                    os.kill(child, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            vts_e2e.stop_server(handle)


def settings(port):
    status, body = vts_e2e.http_call(port, "GET", "/settings")
    return json.loads(body) if status == 200 else {}


def saved_file(handle):
    with open(os.path.join(handle["run_dir"], "settings.json")) as reader:
        return json.load(reader)


def tiny_png(path, red):
    raw = b"\x00" + bytes((red, 0, 0))
    chunks = [(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)), (b"IDAT", zlib.compress(raw)), (b"IEND", b"")]
    with open(path, "wb") as writer:
        writer.write(b"\x89PNG\r\n\x1a\n")
        for kind, data in chunks:
            writer.write(struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data)))


def run(args, result, transcript):
    copies = []
    handle = None
    terminal = None
    try:
        for name in MODELS:
            target = os.path.join(vts_e2e.REPO, "models", "settingstui_" + name)
            if os.path.exists(target):
                raise RuntimeError("%s already exists" % target)
            copies.append(target)
            shutil.copytree(os.path.join(vts_e2e.SAMPLES, name), target)
        handle = vts_e2e.start_server(args.binary, args.port, args.out + ".log", model=MODELS[0], settings={"idle_actions": [], "action_every": 15})
        bg_dir = os.path.join(handle["run_dir"], "background")
        os.makedirs(bg_dir, exist_ok=True)
        tiny_png(os.path.join(bg_dir, "a.png"), 200)
        tiny_png(os.path.join(bg_dir, "b.png"), 100)
        run_binary = os.path.join(handle["run_dir"], os.path.basename(args.binary))
        terminal = Terminal(run_binary, args.port, transcript)
        result.add("tui_draws", (terminal.wait_for("hikari settings") and terminal.wait_for(MODELS[0]), "first screen shows model %s" % MODELS[0]))

        terminal.send(ENTER)
        picker = terminal.wait_for("(●) " + MODELS[0])
        terminal.send(DOWN, ENTER)
        terminal.wait_for("switched to " + MODELS[1])
        result.add("model_picker", (picker and settings(args.port).get("model") == MODELS[1] and saved_file(handle).get("model") == MODELS[1], "picker marked %s, GET %s file %s" % (MODELS[0], settings(args.port).get("model"), saved_file(handle).get("model"))))

        old_face = settings(args.port).get("face")
        terminal.send(DOWN, ENTER, DOWN, ENTER)
        terminal.wait_for("face is")
        face = settings(args.port).get("face")
        result.add("face_picker", (face != old_face and saved_file(handle).get("face") == face, "GET %s -> %s file %s" % (old_face, face, saved_file(handle).get("face"))))

        terminal.send(DOWN, ENTER)
        listed = terminal.wait_for("a.png") and terminal.wait_for("b.png")
        terminal.send(DOWN, ENTER)
        terminal.wait_for("background is a.png")
        tiny_png(os.path.join(bg_dir, "c.png"), 50)
        terminal.send(ENTER)
        refreshed = terminal.wait_for("c.png")
        terminal.send(ESCAPE)
        background = json.loads(vts_e2e.http_call(args.port, "GET", "/background")[1])
        result.add("background_picker", (listed and refreshed and background.get("name") == "a.png" and background.get("type") == "image/png", "listed %s, refreshed with c.png %s, GET /background %s" % (listed, refreshed, background.get("name"))))

        held = json.loads(vts_e2e.http_call(args.port, "GET", "/state")[1])["face"]
        terminal.send(DOWN, ENTER, DOWN, ENTER)
        terminal.wait_for("holding")
        now = json.loads(vts_e2e.http_call(args.port, "GET", "/state")[1])["face"]
        result.add("state_now_live_not_saved", (now != held and terminal.wait_for("holding " + now) and saved_file(handle).get("face") == face, "held %s -> %s, file face %s" % (held, now, saved_file(handle).get("face"))))

        pose = json.loads(vts_e2e.http_call(args.port, "GET", "/state")[1])["pose"]
        terminal.send(DOWN, ENTER, DOWN, ENTER)
        terminal.wait_for("pose ")
        new_pose = json.loads(vts_e2e.http_call(args.port, "GET", "/state")[1])["pose"]
        result.add("pose_now_live_not_saved", (new_pose != pose and "pose" not in saved_file(handle), "pose %s -> %s" % (pose, new_pose)))

        first_action = json.loads(vts_e2e.http_call(args.port, "GET", "/action")[1])["actions"][0]
        terminal.send(DOWN, ENTER, ENTER)
        terminal.wait_for("playing " + first_action)
        with open(args.out + ".log") as reader:
            played = ("hikari: action %s " % first_action) in reader.read()
        result.add("play_action", (played, "log shows action %s: %s" % (first_action, played)))

        terminal.send(DOWN, ENTER, b" ", ENTER)
        terminal.wait_for("idle actions on")
        idle = settings(args.port).get("idle_actions") or []
        result.add("idle_checklist", (len(idle) == 1 and saved_file(handle).get("idle_actions") == idle, "GET %s file %s" % (idle, saved_file(handle).get("idle_actions"))))

        terminal.send(DOWN, RIGHT, RIGHT, RIGHT, ENTER)
        shows_400 = terminal.wait_for("action_every must be")
        result.add("bad_value_shows_400", (shows_400 and settings(args.port).get("action_every") == 15 and saved_file(handle).get("action_every") == 15, "screen shows the 400 line, GET %s" % settings(args.port).get("action_every")))

        terminal.send(DOWN, RIGHT, RIGHT, RIGHT, RIGHT, RIGHT, ENTER)
        terminal.wait_for("saved to settings.json")
        result.add("action_every_pair", (settings(args.port).get("action_every") == [18, 20] and saved_file(handle).get("action_every") == [18, 20], "GET %s file %s" % (settings(args.port).get("action_every"), saved_file(handle).get("action_every"))))

        base_wind = json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["strength"]
        base_transition = settings(args.port).get("transition_seconds")
        terminal.send(DOWN, RIGHT, RIGHT, ENTER)
        terminal.wait_for("saved to settings.json")
        transition = settings(args.port).get("transition_seconds")
        result.add("transition_saved", (abs(transition - base_transition - 0.2) < 1e-6 and saved_file(handle).get("transition_seconds") == transition, "GET %s -> %s file %s" % (base_transition, transition, saved_file(handle).get("transition_seconds"))))

        terminal.send(DOWN, RIGHT, ENTER)
        terminal.wait_for("saved to settings.json")
        lost_off = (settings(args.port).get("tracking_lost_loop"), saved_file(handle).get("tracking_lost_loop"))
        terminal.send(RIGHT, ENTER)
        terminal.wait_for("saved to settings.json")
        lost_on = (settings(args.port).get("tracking_lost_loop"), saved_file(handle).get("tracking_lost_loop"))
        result.add("tracking_lost_loop_toggle", (lost_off == (False, False) and lost_on == (True, True), "GET, file after toggle %s, after toggle back %s" % (lost_off, lost_on)))

        views_file = os.path.join(handle["run_dir"], "views.json")
        zoom = json.loads(vts_e2e.http_call(args.port, "GET", "/view")[1])["zoom"]
        terminal.send(DOWN, RIGHT, RIGHT)
        previewed_zoom = json.loads(vts_e2e.http_call(args.port, "GET", "/view")[1])["zoom"]
        terminal.send(ESCAPE)
        terminal.wait_for("reverted")
        reverted_zoom = json.loads(vts_e2e.http_call(args.port, "GET", "/view")[1])["zoom"]
        result.add("framing_preview_reverts", (abs(previewed_zoom - zoom - 0.1) < 1e-4 and abs(reverted_zoom - zoom) < 1e-4 and not os.path.exists(views_file), "zoom %s -> %s -> %s" % (zoom, previewed_zoom, reverted_zoom)))
        terminal.send(RIGHT, RIGHT, ENTER)
        terminal.wait_for("framing saved")
        with open(views_file) as reader:
            saved_zoom = json.load(reader).get(MODELS[1], {}).get("zoom")
        view = json.loads(vts_e2e.http_call(args.port, "GET", "/view")[1])
        result.add("framing_saved", (saved_zoom is not None and abs(saved_zoom - zoom - 0.1) < 1e-4 and abs(view["saved"]["zoom"] - zoom - 0.1) < 1e-4, "views.json zoom %s, GET saved %s" % (saved_zoom, view["saved"])))

        terminal.send(*([DOWN] * 5), *([RIGHT] * 10))
        previewed = json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["strength"]
        result.add("physics_preview_live_not_saved", (previewed == base_wind + 10 and "physics" not in saved_file(handle) and settings(args.port).get("physics") == {}, "live wind %s -> %s, file physics %s" % (base_wind, previewed, saved_file(handle).get("physics"))))

        terminal.send(ESCAPE)
        terminal.wait_for("reverted")
        reverted = json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["strength"]
        result.add("escape_reverts_preview", (reverted == base_wind, "live wind back to %s" % reverted))

        terminal.send(*([RIGHT] * 5), DOWN, DOWN, DOWN, DOWN, ENTER, DOWN, ENTER)
        terminal.wait_for("face is")
        terminal.send(ESCAPE)
        terminal.wait_for("reverted")
        kept = json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["strength"]
        result.add("unrelated_save_keeps_revert", (kept == base_wind and "physics" not in saved_file(handle), "wind preview, face save, escape: live wind %s, base %s" % (kept, base_wind)))
        terminal.send(*([DOWN] * 15))

        base_direction = json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["direction"]
        terminal.send(*([RIGHT] * 10), DOWN, RIGHT, RIGHT, ENTER)
        terminal.wait_for("saved to settings.json")
        wind = settings(args.port).get("physics", {}).get("wind", {})
        file_wind = saved_file(handle).get("physics", {}).get("wind", {})
        result.add("physics_saved", (wind == {"strength": base_wind + 10, "direction": base_direction + 10} and file_wind == wind and json.loads(vts_e2e.http_call(args.port, "POST", "/physics", b"{}")[1])["wind"]["strength"] == base_wind + 10, "GET %s file %s" % (wind, file_wind)))

        terminal.send(*([UP] * 6), RIGHT, RIGHT, *([UP] * 11), ENTER, UP, ENTER)
        terminal.wait_for("switched to " + MODELS[0])
        terminal.send(ENTER, DOWN, ENTER)
        terminal.wait_for("switched to " + MODELS[1])
        back_zoom = json.loads(vts_e2e.http_call(args.port, "GET", "/view")[1])["zoom"]
        result.add("model_switch_drops_framing_preview", (abs(back_zoom - saved_zoom) < 1e-4, "zoom after preview, switch away and back %s, saved %s" % (back_zoom, saved_zoom)))

        terminal.send(b"q")
        code = terminal.wait_exit(EXIT_SECONDS)
        result.add("q_quits", (code == 0, "exit %s" % code))
        terminal.stop()
        terminal = None

        terminal = Terminal(run_binary, args.port, transcript)
        terminal.wait_for("hikari settings")
        vts_e2e.stop_server(handle)
        handle = None
        code = terminal.wait_exit(EXIT_SECONDS)
        result.add("exits_when_renderer_stops", (code == 0 and b"stopped" in terminal.screen, "exit %s, screen says stopped: %s" % (code, b"stopped" in terminal.screen)))
    finally:
        try:
            if terminal:
                terminal.stop()
        finally:
            try:
                if handle:
                    vts_e2e.stop_server(handle)
            finally:
                for copy in copies:
                    shutil.rmtree(copy, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    if args.port == vts_e2e.LIVE_PORT:
        parser.error("port %d is the live service" % vts_e2e.LIVE_PORT)
    result = vts_e2e.Result(args.out)
    with open(args.out + ".pty", "wb") as transcript:
        try:
            check_auto_settings(args, result)
            run(args, result, transcript)
        except BaseException as error:
            result.payload["error"] = "%s: %s" % (type(error).__name__, error)
            result.flush()
            raise
    return vts_e2e.finish(result)


if __name__ == "__main__":
    sys.exit(main())
