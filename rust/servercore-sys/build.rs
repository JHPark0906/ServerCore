use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

fn run(command: &mut Command) {
    let status = command
        .status()
        .unwrap_or_else(|e| panic!("cannot run {command:?}: {e}"));
    assert!(status.success(), "command failed: {command:?}");
}

fn main() {
    for name in [
        "SERVERCORE_CABI_DIR",
        "SERVERCORE_CABI_STATIC",
        "SERVERCORE_NATIVE_LIBS",
        "SERVERCORE_NATIVE_SEARCH",
        "SERVERCORE_CMAKE_GENERATOR",
        "SERVERCORE_CMAKE_TOOLCHAIN",
        "CMAKE_PREFIX_PATH",
        "CXX",
        "CMAKE",
        "SERVERCORE_CMAKE_JOBS",
    ] {
        println!("cargo:rerun-if-env-changed={name}");
    }
    let target = env::var("TARGET").unwrap();
    assert!(
        target.contains("windows-msvc") || target.contains("linux"),
        "ServerCore supports Windows MSVC and Linux"
    );
    let static_link = env::var_os("SERVERCORE_CABI_STATIC").is_some_and(|v| v == "1");
    let prebuilt = env::var_os("SERVERCORE_CABI_DIR").is_some();
    let directory = if let Some(path) = env::var_os("SERVERCORE_CABI_DIR") {
        PathBuf::from(path)
    } else {
        let root = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../..");
        assert!(
            root.join("CMakeLists.txt").is_file(),
            "set SERVERCORE_CABI_DIR to a built/installed C ABI library directory"
        );
        if env::var("HOST").unwrap() != target {
            assert!(
                env::var_os("SERVERCORE_CMAKE_TOOLCHAIN").is_some(),
                "cross builds require SERVERCORE_CMAKE_TOOLCHAIN or prebuilt SERVERCORE_CABI_DIR"
            );
        }
        println!("cargo:rerun-if-changed={}", root.join("include").display());
        println!("cargo:rerun-if-changed={}", root.join("src").display());
        println!(
            "cargo:rerun-if-changed={}",
            root.join("CMakeLists.txt").display()
        );
        println!("cargo:rerun-if-changed={}", root.join("cmake").display());
        let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
        let build = out.join("native-build");
        let install = out.join("native-install");
        let cmake = env::var_os("CMAKE").unwrap_or_else(|| "cmake".into());
        let mut configure = Command::new(&cmake);
        configure
            .arg("-S")
            .arg(root)
            .arg("-B")
            .arg(&build)
            .arg("-DSERVERCORE_BUILD_TESTS=OFF")
            .arg("-DSERVERCORE_BUILD_C_API=ON")
            .arg(format!(
                "-DSERVERCORE_C_API_SHARED={}",
                if static_link { "OFF" } else { "ON" }
            ))
            .arg("-DCMAKE_BUILD_TYPE=Release")
            .arg(format!("-DCMAKE_INSTALL_PREFIX={}", install.display()));
        if let Some(generator) = env::var_os("SERVERCORE_CMAKE_GENERATOR") {
            configure.arg("-G").arg(generator);
        }
        for (name, cmake_name) in [
            ("SERVERCORE_CMAKE_TOOLCHAIN", "CMAKE_TOOLCHAIN_FILE"),
            ("CMAKE_PREFIX_PATH", "CMAKE_PREFIX_PATH"),
            ("CXX", "CMAKE_CXX_COMPILER"),
        ] {
            if let Some(value) = env::var_os(name) {
                configure.arg(format!("-D{cmake_name}={}", value.to_string_lossy()));
            }
        }
        run(&mut configure);
        run(Command::new(&cmake)
            .arg("--build")
            .arg(&build)
            .args([
                "--config",
                "Release",
                "--target",
                "ServerCoreCAbi",
                "--parallel",
            ])
            .arg(env::var("SERVERCORE_CMAKE_JOBS").unwrap_or_else(|_| "2".into())));
        if static_link {
            println!("cargo:rustc-link-search=native={}", build.display());
            println!(
                "cargo:rustc-link-search=native={}",
                build.join("Release").display()
            );
        }
        run(Command::new(&cmake).arg("--install").arg(build).args([
            "--config",
            "Release",
            "--component",
            "CAbi",
        ]));
        install.join("lib")
    };
    assert!(
        directory.is_dir(),
        "native library directory does not exist: {}",
        directory.display()
    );
    if prebuilt {
        watch_library(&directory, "ServerCoreCAbi");
        watch_library(&directory.join("../bin"), "ServerCoreCAbi");
    }
    println!("cargo:rustc-link-search=native={}", directory.display());
    println!(
        "cargo:rustc-link-lib={}={}",
        if static_link { "static" } else { "dylib" },
        "ServerCoreCAbi"
    );
    if static_link {
        // A prebuilt static archive's transitive native libraries depend on the
        // C++ toolchain and platform; require an explicit link list.
        let libraries = env::var("SERVERCORE_NATIVE_LIBS").expect("static C ABI needs SERVERCORE_NATIVE_LIBS (semicolon-separated ordered linker libraries)");
        for library in libraries.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-lib={library}");
            if prebuilt {
                let name = library.rsplit('=').next().unwrap();
                watch_library(&directory, name);
                if let Ok(paths) = env::var("SERVERCORE_NATIVE_SEARCH") {
                    for path in paths.split(';').filter(|s| !s.is_empty()) {
                        watch_library(Path::new(path), name);
                    }
                }
            }
        }
    }
    if let Ok(paths) = env::var("SERVERCORE_NATIVE_SEARCH") {
        for path in paths.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-search=native={path}");
        }
    }
    if !static_link && target.contains("windows") {
        copy_windows_dll(&directory);
    }
    // Cargo adds native search paths beneath OUT_DIR to the loader path for
    // cargo run/test. Installed/prebuilt Linux libraries need LD_LIBRARY_PATH
    // (or an application-owned rpath); a dependency cannot set its consumer's
    // executable rpath with cargo:rustc-link-arg.
}

fn watch_library(directory: &Path, name: &str) {
    for file in [
        format!("{name}.lib"),
        format!("{name}.dll"),
        format!("lib{name}.a"),
        format!("lib{name}.so"),
    ] {
        let path = directory.join(file);
        if path.is_file() {
            println!("cargo:rerun-if-changed={}", path.display());
        }
    }
}

fn copy_windows_dll(directory: &Path) {
    let dll = [
        directory.join("ServerCoreCAbi.dll"),
        directory.join("../bin/ServerCoreCAbi.dll"),
    ]
    .into_iter()
    .find(|path| path.is_file())
    .expect("ServerCoreCAbi.dll must be beside its import library or in ../bin");
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let runtime = out.join("runtime");
    fs::create_dir_all(&runtime).unwrap();
    fs::copy(&dll, runtime.join("ServerCoreCAbi.dll"))
        .expect("stage C ABI DLL for Cargo's runtime loader path");
    // Cargo supplies OUT_DIR search paths in PATH for cargo run/test. Keep each
    // build variant isolated and never modify files outside OUT_DIR.
    println!("cargo:rustc-link-search=native={}", runtime.display());
}
