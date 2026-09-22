fn main() {
    let (major, minor) = rdma_prototype::fabric::runtime_fabric_version();
    println!("libfabric runtime version: {major}.{minor}");
}