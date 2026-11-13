use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;
use std::thread;

// ── shutdown flag ────────────────────────────────────────────────────────────
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

// ── constants ────────────────────────────────────────────────────────────────
const ADDR: &str = "127.0.0.1:7878";
const MAX_CONNECTIONS: usize = 128;
const READ_TIMEOUT_SECS: u64 = 30;
const ACCEPT_TIMEOUT_MS: u64 = 200; // polling interval for shutdown check

// ── SIGINT handler ───────────────────────────────────────────────────────────
//
// std only: register with libc via signal(2) through the extern "C" shim.
// We set the flag here and let the accept-poll loop notice it.
extern "C" fn handle_sigint(_: i32) {
    SHUTDOWN.store(true, Ordering::SeqCst);
}

fn register_sigint() {
    // SAFETY: single-threaded at this point; handler only writes an atomic.
    unsafe {
        libc_signal(libc_SIGINT, handle_sigint as usize);
    }
}

// Minimal inline libc bindings (std only — no libc crate).
#[cfg(unix)]
mod sys {
    pub const SIGINT: i32 = 2;

    extern "C" {
        pub fn signal(sig: i32, handler: usize) -> usize;
    }
}

#[cfg(unix)]
fn libc_SIGINT() -> i32 { sys::SIGINT }
#[cfg(unix)]
unsafe fn libc_signal(sig: i32, handler: usize) {
    sys::signal(sig, handler);
}

#[cfg(not(unix))]
fn libc_SIGINT() -> i32 { 0 }
#[cfg(not(unix))]
unsafe fn libc_signal(_sig: i32, _handler: usize) {
    // On non-Unix targets (Windows), install a Ctrl-C handler via SetConsoleCtrlHandler
    // or just rely on the polling loop noticing an OS-level termination.
    // For portability we leave this as a no-op; the server still shuts down cleanly
    // when the process is killed because all threads are non-detached daemons.
}

// ── connection handler ───────────────────────────────────────────────────────

fn handle_connection(stream: TcpStream, id: usize, counter: Arc<AtomicUsize>) {
    let peer = stream.peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "<unknown>".to_string());

    println!("[{id}] connected  peer={peer}");

    // Set per-direction timeouts.
    if let Err(e) = stream.set_read_timeout(Some(Duration::from_secs(READ_TIMEOUT_SECS))) {
        eprintln!("[{id}] set_read_timeout failed: {e}");
    }
    if let Err(e) = stream.set_write_timeout(Some(Duration::from_secs(READ_TIMEOUT_SECS))) {
        eprintln!("[{id}] set_write_timeout failed: {e}");
    }

    // Clone for writing; both halves share the underlying fd.
    let writer = match stream.try_clone() {
        Ok(w) => w,
        Err(e) => {
            eprintln!("[{id}] try_clone failed: {e}");
            counter.fetch_sub(1, Ordering::Relaxed);
            return;
        }
    };

    let mut reader = BufReader::new(stream);
    let mut writer = std::io::BufWriter::new(writer);
    let mut bytes_echoed: u64 = 0;

    loop {
        let mut line = String::new();

        match reader.read_line(&mut line) {
            Ok(0) => {
                // EOF — clean disconnect.
                println!("[{id}] disconnected (EOF)  echoed={bytes_echoed}B");
                break;
            }
            Ok(n) => {
                // Echo the line (or raw bytes) back as-is.
                if let Err(e) = writer.write_all(line.as_bytes()) {
                    eprintln!("[{id}] write error: {e}");
                    break;
                }
                if let Err(e) = writer.flush() {
                    eprintln!("[{id}] flush error: {e}");
                    break;
                }
                bytes_echoed += n as u64;
                print!("[{id}] echoed {n}B");
                // Print a sanitised preview (strip trailing newline for readability).
                let preview = line.trim_end();
                if !preview.is_empty() {
                    print!("  {:?}", &preview[..preview.len().min(64)]);
                }
                println!();
            }
            Err(e) => {
                use std::io::ErrorKind;
                match e.kind() {
                    ErrorKind::TimedOut | ErrorKind::WouldBlock => {
                        println!("[{id}] read timeout — closing");
                    }
                    ErrorKind::ConnectionReset | ErrorKind::BrokenPipe => {
                        println!("[{id}] disconnected (reset)  echoed={bytes_echoed}B");
                    }
                    _ => {
                        eprintln!("[{id}] read error: {e}");
                    }
                }
                break;
            }
        }

        // Respect server-wide shutdown between reads.
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("[{id}] shutdown requested — closing connection");
            break;
        }
    }

    counter.fetch_sub(1, Ordering::Relaxed);
}

// ── main ─────────────────────────────────────────────────────────────────────

fn main() {
    register_sigint();

    let listener = TcpListener::bind(ADDR).unwrap_or_else(|e| {
        eprintln!("bind {ADDR}: {e}");
        std::process::exit(1);
    });

    // Non-blocking accept so we can poll SHUTDOWN between attempts.
    listener
        .set_nonblocking(true)
        .expect("set_nonblocking failed");

    let counter = Arc::new(AtomicUsize::new(0));
    let mut next_id: usize = 0;

    println!("echo server listening on {ADDR}  (max connections: {MAX_CONNECTIONS})");
    println!("press Ctrl-C to shut down gracefully");

    loop {
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("shutting down — waiting for active connections to finish…");
            // Give in-flight handlers a moment to drain.  A production server
            // would join thread handles; here we do a simple timed wait.
            let deadline = std::time::Instant::now() + Duration::from_secs(5);
            while counter.load(Ordering::Relaxed) > 0
                && std::time::Instant::now() < deadline
            {
                thread::sleep(Duration::from_millis(50));
            }
            let remaining = counter.load(Ordering::Relaxed);
            if remaining > 0 {
                eprintln!("{remaining} connection(s) still active after timeout — exiting anyway");
            } else {
                println!("all connections closed cleanly");
            }
            break;
        }

        match listener.accept() {
            Ok((stream, _)) => {
                let current = counter.load(Ordering::Relaxed);
                if current >= MAX_CONNECTIONS {
                    eprintln!(
                        "max connections ({MAX_CONNECTIONS}) reached — rejecting new connection"
                    );
                    // stream drops here, closing the socket immediately.
                    continue;
                }

                counter.fetch_add(1, Ordering::Relaxed);
                let id = next_id;
                next_id = next_id.wrapping_add(1);
                let counter_clone = Arc::clone(&counter);

                thread::spawn(move || handle_connection(stream, id, counter_clone));

                println!(
                    "[server] active connections: {}",
                    counter.load(Ordering::Relaxed)
                );
            }
            Err(e) => {
                use std::io::ErrorKind;
                match e.kind() {
                    // Expected: no pending connection yet — sleep the poll interval.
                    ErrorKind::WouldBlock => {
                        thread::sleep(Duration::from_millis(ACCEPT_TIMEOUT_MS));
                    }
                    _ => {
                        eprintln!("accept error: {e}");
                        // Brief back-off to avoid a tight error loop.
                        thread::sleep(Duration::from_millis(ACCEPT_TIMEOUT_MS));
                    }
                }
            }
        }
    }

    println!("server stopped");
}
