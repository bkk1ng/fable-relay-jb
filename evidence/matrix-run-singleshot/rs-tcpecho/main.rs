use std::{
    io::{BufRead, BufReader, Write},
    net::{TcpListener, TcpStream},
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Arc,
    },
    thread,
    time::Duration,
};

// ── shutdown flag ────────────────────────────────────────────────────────────
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

// ── signal handling (SIGINT via a self-pipe) ─────────────────────────────────
//
// We write one byte to a pipe when SIGINT arrives. The accept loop is on a
// non-blocking listener with a short timeout, so it polls SHUTDOWN on every
// iteration and exits cleanly.

fn install_sigint_handler() {
    // SAFETY: signal() is async-signal-safe; we only set an atomic flag and
    // write one byte to a pipe from the handler — both are safe operations.
    unsafe {
        libc::signal(libc::SIGINT, sigint_handler as libc::sighandler_t);
    }
}

extern "C" fn sigint_handler(_: libc::c_int) {
    SHUTDOWN.store(true, Ordering::SeqCst);
}

// ── connection handler ───────────────────────────────────────────────────────

fn handle_connection(
    mut stream: TcpStream,
    peer: std::net::SocketAddr,
    active: Arc<AtomicUsize>,
) {
    // R/W timeout: drop idle clients after 30 s.
    let timeout = Some(Duration::from_secs(30));
    if let Err(e) = stream.set_read_timeout(timeout) {
        eprintln!("[{peer}] set_read_timeout failed: {e}");
        active.fetch_sub(1, Ordering::Relaxed);
        return;
    }
    if let Err(e) = stream.set_write_timeout(timeout) {
        eprintln!("[{peer}] set_write_timeout failed: {e}");
        active.fetch_sub(1, Ordering::Relaxed);
        return;
    }

    println!("[{peer}] connected  (active: {})", active.load(Ordering::Relaxed));

    let mut reader = BufReader::new(stream.try_clone().expect("clone stream"));

    loop {
        let mut line = String::new();

        match reader.read_line(&mut line) {
            // Client closed the connection.
            Ok(0) => break,

            // Received some bytes — echo them back verbatim.
            // read_line keeps the '\n', so the reply is already line-terminated.
            Ok(n) => {
                print!("[{peer}] recv {n}B: {}", line.trim_end());
                println!();
                if let Err(e) = stream.write_all(line.as_bytes()) {
                    eprintln!("[{peer}] write error: {e}");
                    break;
                }
            }

            // Timeout or any other I/O error.
            Err(e) => {
                use std::io::ErrorKind::*;
                match e.kind() {
                    TimedOut | WouldBlock => {
                        eprintln!("[{peer}] read timeout — closing");
                    }
                    _ => eprintln!("[{peer}] read error: {e}"),
                }
                break;
            }
        }

        // If the server is shutting down, stop accepting more I/O from this
        // client and let the thread exit.
        if SHUTDOWN.load(Ordering::Relaxed) {
            break;
        }
    }

    let remaining = active.fetch_sub(1, Ordering::Relaxed) - 1;
    println!("[{peer}] disconnected (active: {remaining})");
}

// ── main ─────────────────────────────────────────────────────────────────────

fn main() {
    install_sigint_handler();

    let addr = "127.0.0.1:7878";
    let listener = TcpListener::bind(addr).expect("bind");

    // Non-blocking accept so we can poll SHUTDOWN between accept calls.
    listener
        .set_nonblocking(true)
        .expect("set_nonblocking on listener");

    println!("Echo server listening on {addr}  (Ctrl-C to stop)");

    // Shared counter of currently active connections.
    let active_connections: Arc<AtomicUsize> = Arc::new(AtomicUsize::new(0));

    // Maximum concurrent connections we're willing to serve.
    const MAX_CONNECTIONS: usize = 128;

    loop {
        if SHUTDOWN.load(Ordering::Relaxed) {
            println!("Shutdown signal received — stopping accept loop.");
            break;
        }

        match listener.accept() {
            Ok((stream, peer)) => {
                let current = active_connections.load(Ordering::Relaxed);
                if current >= MAX_CONNECTIONS {
                    eprintln!("Connection limit ({MAX_CONNECTIONS}) reached, dropping {peer}");
                    // `stream` is dropped here, which closes the socket.
                    continue;
                }

                // Increment before spawning so the count is never under-reported.
                active_connections.fetch_add(1, Ordering::Relaxed);
                let active = Arc::clone(&active_connections);

                thread::Builder::new()
                    .name(format!("conn-{peer}"))
                    .spawn(move || handle_connection(stream, peer, active))
                    .expect("spawn thread");
            }

            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                // No pending connection — sleep briefly before retrying.
                thread::sleep(Duration::from_millis(50));
            }

            Err(e) => {
                eprintln!("accept error: {e}");
                thread::sleep(Duration::from_millis(100));
            }
        }
    }

    // Brief grace period: let in-flight threads notice the flag and drain.
    println!(
        "Waiting for {} active connection(s) to close…",
        active_connections.load(Ordering::Relaxed)
    );
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    while active_connections.load(Ordering::Relaxed) > 0 {
        if std::time::Instant::now() >= deadline {
            eprintln!(
                "Deadline reached; {} connection(s) still open.",
                active_connections.load(Ordering::Relaxed)
            );
            break;
        }
        thread::sleep(Duration::from_millis(100));
    }

    println!("Server stopped.");
}
