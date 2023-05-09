#[cxx::bridge(namespace = "test_seastar_rs")]
mod ffi {
    unsafe extern "C++" {
        include!("rust/cxx_async_headers/cxx_async_futures.hh");

        type VoidFuture = crate::VoidFuture; 
    }

    extern "Rust" {
        fn sleep(secs: i32) -> VoidFuture;

        fn spawn_sleep(secs: i32);
    }
}

#[cxx_async::bridge(namespace = test_seastar_rs)]
unsafe impl std::future::Future for VoidFuture {
    type Output = ();
}

pub fn sleep(secs: i32) -> VoidFuture {
    VoidFuture::infallible(async move {
        let duration = seastar::Duration::from_secs(secs);
        seastar::sleep::<seastar::SteadyClock>(duration).await;
        println!("SLEEP FINISHED");
    })
}

pub fn spawn_sleep(secs: i32) {
    let _ = seastar::spawn(async move {
        let duration = seastar::Duration::from_secs(secs);
        seastar::sleep::<seastar::SteadyClock>(duration).await;
        println!("SPAWN SLEEP FINISHED");
    });
}
