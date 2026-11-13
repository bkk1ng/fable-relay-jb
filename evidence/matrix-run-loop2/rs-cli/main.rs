use std::collections::HashMap;
use std::env;
use std::process;

const VERSION: &str = "0.1.0";
const HELP: &str = "\
Usage: mytool <SUBCOMMAND> [OPTIONS] [ARGS...]

Subcommands:
  build    Compile the project
  run      Build and run the project
  clean    Remove build artifacts

Options:
  -v, --verbose        Enable verbose output
  -r, --release        Build in release mode
  -j, --jobs <N>       Number of parallel jobs
      --target <T>     Target triple
      --out-dir <DIR>  Output directory
  -h, --help           Print this help message
      --version        Print version information

Examples:
  mytool build --release
  mytool run --target x86_64-unknown-linux-gnu
  mytool clean -v
";

#[derive(Debug)]
enum Subcommand {
    Build,
    Run,
    Clean,
}

#[derive(Debug)]
struct Config {
    subcommand: Subcommand,
    verbose: bool,
    release: bool,
    jobs: Option<u32>,
    target: Option<String>,
    out_dir: Option<String>,
    positional: Vec<String>,
    extra_flags: HashMap<String, Option<String>>,
}

/// Splits a `--key=value` token into `("--key", Some("value"))`.
/// Returns `(token, None)` if there is no `=`.
fn split_eq(token: &str) -> (&str, Option<&str>) {
    match token.find('=') {
        Some(pos) => (&token[..pos], Some(&token[pos + 1..])),
        None => (token, None),
    }
}

fn parse_args(args: &[String]) -> Result<Config, String> {
    if args.is_empty() {
        return Err("no subcommand provided\nRun with --help for usage.".to_owned());
    }

    // ── subcommand ────────────────────────────────────────────────────────────
    let subcommand = match args[0].as_str() {
        "build" => Subcommand::Build,
        "run"   => Subcommand::Run,
        "clean" => Subcommand::Clean,
        other   => {
            return Err(format!(
                "unknown subcommand `{other}`\nRun with --help for usage."
            ))
        }
    };

    // ── flag parsing ──────────────────────────────────────────────────────────
    let mut verbose  = false;
    let mut release  = false;
    let mut jobs: Option<u32>       = None;
    let mut target: Option<String>  = None;
    let mut out_dir: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();
    let mut extra_flags: HashMap<String, Option<String>> = HashMap::new();

    let mut i = 1usize;
    while i < args.len() {
        let token = &args[i];

        // end-of-flags sentinel
        if token == "--" {
            i += 1;
            while i < args.len() {
                positional.push(args[i].clone());
                i += 1;
            }
            break;
        }

        let is_long  = token.starts_with("--");
        let is_short = !is_long && token.starts_with('-') && token.len() == 2;

        if is_long || is_short {
            let (flag, inline_val) = split_eq(token);

            // Returns (owned_value, did_consume_next_token).
            // Errors if a value is required but nothing follows.
            macro_rules! require_value {
                ($name:expr) => {{
                    match inline_val {
                        Some(v) => (v.to_owned(), false),
                        None => {
                            if i + 1 >= args.len() {
                                return Err(format!(
                                    "flag `{}` requires a value",
                                    $name
                                ));
                            }
                            (args[i + 1].clone(), true)
                        }
                    }
                }};
            }

            match flag {
                "--help" | "-h" => {
                    print!("{HELP}");
                    process::exit(0);
                }
                "--version" => {
                    println!("mytool {VERSION}");
                    process::exit(0);
                }
                "--verbose" | "-v" => {
                    verbose = true;
                }
                "--release" | "-r" => {
                    release = true;
                }
                "--jobs" | "-j" => {
                    let (val, consumed) = require_value!(flag);
                    jobs = Some(val.parse::<u32>().map_err(|_| {
                        format!("`--jobs` expects a positive integer, got `{val}`")
                    })?);
                    if consumed { i += 1; }
                }
                "--target" => {
                    let (val, consumed) = require_value!(flag);
                    target = Some(val);
                    if consumed { i += 1; }
                }
                "--out-dir" => {
                    let (val, consumed) = require_value!(flag);
                    out_dir = Some(val);
                    if consumed { i += 1; }
                }
                unknown => {
                    // Collect the unknown flag, then surface an error immediately.
                    extra_flags.insert(unknown.to_owned(), inline_val.map(str::to_owned));
                    return Err(format!(
                        "unknown flag `{unknown}`\nRun with --help for usage."
                    ));
                }
            }
        } else {
            positional.push(token.clone());
        }

        i += 1;
    }

    Ok(Config {
        subcommand,
        verbose,
        release,
        jobs,
        target,
        out_dir,
        positional,
        extra_flags,
    })
}

fn main() {
    let raw: Vec<String> = env::args().skip(1).collect();

    // Handle --help / --version before the subcommand check so they work
    // as `mytool --help` with no subcommand present.
    for arg in &raw {
        match arg.as_str() {
            "--help" | "-h" => {
                print!("{HELP}");
                process::exit(0);
            }
            "--version" => {
                println!("mytool {VERSION}");
                process::exit(0);
            }
            _ => {}
        }
    }

    let config = parse_args(&raw).unwrap_or_else(|err| {
        eprintln!("error: {err}");
        process::exit(2);
    });

    // ── print resolved configuration ──────────────────────────────────────────
    println!("Subcommand : {:?}", config.subcommand);
    println!("Verbose    : {}", config.verbose);
    println!("Release    : {}", config.release);
    println!(
        "Jobs       : {}",
        config.jobs.map(|n| n.to_string()).unwrap_or_else(|| "(default)".to_owned())
    );
    println!(
        "Target     : {}",
        config.target.as_deref().unwrap_or("(host)")
    );
    println!(
        "Out dir    : {}",
        config.out_dir.as_deref().unwrap_or("(default)")
    );
    if !config.positional.is_empty() {
        println!("Positional : {:?}", config.positional);
    }
}
