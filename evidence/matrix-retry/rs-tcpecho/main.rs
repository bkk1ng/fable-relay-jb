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
/// How long TcpListener::accept() blocks before we re-check SHUTDOWN.
const ACCEPT_TIMEOUT_MS: u64 = 500;

// ── SIGINT via self-pipe ──────────────────────────────────────────────────────
//
// We open a loopback connection to ourselves so that the accept loop unblocks
// immediately when Ctrl-C arrives, rather than waiting for the next real
// connection.  The handler writes one byte to the "trigger" address; the
// resulting accepted stream is detected and discarded.
fn install_sigint_handler(trigger_addr: std::net::SocketAddr) {
    // SAFETY: libc signal() is the only portable way to register a handler in
    // std-only Rust.  The handler only sets an AtomicBool and opens a brief
    // TCP connection, both of which are async-signal-safe enough for our use.
    extern "C" fn handler(_: libc::c_int) {
        SHUTDOWN.store(true, Ordering::SeqCst);
    }

    unsafe {
        libc::signal(libc::SIGINT, handler as libc::sighandler_t);
    }

    // Spawn a watcher thread: once SHUTDOWN fires it dials the listener so the
    // accept() call returns immediately.
    thread::spawn(move || {
        while !SHUTDOWN.load(Ordering::SeqCst) {
            thread::sleep(Duration::from_millis(100));
        }
        // Best-effort: ignore errors — we're shutting down anyway.
        let _ = TcpStream::connect(trigger_addr);
    });
}

// ── connection handler ────────────────────────────────────────────────────────
fn handle_connection(
    mut stream: TcpStream,
    peer: std::net::SocketAddr,
    active: Arc<AtomicUsize>,
) {
    // Set a read deadline so idle clients don't hold threads forever.
    if let Err(e) = stream.set_read_timeout(Some(Duration::from_secs(READ_TIMEOUT_SECS))) {
        eprintln!("[{peer}] set_read_timeout error: {e}");
        return;
    }

    println!("[{peer}] connected  (active: {})", active.load(Ordering::Relaxed));

    let mut reader = BufReader::new(stream.try_clone().expect("clone stream"));
    let mut buf = Vec::with_capacity(4096);

    loop {
        buf.clear();

        // Try a line read first.  If the client sends raw bytes with no '\n'
        // we still echo whatever arrived before the timeout.
        match reader.read_until(b'\n', &mut buf) {
            Ok(0) => {
                // Clean EOF.
                println!("[{peer}] disconnected (EOF)");
                break;
            }
            Ok(n) => {
                // Echo the bytes back verbatim (line or partial chunk).
                if let Err(e) = stream.write_all(&buf[..n]) {
                    eprintln!("[{peer}] write error: {e}");
                    break;
                }
            }
            Err(ref e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut =>
            {
                println!("[{peer}] read timeout — closing idle connection");
                break;
            }
            Err(e) => {
                eprintln!("[{peer}] read error: {e}");
                break;
            }
        }

        // If the server is shutting down, finish this echo cycle then exit.
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("[{peer}] shutdown in progress — closing connection");
            break;
        }
    }

    // Decrement *after* the connection is fully done.
    let remaining = active.fetch_sub(1, Ordering::Relaxed) - 1;
    println!("[{peer}] handler exiting  (active: {remaining})");
}

// ── main ──────────────────────────────────────────────────────────────────────
fn main() -> io::Result<()> {
    let listener = TcpListener::bind(ADDR)?;
    // Non-blocking accept with a short timeout lets us poll SHUTDOWN.
    listener.set_nonblocking(false)?;
    listener
        .set_ttl(64)
        .ok(); // best-effort; not critical

    // We need the actual bound address (port may be 0 in tests, etc.).
    let local_addr = listener.local_addr()?;
    println!("Echo server listening on {local_addr}");
    println!("Press Ctrl-C to shut down.");

    install_sigint_handler(local_addr);

    // Give accept() a short timeout so the loop can check SHUTDOWN.
    listener.set_nonblocking(false)?;
    // We use the socket's read timeout as the accept poll interval.
    // On most platforms TcpListener wraps a socket, but std doesn't expose
    // set_read_timeout on TcpListener directly — we approximate by using a
    // non-blocking accept loop with thread::sleep.
    //
    // For a clean portable solution we set the underlying socket option via
    // the raw fd / SOCKET.
    set_accept_timeout(&listener, Duration::from_millis(ACCEPT_TIMEOUT_MS));

    let active = Arc::new(AtomicUsize::new(0));

    loop {
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("Shutdown signal received — stopping accept loop.");
            break;
        }

        let (stream, peer) = match listener.accept() {
            Ok(pair) => pair,
            Err(ref e)
                if e.kind() == io::ErrorKind::WouldBlock
                    || e.kind() == io::ErrorKind::TimedOut =>
            {
                // No connection arrived before the timeout — loop and recheck.
                continue;
            }
            Err(e) => {
                if SHUTDOWN.load(Ordering::Relaxed) {
                    // The self-pipe woke us up; treat as normal shutdown.
                    break;
                }
                eprintln!("accept error: {e}");
                continue;
            }
        };

        // If this is the self-pipe wake-up connection, discard it silently.
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("Shutdown signal received — stopping accept loop.");
            break;
        }

        // Enforce max-connections limit.
        let current = active.fetch_add(1, Ordering::Relaxed);
        if current >= MAX_CONNECTIONS {
            active.fetch_sub(1, Ordering::Relaxed);
            eprintln!("[{peer}] rejected — connection limit ({MAX_CONNECTIONS}) reached");
            // stream drops here, closing the connection immediately.
            continue;
        }

        let active_clone = Arc::clone(&active);
        thread::spawn(move || {
            handle_connection(stream, peer, active_clone);
        });
    }

    // Brief grace period — wait for in-flight handlers to finish.
    println!("Waiting for active connections to close…");
    let mut waited = 0u32;
    while active.load(Ordering::Relaxed) > 0 && waited < 50 {
        thread::sleep(Duration::from_millis(100));
        waited += 1;
    }
    let remaining = active.load(Ordering::Relaxed);
    if remaining > 0 {
        println!("Exiting with {remaining} connection(s) still active.");
    } else {
        println!("All connections closed. Goodbye.");
    }

    Ok(())
}

// ── platform helpers ──────────────────────────────────────────────────────────

/// Set SO_RCVTIMEO on the listener socket so that accept() times out instead
/// of blocking forever.  This is the portable std-only way to poll SHUTDOWN.
#[cfg(unix)]
fn set_accept_timeout(listener: &TcpListener, timeout: Duration) {
    use std::os::unix::io::AsRawFd;

    let fd = listener.as_raw_fd();
    let tv = libc::timeval {
        tv_sec: timeout.as_secs() as libc::time_t,
        tv_usec: timeout.subsec_micros() as libc::suseconds_t,
    };
    unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_RCVTIMEO,
            &tv as *const _ as *const libc::c_void,
            std::mem::size_of::<libc::timeval>() as libc::socklen_t,
        );
    }
}

#[cfg(windows)]
fn set_accept_timeout(listener: &TcpListener, timeout: Duration) {
    use std::os::windows::io::AsRawSocket;
    use winapi::um::winsock2::{setsockopt, SOCKET, SOL_SOCKET, SO_RCVTIMEO};

    let sock = listener.as_raw_socket() as SOCKET;
    let ms: u32 = timeout.as_millis().min(u32::MAX as u128) as u32;
    unsafe {
        setsockopt(
            sock,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &ms as *const u32 as *const i8,
            std::mem::size_of::<u32>() as i32,
        );
    }
}

#[cfg(not(any(unix, windows)))]
fn set_accept_timeout(_listener: &TcpListener, _timeout: Duration) {
    // Fallback: accept will block; SHUTDOWN is still checked after each
    // connection arrives, so shutdown will happen on the next connection.
}
