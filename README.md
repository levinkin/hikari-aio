# hikari

hikari draws a moc3 model and streams the frames over http or shows it in its own window. i built it to be as light weight as possible without sacrificing speed or quality. it runs on metal (mac/ios) and vulkan (windows/linux/android).

[getting started](docs/getting-started.md) has setup, running it and the http api.

## changes

- no browser or game engine. it draws straight on metal or vulkan, so the whole thing is one binary (1.2 MB on mac, 1.9 MB on windows) with the core and framework built in.
- every window/stream showing the same model shares one gpu copy of its textures.
- it bc7-compresses each texture (4x less gpu memory) only if the result stays visually identical (mean error ≤ 1 and max ≤ 160 out of 255, measured per texture). otherwise the texture stays uncompressed.
- a stream at a different size than the main view gets its own copy of the model. the main view used to update its model every frame even when nothing drew it. now it skips that (0.86 → 0.67 ms per frame on an m6, frames byte-identical).

## clean room

the core (`core/`) and framework (`native/framework/`) are a clean room build. one side studied how the stock runtime behaves and wrote specs. the other side wrote the code from those specs only, without seeing the stock code. every step (specs, study notes, timestamps) is documented and archived.

## quality

frames match the previous hikari build within 0.5 out of 255 on average.

## code signing policy

free code signing provided by [signpath.io](https://signpath.io), certificate by [signpath foundation](https://signpath.org).

- committers and reviewers: [kevin lin](https://github.com/levinkin)
- approvers: [kevin lin](https://github.com/levinkin)

privacy: this program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## licensing

my code is under the mit license (see `LICENSE`), and bc7enc in `native/engine/bc7enc/` keeps its own. the core and framework are my own (see clean room). models aren't included, and each has its own terms, so check them before you use one.
