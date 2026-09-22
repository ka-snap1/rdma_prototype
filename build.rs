fn main() {
    println!("cargo:rerun-if-changed=naive/fabric_shim.c");
    println!("cargo:rerun-if-changed=naive/fabric_shim.h");

    // get the path to libfabric.pc and check the version
    let fabric = pkg_config::Config::new()
        .atleast_version("1.11")
        .statik(false)
        .cargo_metadata(false)
        .probe("libfabric")
        .expect("can not probe libfabric: please check libfabric.pc and PKG_CONFIG_PATH");

    let mut build = cc::Build::new();

    build
        .file("naive/fabric_shim.c")
        .include("naive")
        .flag_if_supported("-std=c11")
        .warnings(true);

    for path in &fabric.include_paths {
        build.include(path);
    }

    for (name, value) in &fabric.defines {
        build.define(name.as_str(), value.as_deref());
    }

    // compile and input the fabric_shim.c file into a static library
    build.compile("fabric_shim");

    // input the libfabric link parameters into the cargo build process
    pkg_config::Config::new()
        .atleast_version("1.11")
        .statik(false)
        .probe("libfabric")
        .expect("can not get libfabric link parameters: please check libfabric.pc and PKG_CONFIG_PATH");
}