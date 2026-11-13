#!/bin/sh
set -e
cd "$(dirname "$0")/../.."
keystore=${1:?usage: build.sh <debug keystore>}
sdk=$HOME/Library/Android/sdk
ndk=$sdk/ndk/$(ls "$sdk/ndk" | tail -1)
tools=$sdk/build-tools/$(ls "$sdk/build-tools" | tail -1)
bin=$ndk/toolchains/llvm/prebuilt/darwin-x86_64/bin
export CC_aarch64_linux_android=$bin/aarch64-linux-android33-clang
export CXX_aarch64_linux_android=$bin/aarch64-linux-android33-clang++
export AR_aarch64_linux_android=$bin/llvm-ar
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$CC_aarch64_linux_android
cargo build --release --lib --target aarch64-linux-android
out=${CARGO_TARGET_DIR:-target}/aarch64-linux-android/release
apk=$out/apk
rm -rf "$apk"
mkdir -p "$apk/lib/arm64-v8a" "$apk/assets/models"
glue=$ndk/sources/android/native_app_glue
$CXX_aarch64_linux_android -shared -O2 -static-libstdc++ -u ANativeActivity_onCreate -Inative/engine -I"$glue" -x c native/android/main.c "$glue/android_native_app_glue.c" -x none "$out/libhikari.a" -landroid -llog -lvulkan -o "$apk/lib/arm64-v8a/libhikari.so"
$bin/llvm-strip "$apk/lib/arm64-v8a/libhikari.so"
cp default.model3.json settings.json "$apk/assets/"
cp -R actions states "$apk/assets/"
cp -RL models/hiyori "$apk/assets/models/"
(cd "$apk/assets" && find . -type f ! -name files | sed 's|^\./||' > files)
"$tools/aapt2" link -o "$apk/unaligned.apk" --manifest native/android/AndroidManifest.xml -I "$sdk/platforms/android-35/android.jar" -A "$apk/assets"
(cd "$apk" && zip -q -r unaligned.apk lib)
"$tools/zipalign" -f -P 16 4 "$apk/unaligned.apk" "$apk/aligned.apk"
[ -f "$keystore" ] || keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias debug -dname CN=hikari -keyalg RSA -validity 10000
"$tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --out "$out/hikari.apk" "$apk/aligned.apk"
echo "$out/hikari.apk"
