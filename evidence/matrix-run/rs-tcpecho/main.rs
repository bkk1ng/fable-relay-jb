use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;
use std::{io, thread};

// ── shutdown flag ─────────────────────────────────────────────────────────────
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

// ── tunables ──────────────────────────────────────────────────────────────────
const ADDR: &str = "127.0.0.1:7878";
const MAX_CONNECTIONS: usize = 128;
const READ_TIMEOUT_SECS: u64 = 30;
/// Accept loop polls the shutdown flag on this cadence.
const ACCEPT_TIMEOUT_MS: u64 = 500;

// ── SIGINT handler ────────────────────────────────────────────────────────────

/// Installs a SIGINT handler that sets the global shutdown flag.
/// Uses the POSIX `signal` syscall via libc-free inline assembly on Linux,
/// or the standard `signal` from libc — but because we're std-only we call
/// the C runtime through the `extern "C"` FFI that std itself uses internally.
#[cfg(unix)]
fn install_signal_handler() {
    use std::os::raw::c_int;

    extern "C" {
        // POSIX signal(2) — available on every Unix target in the C runtime
        // that Rust's std already links against.
        fn signal(signum: c_int, handler: usize) -> usize;
    }

    // SIGINT is 2 on every target we care about.
    const SIGINT: c_int = 2;

    extern "C" fn handler(_: std::os::raw::c_int) {
        SHUTDOWN.store(true, Ordering::SeqCst);
    }

    unsafe {
        signal(SIGINT, handler as usize);
    }
}

#[cfg(not(unix))]
fn install_signal_handler() {
    // On non-Unix targets graceful shutdown is triggered only by the
    // ACCEPT_TIMEOUT polling loop; Ctrl-C will still terminate the process.
    eprintln!("[warn] SIGINT handler not installed on this platform");
}

// ── connection handler ────────────────────────────────────────────────────────

fn handle_connection(stream: TcpStream, id: usize, counter: Arc<AtomicUsize>) {
    // Count this connection.
    let prev = counter.fetch_add(1, Ordering::Relaxed);
    println!("[conn {id}] opened  (active connections: {})", prev + 1);

    // Shadow with a clone so the guard drop at the end decrements correctly.
    let counter_guard = counter.clone();

    let peer = stream
        .peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "<unknown>".into());

    // Apply the read timeout.
    if let Err(e) = stream.set_read_timeout(Some(Duration::from_secs(READ_TIMEOUT_SECS))) {
        eprintln!("[conn {id}] set_read_timeout failed: {e}");
    }

    // Split into reader / writer halves.
    let writer_stream = match stream.try_clone() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[conn {id}] try_clone failed: {e}");
            counter_guard.fetch_sub(1, Ordering::Relaxed);
            return;
        }
    };

    let mut reader = BufReader::new(stream);
    let mut writer = io::BufWriter::new(writer_stream);

    println!("[conn {id}] peer={peer}");

    loop {
        let mut line = String::new();

        match reader.read_line(&mut line) {
            Ok(0) => {
                // EOF — client closed the connection.
                println!("[conn {id}] EOF from {peer}");
                break;
            }
            Ok(n) => {
                // Echo the bytes back (preserving any binary content that
                // happened to end with '\n').
                print!("[conn {id}] recv {n} bytes | {:?}", line.trim_end());
                println!();

                if let Err(e) = writer.write_all(line.as_bytes()) {
                    eprintln!("[conn {id}] write error: {e}");
                    break;
                }
                if let Err(e) = writer.flush() {
                    eprintln!("[conn {id}] flush error: {e}");
                    break;
                }
            }
            Err(e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut =>
            {
                println!("[conn {id}] read timeout — closing {peer}");
                break;
            }
            Err(e) => {
                eprintln!("[conn {id}] read error: {e}");
                break;
            }
        }

        // Respect the shutdown flag between reads.
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("[conn {id}] shutdown requested — closing {peer}");
            break;
        }
    }

    let remaining = counter_guard.fetch_sub(1, Ordering::Relaxed) - 1;
    println!("[conn {id}] closed  (active connections: {remaining})");
}

// ── main ──────────────────────────────────────────────────────────────────────

fn main() -> io::Result<()> {
    install_signal_handler();

    let listener = TcpListener::bind(ADDR)?;
    // Set a short accept timeout so the loop can poll the shutdown flag.
    listener.set_nonblocking(false)?;
    listener
        .set_ttl(64)
        .ok(); // best-effort; non-fatal

    // We approximate the accept timeout with a non-blocking peek loop on
    // platforms that don't expose SO_RCVTIMEO on listening sockets directly.
    // The cleanest std-only approach: set the listener to non-blocking and
    // sleep between WouldBlock returns.
    listener.set_nonblocking(true)?;

    println!("[server] listening on {ADDR}  (max connections: {MAX_CONNECTIONS})");
    println!("[server] send SIGINT (Ctrl-C) to shut down gracefully");

    let active = Arc::new(AtomicUsize::new(0));
    let mut conn_id: usize = 0;

    loop {
        // Poll the shutdown flag first so we stop accepting immediately.
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("[server] shutdown flag set — stopping accept loop");
            break;
        }

        match listener.accept() {
            Ok((stream, _addr)) => {
                // Check capacity before spawning.
                if active.load(Ordering::Relaxed) >= MAX_CONNECTIONS {
                    eprintln!("[server] max connections reached — dropping incoming connection");
                    // stream drops here, closing the socket.
                    continue;
                }

                // Restore blocking mode for this individual socket so the
                // handler's read_timeout works correctly.
                if let Err(e) = stream.set_nonblocking(false) {
                    eprintln!("[server] set_nonblocking(false) on accepted stream: {e}");
                    continue;
                }

                conn_id = conn_id.wrapping_add(1);
                let id = conn_id;
                let counter = Arc::clone(&active);

                thread::spawn(move || handle_connection(stream, id, counter));
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                // No pending connection — sleep briefly before retrying.
                thread::sleep(Duration::from_millis(ACCEPT_TIMEOUT_MS));
            }
            Err(e) => {
                eprintln!("[server] accept error: {e}");
                // A transient error (e.g. EMFILE) shouldn't kill the server.
                thread::sleep(Duration::from_millis(ACCEPT_TIMEOUT_MS));
            }
        }
    }

    // Wait briefly for in-flight connections to drain.
    let drain_limit = 20; // × 100 ms = 2 s
    for i in 0..drain_limit {
        let n = active.load(Ordering::Relaxed);
        if n == 0 {
            break;
        }
        if i == 0 {
            println!("[server] draining {n} active connection(s)…");
        }
        thread::sleep(Duration::from_millis(100));
    }

    println!("[server] shut down cleanly");
    Ok(())
}
