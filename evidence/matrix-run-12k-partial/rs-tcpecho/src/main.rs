// Threaded TCP echo server — Rust 2021, std-only.
// 127.0.0.1:7878, one thread per connection, 30s read timeout,
// line-or-bytes echo, max-connections guard (Arc<AtomicUsize>),
// graceful SIGINT shutdown via static AtomicBool + polling accept.

use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

// ── constants ────────────────────────────────────────────────────────────────
const ADDR: &str = "127.0.0.1:7878";
const MAX_CONNECTIONS: usize = 128;
const READ_TIMEOUT: Duration = Duration::from_secs(30);
const POLL_INTERVAL: Duration = Duration::from_millis(200);

// ── shutdown flag ────────────────────────────────────────────────────────────
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

/// Install a SIGINT handler that sets SHUTDOWN.  The only FFI needed is
/// libc `signal`; the handler does a single atomic store (async-signal-safe).
#[cfg(unix)]
fn install_sigint_handler() {
    use std::os::raw::{c_int, c_void};

    extern "C" fn on_sigint(_: c_int) {
        SHUTDOWN.store(true, Ordering::SeqCst);
    }

    extern "C" {
        fn signal(signum: c_int, handler: *const c_void) -> *const c_void;
    }

    const SIGINT: c_int = 2;
    unsafe {
        signal(SIGINT, on_sigint as *const c_void);
    }
}

#[cfg(not(unix))]
fn install_sigint_handler() {
    // No portable SIGINT in pure std on non-Unix.
}

// ── connection handler ───────────────────────────────────────────────────────
fn handle_connection(
    mut stream: std::net::TcpStream,
    peer: std::net::SocketAddr,
    id: usize,
) {
    println!("[conn {id}] accepted from {peer}");

    if let Err(e) = stream.set_read_timeout(Some(READ_TIMEOUT)) {
        eprintln!("[conn {id}] set_read_timeout failed: {e}");
        return;
    }

    let mut buf = [0u8; 4096];
    loop {
        match stream.read(&mut buf) {
            Ok(0) => {
                println!("[conn {id}] client closed");
                break;
            }
            Ok(n) => {
                // Echo exactly what was read — handles line-terminated text
                // and raw binary bytes identically.
                if let Err(e) = stream.write_all(&buf[..n]) {
                    eprintln!("[conn {id}] write error: {e}");
                    break;
                }
                println!("[conn {id}] echoed {n} byte(s)");
            }
            Err(e) => {
                use std::io::ErrorKind::*;
                match e.kind() {
                    TimedOut | WouldBlock => {
                        println!("[conn {id}] read timeout, closing");
                    }
                    ConnectionReset | ConnectionAborted | BrokenPipe => {
                        println!("[conn {id}] client disconnected");
                    }
                    _ => eprintln!("[conn {id}] read error: {e}"),
                }
                break;
            }
        }
    }

    println!("[conn {id}] closed");
}

// ── main ─────────────────────────────────────────────────────────────────────
fn main() {
    install_sigint_handler();

    let listener = match TcpListener::bind(ADDR) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("bind {ADDR} failed: {e}");
            std::process::exit(1);
        }
    };

    // Non-blocking so accept() returns WouldBlock and we can poll SHUTDOWN.
    if let Err(e) = listener.set_nonblocking(true) {
        eprintln!("set_nonblocking failed: {e}");
        std::process::exit(1);
    }

    let active = Arc::new(AtomicUsize::new(0));
    let next_id = Arc::new(AtomicUsize::new(0));

    println!(
        "echo server on {ADDR}  (max {MAX_CONNECTIONS} conns, 30s read timeout, Ctrl-C to stop)"
    );

    loop {
        if SHUTDOWN.load(Ordering::SeqCst) {
            println!("shutdown signal received — stopping accept loop");
            break;
        }

        match listener.accept() {
            Ok((stream, peer)) => {
                // Accepted sockets may inherit non-blocking mode; switch back
                // so read/write block normally with the read timeout.
                let _ = stream.set_nonblocking(false);

                let current = active.load(Ordering::SeqCst);
                if current >= MAX_CONNECTIONS {
                    eprintln!(
                        "max connections ({MAX_CONNECTIONS}) reached — dropping {peer}"
                    );
                    continue;
                }

                let id = next_id.fetch_add(1, Ordering::SeqCst);
                active.fetch_add(1, Ordering::SeqCst);

                let active_ref = Arc::clone(&active);
                thread::spawn(move || {
                    handle_connection(stream, peer, id);
                    let remaining = active_ref.fetch_sub(1, Ordering::SeqCst) - 1;
                    println!("[conn {id}] active: {remaining}");
                });
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(POLL_INTERVAL);
            }
            Err(e) => {
                eprintln!("accept error: {e}");
                thread::sleep(POLL_INTERVAL);
            }
        }
    }

    // Best-effort drain: give in-flight handlers up to 5s to finish.
    let in_flight = active.load(Ordering::SeqCst);
    if in_flight > 0 {
        println!("draining {in_flight} active connection(s)…");
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while active.load(Ordering::SeqCst) > 0 && std::time::Instant::now() < deadline {
            thread::sleep(Duration::from_millis(100));
        }
    }

    let remaining = active.load(Ordering::SeqCst);
    if remaining > 0 {
        println!("server stopped ({remaining} connection(s) still active)");
    } else {
        println!("server stopped");
    }
}
