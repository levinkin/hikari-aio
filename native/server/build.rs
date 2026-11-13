use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

fn collect_sources(dir: &Path, skip: &[&str], out: &mut Vec<PathBuf>) {
    for entry in fs::read_dir(dir).unwrap() {
        let path = entry.unwrap().path();
        let name = path.file_name().and_then(OsStr::to_str).unwrap_or_default().to_string();
        if name.starts_with('.') {
            continue;
        }
        if path.is_dir() {
            if !skip.contains(&name.as_str()) {
                collect_sources(&path, skip, out);
            }
            continue;
        }
        let ext = path.extension().and_then(OsStr::to_str).unwrap_or_default();
        if (ext == "cpp" || ext == "mm") && !skip.contains(&name.as_str()) {
            out.push(path);
        }
    }
}

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    let engine_dir = manifest_dir.join("../engine");
    let framework_dir = manifest_dir.join("../framework");
    let framework_include = framework_dir.join("include");
    let core_include = manifest_dir.join("../../core/include");
    println!("cargo:rerun-if-changed={}", core_include.display());
    let shader_dir = framework_dir.join("shaders");
    let apple = target_os == "macos" || target_os == "ios";
    let mut framework_sources = Vec::new();
    let skip: &[&str] = if apple { &["vk.cpp"] } else { &[] };
    collect_sources(&framework_dir.join("src"), skip, &mut framework_sources);
    let mut framework = cc::Build::new();
    framework.cpp(true).std("c++17").opt_level(2).include(&framework_include).include(&core_include).include(engine_dir.join("vulkan/include")).warnings(false).files(&framework_sources);
    if env::var("CARGO_CFG_TARGET_ENV").unwrap() != "msvc" {
        framework.flag("-ffp-contract=off").flag("-fno-exceptions").flag("-fno-rtti");
    }
    framework.compile("hfw");
    for dir in ["src", "include", "shaders"] {
        println!("cargo:rerun-if-changed={}", framework_dir.join(dir).display());
    }
    if apple {
        cc::Build::new()
            .cpp(true)
            .std("c++17")
            .flag("-fobjc-arc")
            .flag("-Wno-c23-extensions")
            .flag(format!("--embed-dir={}", shader_dir.display()))
            .include(&engine_dir)
            .include(engine_dir.join("syphon"))
            .include(&framework_include)
            .include(&framework_dir)
            .warnings(false)
            .file(engine_dir.join("engine.mm"))
            .file(engine_dir.join("renderer_metal.mm"))
            .file(engine_dir.join(if target_os == "ios" { "window_ios.mm" } else { "window_mac.mm" }))
            .compile("hikari_engine");
        for framework in ["Metal", "Foundation", "ImageIO", "CoreGraphics", "QuartzCore"] {
            println!("cargo:rustc-link-lib=framework={}", framework);
        }
        if target_os == "macos" {
            let syphon_dir = engine_dir.join("syphon");
            let syphon_sources: Vec<PathBuf> = fs::read_dir(&syphon_dir).unwrap().map(|entry| entry.unwrap().path()).filter(|path| matches!(path.extension().and_then(OsStr::to_str), Some("m" | "c"))).collect();
            cc::Build::new()
                .flag("-fobjc-arc")
                .flag("-Wno-deprecated-declarations")
                .flag("-include")
                .flag(syphon_dir.join("Syphon_Prefix.pch").to_str().unwrap())
                .include(&syphon_dir)
                .warnings(false)
                .files(&syphon_sources)
                .compile("syphon");
            for framework in ["AppKit", "IOSurface"] {
                println!("cargo:rustc-link-lib=framework={}", framework);
            }
        }
    } else {
        let mut engine = cc::Build::new();
        engine.cpp(true).std("c++17").include(&engine_dir).include(&framework_include).include(engine_dir.join("vulkan/include")).include(engine_dir.join("third_party/stb")).warnings(false);
        if target_os == "windows" {
            engine.flag("/utf-8").define("NOMINMAX", None);
        }
        let window_source = match target_os.as_str() {
            "windows" => "window_win32.cpp",
            "android" => "window_android.cpp",
            _ => "window_x11.cpp",
        };
        engine.file(engine_dir.join("engine_vk.cpp")).file(engine_dir.join("renderer_vk.cpp")).file(engine_dir.join("kqueue_compat.cpp")).file(engine_dir.join(window_source)).compile("hikari_engine");
        if target_os == "windows" {
            let target = env::var("TARGET").unwrap();
            let lib_exe = cc::windows_registry::find_tool(&target, "lib.exe").expect("lib.exe from visual studio build tools");
            let status = lib_exe
                .to_command()
                .arg(format!("/def:{}", engine_dir.join("vulkan/vulkan-1.def").display()))
                .arg(if target_arch == "aarch64" { "/machine:arm64" } else { "/machine:x64" })
                .arg(format!("/out:{}", out_dir.join("vulkan-1.lib").display()))
                .arg("/nologo")
                .status()
                .unwrap();
            assert!(status.success(), "lib.exe failed to make vulkan-1.lib");
            println!("cargo:rustc-link-search=native={}", out_dir.display());
            println!("cargo:rustc-link-lib=dylib=vulkan-1");
            println!("cargo:rustc-link-lib=dylib=user32");
            println!("cargo:rustc-link-lib=dylib=d3d11");
            println!("cargo:rustc-link-lib=dylib=dxgi");
        } else if target_os == "android" {
            for library in ["vulkan", "android", "log"] {
                println!("cargo:rustc-link-lib=dylib={}", library);
            }
        } else {
            println!("cargo:rustc-link-lib=dylib:+verbatim=libvulkan.so.1");
            println!("cargo:rustc-link-lib=dylib=X11");
        }
    }
    println!("cargo:rerun-if-changed={}", engine_dir.display());
    println!("cargo:rerun-if-changed=build.rs");
}
