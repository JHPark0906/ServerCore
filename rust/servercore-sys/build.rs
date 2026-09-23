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
        "SERVERCORE_NATIVE_LIBRARY",
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
            // Remove the former independent adapter-linkage cache option.
            .args(["-U", "SERVERCORE_C_API_SHARED"])
            .arg("-DSERVERCORE_BUILD_TESTS=OFF")
            .arg("-DSERVERCORE_BUILD_C_API=ON")
            .arg(format!(
                "-DSERVERCORE_BUILD_SHARED={}",
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
                "ServerCore",
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
        for name in ["ServerCore", "ServerCored"] {
            watch_library(&directory, name);
            watch_library(&directory.join("../bin"), name);
        }
    }
    let library = if prebuilt {
        select_prebuilt_library(&directory, &target, static_link)
    } else {
        "ServerCore"
    };
    println!("cargo:rustc-link-search=native={}", directory.display());
    println!(
        "cargo:rustc-link-lib={}={}",
        if static_link { "static" } else { "dylib" },
        library
    );
    if static_link {
        // A prebuilt static archive's transitive native libraries depend on the
        // C++ toolchain and platform; require an explicit link list.
        let libraries = env::var("SERVERCORE_NATIVE_LIBS").expect("static ServerCore needs SERVERCORE_NATIVE_LIBS (semicolon-separated ordered transitive linker libraries)");
        for dependency in libraries.split(';').filter(|s| !s.is_empty()) {
            println!("cargo:rustc-link-lib={dependency}");
            if prebuilt {
                let name = dependency.rsplit('=').next().unwrap();
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
        copy_windows_dll(&directory, library);
    }
    // Cargo adds native search paths beneath OUT_DIR to the loader path for
    // cargo run/test. Installed/prebuilt Linux libraries need LD_LIBRARY_PATH
    // (or an application-owned rpath); a dependency cannot set its consumer's
    // executable rpath with cargo:rustc-link-arg.
}

fn select_prebuilt_library(directory: &Path, target: &str, static_link: bool) -> &'static str {
    const NAMES: [&str; 2] = ["ServerCore", "ServerCored"];
    let exists = |name: &str| {
        directory
            .join(if target.contains("windows") {
                format!("{name}.lib")
            } else if static_link {
                format!("lib{name}.a")
            } else {
                format!("lib{name}.so")
            })
            .is_file()
    };
    if let Ok(selected) = env::var("SERVERCORE_NATIVE_LIBRARY") {
        let name = NAMES
            .into_iter()
            .find(|name| *name == selected)
            .expect("SERVERCORE_NATIVE_LIBRARY must be ServerCore or ServerCored");
        assert!(exists(name), "selected native library is absent: {name}");
        return name;
    }
    // Release is the default when both configurations are installed together.
    NAMES
        .into_iter()
        .find(|name| exists(name))
        .expect("no matching ServerCore native library; build with SERVERCORE_BUILD_C_API=ON")
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

fn copy_windows_dll(directory: &Path, library: &str) {
    let filename = format!("{library}.dll");
    let dll = [
        directory.join(&filename),
        directory.join("../bin").join(&filename),
    ]
    .into_iter()
    .find(|path| path.is_file())
    .unwrap_or_else(|| panic!("{filename} must be beside its import library or in ../bin"));
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let runtime = out.join("runtime");
    fs::create_dir_all(&runtime).unwrap();
    // Stage exactly the selected binary. Remove stale variants when a
    // Cargo output directory is reused for another native configuration.
    let bundle = dll.parent().unwrap();
    for name in ["ServerCoreCAbi.dll", "ServerCore.dll", "ServerCored.dll"] {
        let source = bundle.join(name);
        let destination = runtime.join(name);
        if name == filename {
            fs::copy(source, destination).expect("stage ServerCore DLL");
        } else if destination.exists() {
            fs::remove_file(destination).expect("remove stale staged ServerCore DLL");
        }
    }
    // Cargo supplies OUT_DIR search paths in PATH for cargo run/test. Keep each
    // build variant isolated and never modify files outside OUT_DIR.
    println!("cargo:rustc-link-search=native={}", runtime.display());
}
