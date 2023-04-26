#[cxx::bridge(namespace = "test_seastar_rs")]
mod ffi {
    extern "Rust" {
        fn spawn_sleep(secs: i32);
    }
}

pub fn spawn_sleep(secs: i32) {
    let _ = seastar::spawn(async move {
        let duration = seastar::Duration::from_secs(secs);
        seastar::sleep::<seastar::SteadyClock>(duration).await;
        println!("SPAWN SLEEP FINISHED");
    });
}
