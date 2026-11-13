// mycli — a demo subcommand CLI argument parser.
// Rust 2021, std only (no clap).
//
// Build:  cargo build
// Run:    cargo run -- <SUBCOMMAND> [OPTIONS] [POSITIONALS]...
// Example:cargo run -- build --release --target x86_64-unknown-linux-gnu -j4 out/

use std::collections::{HashMap, HashSet};
use std::env;
use std::process;

const VERSION_TEXT: &str = "mycli 0.1.0";

const TOP_HELP: &str = "\
mycli 0.1.0 — a demo subcommand CLI

USAGE:
    mycli <SUBCOMMAND> [OPTIONS] [POSITIONALS]...
    mycli <SUBCOMMAND> -h | --help
    mycli -h | --help
    mycli -V | --version

SUBCOMMANDS:
    build   Compile the project, optionally with a target triple and job count
    run     Build and run the project, forwarding positionals as program args
    clean   Remove generated build artifacts

OPTIONS:
    -h, --help       Print help for the CLI or a subcommand
    -V, --version    Print version information
";

const BUILD_HELP: &str = "\
mycli build — compile the project

USAGE:
    mycli build [OPTIONS] [TARGET_DIR]...

OPTIONS:
    -r, --release            Build in release mode
        --target <TRIPLE>    Build for the given target triple
    -j, --jobs <N>           Number of parallel jobs
    -h, --help               Print this help message

POSITIONALS:
    TARGET_DIR               Optional target directories to build into
";

const RUN_HELP: &str = "\
mycli run — build and run the project

USAGE:
    mycli run [OPTIONS] [-- PROGRAM_ARGS]...

OPTIONS:
    -r, --release    Build in release mode before running
    -h, --help       Print this help message

POSITIONALS:
    PROGRAM_ARGS     Arguments forwarded to the built program
";

const CLEAN_HELP: &str = "\
mycli clean — remove build artifacts

USAGE:
    mycli clean [OPTIONS] [PATH]...

OPTIONS:
    -n, --dry-run    Show what would be removed without deleting
    -v, --verbose    Print each path as it is removed
    -h, --help       Print this help message

POSITIONALS:
    PATH             Paths to clean (defaults to the project target dir)
";

// ---- Flag specifications ----------------------------------------------------

/// Describes one flag a subcommand accepts.
#[derive(Clone, Copy)]
struct Spec {
    long: &'static str,
    short: Option<char>,
    takes_value: bool,
}

fn specs_for(sub: &str) -> Vec<Spec> {
    // `-h` / `--help` is available on every subcommand.
    let help = Spec {
        long: "help",
        short: Some('h'),
        takes_value: false,
    };
    match sub {
        "build" => vec![
            Spec {
                long: "release",
                short: Some('r'),
                takes_value: false,
            },
            Spec {
                long: "target",
                short: Some('t'),
                takes_value: true,
            },
            Spec {
                long: "jobs",
                short: Some('j'),
                takes_value: true,
            },
            help,
        ],
        "run" => vec![
            Spec {
                long: "release",
                short: Some('r'),
                takes_value: false,
            },
            help,
        ],
        "clean" => vec![
            Spec {
                long: "dry-run",
                short: Some('n'),
                takes_value: false,
            },
            Spec {
                long: "verbose",
                short: Some('v'),
                takes_value: false,
            },
            help,
        ],
        _ => vec![],
    }
}

// ---- Parsed flags container -------------------------------------------------

/// Raw result of parsing one subcommand's argument list.
struct Flags {
    set: HashSet<String>,
    values: HashMap<String, Vec<String>>,
    positionals: Vec<String>,
}

impl Flags {
    fn bool(&self, name: &str) -> bool {
        self.set.contains(name)
    }
    fn single(&self, name: &str) -> Option<&str> {
        self.values
            .get(name)
            .and_then(|v| v.last())
            .map(|s| s.as_str())
    }
}

// ---- Parser -----------------------------------------------------------------

/// Walks `args` (the tokens following the subcommand) and resolves them against
/// `specs`, returning the collected bool/value flags and positionals.
///
/// Supports:
///   --flag            boolean long flag
///   --flag=value      long flag with an inline value
///   --flag value      long flag with the value taken from the next token
///   -f                boolean short flag
///   -fvalue           short flag whose value is the rest of the token
///   -f value           short flag with the value taken from the next token
///   -rv               combined boolean short flags
///   --                everything after is treated as positional
fn parse(args: &[String], specs: &[Spec]) -> Result<Flags, String> {
    let mut flags = Flags {
        set: HashSet::new(),
        values: HashMap::new(),
        positionals: Vec::new(),
    };
    let by_short: HashMap<char, &Spec> = specs
        .iter()
        .filter_map(|s| s.short.map(|c| (c, s)))
        .collect();
    let by_long: HashMap<&str, &Spec> = specs.iter().map(|s| (s.long, s)).collect();

    let mut i = 0;
    let mut only_positionals = false;
    while i < args.len() {
        let arg = &args[i];
        if only_positionals {
            flags.positionals.push(arg.clone());
            i += 1;
            continue;
        }
        if arg == "--" {
            only_positionals = true;
            i += 1;
            continue;
        }
        if let Some(rest) = arg.strip_prefix("--") {
            // Long flag: split `name=value` if present.
            let (name, inline) = match rest.split_once('=') {
                Some((n, v)) => (n, Some(v.to_string())),
                None => (rest, None),
            };
            let spec = match by_long.get(name) {
                Some(s) => *s,
                None => return Err(format!("unknown flag `--{}`", name)),
            };
            if spec.takes_value {
                let value = match inline {
                    Some(v) => v,
                    None => {
                        i += 1;
                        if i >= args.len() {
                            return Err(format!("flag `--{}` requires a value", name));
                        }
                        args[i].clone()
                    }
                };
                flags
                    .values
                    .entry(spec.long.to_string())
                    .or_default()
                    .push(value);
            } else {
                if inline.is_some() {
                    return Err(format!("flag `--{}` does not take a value", name));
                }
                flags.set.insert(spec.long.to_string());
            }
        } else if arg.starts_with('-') && arg.len() > 1 {
            // Short flag(s): one or more chars, the first value-taking one ends the token.
            let chars: Vec<char> = arg[1..].chars().collect();
            let mut k = 0;
            while k < chars.len() {
                let c = chars[k];
                let spec = match by_short.get(&c) {
                    Some(s) => *s,
                    None => return Err(format!("unknown flag `-{}`", c)),
                };
                if spec.takes_value {
                    let rest: String = chars[k + 1..].iter().collect();
                    let value = if !rest.is_empty() {
                        rest
                    } else {
                        i += 1;
                        if i >= args.len() {
                            return Err(format!("flag `-{}` requires a value", c));
                        }
                        args[i].clone()
                    };
                    flags
                        .values
                        .entry(spec.long.to_string())
                        .or_default()
                        .push(value);
                    break;
                } else {
                    flags.set.insert(spec.long.to_string());
                    k += 1;
                }
            }
        } else {
            flags.positionals.push(arg.clone());
        }
        i += 1;
    }
    Ok(flags)
}

// ---- Typed configs ----------------------------------------------------------

struct BuildConfig {
    release: bool,
    target: Option<String>,
    jobs: Option<usize>,
    positionals: Vec<String>,
}

struct RunConfig {
    release: bool,
    positionals: Vec<String>,
}

struct CleanConfig {
    dry_run: bool,
    verbose: bool,
    positionals: Vec<String>,
}

enum Config {
    Build(BuildConfig),
    Run(RunConfig),
    Clean(CleanConfig),
}

fn build_config(flags: &Flags) -> Result<BuildConfig, String> {
    let jobs = match flags.single("jobs") {
        Some(v) => match v.parse::<usize>() {
            Ok(n) => Some(n),
            Err(_) => return Err(format!("invalid value for `--jobs`: `{}`", v)),
        },
        None => None,
    };
    Ok(BuildConfig {
        release: flags.bool("release"),
        target: flags.single("target").map(|s| s.to_string()),
        jobs,
        positionals: flags.positionals.clone(),
    })
}

fn run_config(flags: &Flags) -> RunConfig {
    RunConfig {
        release: flags.bool("release"),
        positionals: flags.positionals.clone(),
    }
}

fn clean_config(flags: &Flags) -> CleanConfig {
    CleanConfig {
        dry_run: flags.bool("dry-run"),
        verbose: flags.bool("verbose"),
        positionals: flags.positionals.clone(),
    }
}

// ---- Pretty print -----------------------------------------------------------

fn print_config(cfg: &Config) {
    match cfg {
        Config::Build(b) => {
            println!("resolved config:");
            println!("  subcommand:   build");
            println!("  release:       {}", b.release);
            println!(
                "  target:        {}",
                b.target.as_deref().unwrap_or("(none)")
            );
            println!(
                "  jobs:          {}",
                b.jobs.map_or("(none)".to_string(), |j| j.to_string())
            );
            println!("  positionals:   {:?}", b.positionals);
        }
        Config::Run(r) => {
            println!("resolved config:");
            println!("  subcommand:    run");
            println!("  release:       {}", r.release);
            println!("  positionals:   {:?}", r.positionals);
        }
        Config::Clean(c) => {
            println!("resolved config:");
            println!("  subcommand:    clean");
            println!("  dry_run:       {}", c.dry_run);
            println!("  verbose:       {}", c.verbose);
            println!("  positionals:   {:?}", c.positionals);
        }
    }
}

// ---- main -------------------------------------------------------------------

/// Prints an error to stderr and exits with status 2.
fn fail(msg: &str) -> ! {
    eprintln!("error: {}", msg);
    process::exit(2);
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let rest = &args[1..];

    if rest.is_empty() {
        print!("{}", TOP_HELP);
        process::exit(0);
    }

    let sub = rest[0].as_str();
    let sub_args = &rest[1..];

    // Top-level help / version take precedence over subcommand dispatch.
    match sub {
        "-h" | "--help" => {
            print!("{}", TOP_HELP);
            process::exit(0);
        }
        "-V" | "--version" => {
            println!("{}", VERSION_TEXT);
            process::exit(0);
        }
        "build" | "run" | "clean" => {}
        other => {
            fail(&format!("unknown subcommand `{}`", other));
        }
    }

    let specs = specs_for(sub);
    let flags = match parse(sub_args, &specs) {
        Ok(f) => f,
        Err(e) => fail(&e),
    };

    // A subcommand `-h` / `--help` prints that subcommand's help and exits 0.
    if flags.bool("help") {
        match sub {
            "build" => print!("{}", BUILD_HELP),
            "run" => print!("{}", RUN_HELP),
            "clean" => print!("{}", CLEAN_HELP),
            _ => {}
        }
        process::exit(0);
    }

    let cfg = match sub {
        "build" => match build_config(&flags) {
            Ok(c) => Config::Build(c),
            Err(e) => fail(&e),
        },
        "run" => Config::Run(run_config(&flags)),
        "clean" => Config::Clean(clean_config(&flags)),
        _ => unreachable!("subcommand validated above"),
    };

    print_config(&cfg);
}
