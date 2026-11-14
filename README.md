# hikari

hikari draws a moc3 model and streams the frames over http or shows it in its own window. i built it to be as light weight as possible without sacrificing speed or quality. it runs on metal (mac/ios) and vulkan (windows/linux/android).

[getting started](docs/getting-started.md) has setup, running it and the http api.

## changes

- no browser or game engine. it draws straight on metal or vulkan, so the whole thing is one binary (1.4 MB on mac, 2.1 MB on windows) with the core and framework built in.
- every window/stream showing the same model shares one gpu copy of its textures.
- it bc7-compresses each texture (a quarter of the gpu memory) only if the result stays visually identical (mean error ≤ 1 and max ≤ 160 out of 255, measured per texture). otherwise the texture stays uncompressed.
- a stream at a different size than the main view gets its own copy of the model. the main view used to update its model every frame even when nothing drew it. now it skips that (0.86 → 0.67 ms per frame on an m6, frames byte-identical).

## clean room

the core (`core/`) and framework (`native/framework/`) are a clean room build. one side studied how the stock runtime behaves and wrote specs. the other side wrote the code from those specs only, without seeing the stock code. every step (specs, study notes, timestamps) is documented and archived.

## numbers

### against vtube studio

both apps ran one at a time on the same windows pc (rtx 4090), fullscreen at 2560x1440 and capped at 60 fps, with a stock sample model (133 drawables, 2,807 vertices), the same files on both sides. active means an action every 2 seconds plus lip sync. each number is the median of three 60 second runs.

| | vtube studio | hikari | hikari uses |
|---|---|---|---|
| memory, idle / active | 724 / 733 MB | 138 / 139 MB | 81% / 81% less |
| private memory, idle / active | 1,569 / 1,583 MB | 336 / 338 MB | 79% / 79% less |
| gpu memory, idle / active | 711 / 711 MB | 247 / 248 MB | 65% / 65% less |
| cpu per frame, idle / active | 8.1 / 10.7 ms | 0.39 / 0.56 ms | 95% / 95% less |
| gpu per frame, idle / active | 1.27 / 1.25 ms | 1.06 / 1.11 ms | 17% / 11% less |

`hikari.exe` is 2.1 MB against vtube studio's 1,612 MB install. gpu time comes from windows' busy counter, so treat it as an estimate.

### quality

frames match the previous hikari build within 0.5 out of 255 on average.

## code signing policy

free code signing provided by [signpath.io](https://signpath.io), certificate by [signpath foundation](https://signpath.org).

- committers and reviewers: [kevin lin](https://github.com/levinkin)
- approvers: [kevin lin](https://github.com/levinkin)

privacy: this program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## licensing

my code is under the mit license (see `LICENSE`), and bc7enc in `native/engine/bc7enc/` keeps its own. the core and framework are my own (see clean room). models aren't included, and each has its own terms, so check them before you use one.
