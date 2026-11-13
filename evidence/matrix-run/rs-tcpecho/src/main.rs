// Threaded TCP echo server — Rust 2021 edition, std only.
//
// Features:
//   - Per-connection thread handler
//   - Read (idle) timeout with short-poll so handlers notice shutdown quickly
//   - Line-or-bytes echo: reads a chunk and writes it straight back, so both
//     line-based (telnet) and raw binary clients work unchanged
//   - Max-connections counter via Arc<AtomicUsize>; new connections past the
//     limit are rejected and immediately shut down
//   - Graceful shutdown on SIGINT: a static AtomicBool set by an FFI signal
//     handler, checked by the accept loop (non-blocking listener + 100 ms
//     poll) and by every handler thread (1 s read-timeout poll)
//
// Requires Rust 1.61+ (JoinHandle::is_finished).

use std::io::{ErrorKind, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::os::raw::c_int;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

const BIND_ADDR: &str = "127.0.0.1:7878";

/// A connection with no traffic for this long is closed.
const READ_TIMEOUT_SECS: u64 = 30;

/// The actual read timeout set on the socket — kept short so the handler
/// wakes up and re-checks the shutdown flag at most every second.
const READ_POLL_SECS: u64 = 1;

/// How long the non-blocking accept loop sleeps before retrying.
const ACCEPT_POLL_MS: u64 = 100;

/// Hard cap on concurrent connections. New ones past this are rejected.
const MAX_CONNECTIONS: usize = 64;

/// Per-read buffer size.
const BUF_SIZE: usize = 4096;

/// SIGINT numeric value (same on Unix and Windows CRT).
const SIGINT: c_int = 2;

// ---------------------------------------------------------------------------
// Signal handling (FFI to libc / C runtime, no external crate)
// ---------------------------------------------------------------------------

/// Set by the SIGINT handler.  AtomicBool::store with SeqCst is
/// async-signal-safe.
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

extern "C" fn handle_sigint(_sig: c_int) {
    SHUTDOWN.store(true, Ordering::SeqCst);
}

extern "C" {
    fn signal(signum: c_int, handler: extern "C" fn(c_int)) -> isize;
}

fn install_sigint_handler() {
    // Returns the previous handler (as a function pointer coerced to isize)
    // or -1 (SIG_ERR) on failure.
    let prev = unsafe { signal(SIGINT, handle_sigint) };
    if prev == -1 {
        eprintln!("warning: failed to install SIGINT handler — Ctrl+C will not shut down gracefully");
    }
}

// ---------------------------------------------------------------------------
// Per-connection handler
// ---------------------------------------------------------------------------

fn handle_connection(mut stream: TcpStream, active: Arc<AtomicUsize>) {
    let peer = stream
        .peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "<unknown>".into());

    // The listener is non-blocking; accepted sockets may inherit that on some
    // platforms, so switch back to blocking mode before applying a read timeout.
    let _ = stream.set_nonblocking(false);

    // Short read timeout so the handler wakes up frequently to check shutdown.
    let poll = Duration::from_secs(READ_POLL_SECS);
    if let Err(e) = stream.set_read_timeout(Some(poll)) {
        eprintln!("[{}] failed to set read timeout: {}", peer, e);
    }

    let idle_limit = Duration::from_secs(READ_TIMEOUT_SECS);
    let mut last_data = Instant::now();
    let mut buf = [0u8; BUF_SIZE];

    loop {
        // Cooperate with graceful shutdown.
        if SHUTDOWN.load(Ordering::SeqCst) {
            println!("[{}] closing for shutdown", peer);
            break;
        }

        // Enforce the real idle timeout across multiple short polls.
        if last_data.elapsed() >= idle_limit {
            println!("[{}] idle timeout ({}s)", peer, READ_TIMEOUT_SECS);
            break;
        }

        match stream.read(&mut buf) {
            // Peer half-closed.
            Ok(0) => {
                println!("[{}] peer closed connection", peer);
                break;
            }
            // Got data — echo it straight back (works for lines or raw bytes).
            Ok(n) => {
                last_data = Instant::now();
                if let Err(e) = stream.write_all(&buf[..n]) {
                    eprintln!("[{}] write error: {}", peer, e);
                    break;
                }
            }
            // Short poll expired — loop back, re-check shutdown / idle limit.
            Err(ref e) if e.kind() == ErrorKind::TimedOut || e.kind() == ErrorKind::WouldBlock => {
                continue;
            }
            // EINTR — retry.
            Err(ref e) if e.kind() == ErrorKind::Interrupted => {
                continue;
            }
            Err(e) => {
                eprintln!("[{}] read error: {}", peer, e);
                break;
            }
        }
    }

    let _ = stream.shutdown(Shutdown::Both);
    let remaining = active.fetch_sub(1, Ordering::SeqCst) - 1;
    println!("[{}] closed (active: {})", peer, remaining);
}

// ---------------------------------------------------------------------------
// Main accept loop
// ---------------------------------------------------------------------------

fn main() {
    install_sigint_handler();

    let listener = TcpListener::bind(BIND_ADDR).unwrap_or_else(|e| {
        eprintln!("bind {} failed: {}", BIND_ADDR, e);
        std::process::exit(1);
    });

    // Non-blocking listener so accept() returns WouldBlock instead of
    // blocking forever, letting the loop poll the shutdown flag.
    listener
        .set_nonblocking(true)
        .expect("failed to set listener non-blocking");

    let active = Arc::new(AtomicUsize::new(0));
    let mut handles: Vec<thread::JoinHandle<()>> = Vec::new();

    println!(
        "echo server on {} | max {} conns | {}s idle timeout | Ctrl+C to stop",
        BIND_ADDR, MAX_CONNECTIONS, READ_TIMEOUT_SECS
    );

    loop {
        if SHUTDOWN.load(Ordering::SeqCst) {
            println!("shutdown requested, stopping accept loop");
            break;
        }

        match listener.accept() {
            Ok((stream, addr)) => {
                let count = active.fetch_add(1, Ordering::SeqCst) + 1;

                if count > MAX_CONNECTIONS {
                    active.fetch_sub(1, Ordering::SeqCst);
                    println!("[{}] rejected — {} connections already active", addr, MAX_CONNECTIONS);
                    let _ = stream.shutdown(Shutdown::Both);
                    continue;
                }

                println!("[{}] connected (active {}/{})", addr, count, MAX_CONNECTIONS);

                let active = Arc::clone(&active);
                handles.push(thread::spawn(move || {
                    handle_connection(stream, active);
                }));

                // Reap finished handles to keep the vector bounded.
                if handles.len() > MAX_CONNECTIONS * 2 {
                    handles.retain(|h| !h.is_finished());
                }
            }
            Err(ref e) if e.kind() == ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
            }
            Err(e) => {
                if !SHUTDOWN.load(Ordering::SeqCst) {
                    eprintln!("accept error: {}", e);
                }
                thread::sleep(Duration::from_millis(ACCEPT_POLL_MS));
            }
        }
    }

    // Graceful drain: join every handler thread that is still running.
    let pending = active.load(Ordering::SeqCst);
    if pending > 0 {
        println!("draining {} active connection(s)...", pending);
    }

    for h in handles.drain(..) {
        let _ = h.join();
    }

    println!("server stopped");
}
