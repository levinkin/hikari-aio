#!/bin/sh
set -e
cd "$(dirname "$0")/../.."
target=${1:-aarch64-apple-ios-sim}
case $target in
    *-sim) sdk=iphonesimulator triple=arm64-apple-ios17.0-simulator ;;
    *) sdk=iphoneos triple=arm64-apple-ios17.0 ;;
esac
export IPHONEOS_DEPLOYMENT_TARGET=17.0
cargo build --release --lib --target "$target"
out=${CARGO_TARGET_DIR:-target}/$target/release
app=$out/hikari.app
rm -rf "$app"
mkdir -p "$app/models"
xcrun -sdk $sdk clang -fobjc-arc -O2 -target $triple -Inative/engine native/ios/main.m "$out/libhikari.a" -lc++ -framework UIKit -framework Metal -framework QuartzCore -framework ImageIO -framework CoreGraphics -o "$app/hikari"
cp native/ios/Info.plist default.model3.json settings.json "$app/"
cp -R actions states "$app/"
cp -RL models/hiyori "$app/models/"
codesign -s - "$app"
echo "$app"
