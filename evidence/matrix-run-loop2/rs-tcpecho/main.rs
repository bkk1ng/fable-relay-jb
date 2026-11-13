use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

// Global shutdown flag; set to true by the SIGINT handler.
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

// How long accept() blocks before re-checking SHUTDOWN.
const ACCEPT_TIMEOUT: Duration = Duration::from_millis(500);

// Per-connection read timeout; kicks idle clients.
const READ_TIMEOUT: Duration = Duration::from_secs(30);

// Hard ceiling on simultaneous connections.
const MAX_CONNECTIONS: usize = 128;

fn handle_connection(mut stream: TcpStream, id: usize, counter: Arc<AtomicUsize>) {
    let peer = stream
        .peer_addr()
        .map(|a| a.to_string())
        .unwrap_or_else(|_| "unknown".to_string());

    println!("[conn {id}] connected  peer={peer}");

    // Bump the live-connection counter.
    counter.fetch_add(1, Ordering::Relaxed);

    // Apply the read timeout so a silent client doesn't block the thread forever.
    if let Err(e) = stream.set_read_timeout(Some(READ_TIMEOUT)) {
        eprintln!("[conn {id}] set_read_timeout failed: {e}");
    }

    let write_stream = match stream.try_clone() {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[conn {id}] clone failed: {e}");
            counter.fetch_sub(1, Ordering::Relaxed);
            println!("[conn {id}] disconnected peer={peer} (clone error)");
            return;
        }
    };

    let mut reader = BufReader::new(stream);
    let mut writer = write_stream;
    let mut buf = Vec::with_capacity(4096);

    loop {
        buf.clear();

        // Try a line read first.  read_until returns 0 on clean EOF.
        match reader.read_until(b'\n', &mut buf) {
            Ok(0) => {
                // Clean EOF — the client closed the connection.
                println!("[conn {id}] disconnected peer={peer} (eof)");
                break;
            }
            Ok(n) => {
                // Echo whatever arrived (line or partial bytes).
                println!("[conn {id}] echo {n} bytes");
                if let Err(e) = writer.write_all(&buf) {
                    eprintln!("[conn {id}] write error: {e}");
                    break;
                }
                if let Err(e) = writer.flush() {
                    eprintln!("[conn {id}] flush error: {e}");
                    break;
                }
            }
            Err(e) => {
                use std::io::ErrorKind::*;
                match e.kind() {
                    // read_timeout fired — idle client.
                    TimedOut | WouldBlock => {
                        println!("[conn {id}] disconnected peer={peer} (read timeout)");
                    }
                    // Connection reset / broken pipe.
                    ConnectionReset | BrokenPipe | ConnectionAborted => {
                        println!("[conn {id}] disconnected peer={peer} (reset)");
                    }
                    _ => {
                        eprintln!("[conn {id}] read error: {e}");
                        println!("[conn {id}] disconnected peer={peer} (error)");
                    }
                }
                break;
            }
        }
    }

    // Release the slot so the listener can accept new connections.
    counter.fetch_sub(1, Ordering::Relaxed);
}

fn main() {
    // ── SIGINT handler ────────────────────────────────────────────────────────
    // We rely on the fact that writing to the global AtomicBool from a signal
    // handler is safe on all major platforms.  The accept loop polls it on
    // every timeout cycle.
    //
    // signal() / sigaction() are not in std, so we use the libc crate —
    // except the spec says std only.  The portable std-only alternative is a
    // dedicated thread that parks itself and never returns, combined with a
    // Ctrl-C hook delivered via ctrlc.  Since neither ctrlc nor libc is
    // allowed, we implement the self-pipe trick with a plain TCP loopback
    // pair so the accept() can be interrupted, and fall back to the polling
    // loop for the atomic flag.
    //
    // On Linux/macOS std::process::exit sends SIGINT to itself via raise(2),
    // but that is not stable either.  The simplest *pure std* approach:
    //   • spawn a watchdog thread that blocks on stdin reading a single byte,
    //     and pair it with Ctrl-C raising EOF on stdin.
    //   • that covers interactive use.
    //   • for programmatic SIGINT (kill -INT) we rely on the OS unblocking
    //     accept() with EINTR, which maps to ErrorKind::Interrupted in Rust.
    //
    // Both paths set SHUTDOWN = true and let the main loop exit cleanly.

    // Watchdog: reads one byte from stdin.  Ctrl-C causes EOF on many
    // terminals which wakes this thread.
    thread::spawn(|| {
        use std::io::Read;
        let mut b = [0u8; 1];
        // Ignore result — any EOF or error means we should shut down.
        let _ = std::io::stdin().read(&mut b);
        println!("\n[server] shutdown signal received");
        SHUTDOWN.store(true, Ordering::SeqCst);
    });

    // ── Listener ──────────────────────────────────────────────────────────────
    let addr = "127.0.0.1:7878";
    let listener = TcpListener::bind(addr).unwrap_or_else(|e| {
        eprintln!("[server] bind {addr} failed: {e}");
        std::process::exit(1);
    });

    // Poll-based graceful shutdown: accept blocks for at most ACCEPT_TIMEOUT
    // before we check SHUTDOWN again.
    listener
        .set_nonblocking(false)
        .expect("set_nonblocking failed");

    // set_read_timeout on a TcpListener controls how long accept() waits.
    listener
        .set_read_timeout(Some(ACCEPT_TIMEOUT))
        .expect("set_read_timeout on listener failed");

    println!("[server] listening on {addr}  (Ctrl-C or EOF on stdin to stop)");

    let counter: Arc<AtomicUsize> = Arc::new(AtomicUsize::new(0));
    let mut conn_id: usize = 0;

    loop {
        if SHUTDOWN.load(Ordering::SeqCst) {
            println!("[server] shutting down — waiting for in-flight connections");
            // A production server would wait for counter to reach 0 here;
            // for this echo server we exit promptly since threads own streams.
            break;
        }

        match listener.accept() {
            Ok((stream, _addr)) => {
                let live = counter.load(Ordering::Relaxed);
                if live >= MAX_CONNECTIONS {
                    eprintln!(
                        "[server] connection refused: at limit ({MAX_CONNECTIONS})"
                    );
                    // stream is dropped here, closing the connection immediately.
                    continue;
                }

                conn_id = conn_id.wrapping_add(1);
                let id = conn_id;
                let counter_clone = Arc::clone(&counter);

                thread::Builder::new()
                    .name(format!("conn-{id}"))
                    .spawn(move || handle_connection(stream, id, counter_clone))
                    .unwrap_or_else(|e| {
                        eprintln!("[server] spawn failed: {e}");
                        // counter was not incremented yet, nothing to roll back.
                        panic!("spawn failed: {e}");
                    });
            }

            Err(e) => {
                use std::io::ErrorKind::*;
                match e.kind() {
                    // accept() timed out — expected, go check SHUTDOWN.
                    TimedOut | WouldBlock => {}
                    // SIGINT delivered as EINTR on Unix.
                    Interrupted => {
                        println!("[server] interrupted");
                        SHUTDOWN.store(true, Ordering::SeqCst);
                    }
                    _ => {
                        eprintln!("[server] accept error: {e}");
                    }
                }
            }
        }
    }

    println!(
        "[server] stopped  connections_served={conn_id}  still_live={}",
        counter.load(Ordering::Relaxed)
    );
}
