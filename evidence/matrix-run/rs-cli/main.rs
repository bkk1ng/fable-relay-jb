use std::env;
use std::process;

// ---------------------------------------------------------------------------
// Config types
// ---------------------------------------------------------------------------

#[derive(Debug)]
enum Subcommand {
    Build,
    Run,
    Clean,
}

#[derive(Debug)]
struct Config {
    subcommand: Subcommand,
    /// --release / -r
    release: bool,
    /// --verbose / -v
    verbose: bool,
    /// --jobs / -j <N>
    jobs: Option<u32>,
    /// --target <triple>
    target: Option<String>,
    /// --output / -o <path>
    output: Option<String>,
    /// Positional arguments after the subcommand and flags
    positional: Vec<String>,
}

// ---------------------------------------------------------------------------
// Help / version text
// ---------------------------------------------------------------------------

const VERSION: &str = env!("CARGO_PKG_VERSION");
const BIN: &str = env!("CARGO_PKG_NAME");

fn print_version() {
    println!("{} {}", BIN, VERSION);
}

fn print_help() {
    println!(
        "Usage: {bin} <SUBCOMMAND> [OPTIONS] [ARGS...]

Subcommands:
  build    Compile the project
  run      Compile and run the project
  clean    Remove build artifacts

Options:
  -r, --release          Build in release mode
  -v, --verbose          Enable verbose output
  -j, --jobs <N>         Number of parallel jobs
      --target <triple>  Cross-compilation target triple
  -o, --output <path>    Write output to <path>
  -h, --help             Print this help message
  -V, --version          Print version information

Examples:
  {bin} build --release
  {bin} run --jobs=4 src/main.rs
  {bin} clean --verbose
  {bin} build --target=aarch64-unknown-linux-gnu -o out/app",
        bin = BIN
    );
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// A minimal iterator-based argument parser that supports:
///   • long flags:          --flag
///   • long key=value:      --key=value
///   • long key value:      --key value
///   • short flags:         -f
///   • short key value:     -j 4  (single char that takes a value)
///   • `--` to stop flag parsing
///   • positional arguments
struct Args {
    inner: Vec<String>,
    pos: usize,
}

impl Args {
    fn new(raw: Vec<String>) -> Self {
        Self { inner: raw, pos: 0 }
    }

    fn peek(&self) -> Option<&str> {
        self.inner.get(self.pos).map(String::as_str)
    }

    fn next(&mut self) -> Option<&str> {
        let v = self.inner.get(self.pos).map(String::as_str);
        if v.is_some() {
            self.pos += 1;
        }
        v
    }

    /// Consume the next token as a value for the preceding flag.
    /// Returns an error string if nothing is left.
    fn next_value(&mut self, flag: &str) -> Result<String, String> {
        match self.next() {
            Some(v) if !v.starts_with('-') => Ok(v.to_owned()),
            Some(v) => Err(format!(
                "flag '{}' expects a value, got '{}'",
                flag, v
            )),
            None => Err(format!("flag '{}' expects a value", flag)),
        }
    }
}

fn parse_args(raw: Vec<String>) -> Result<Config, String> {
    let mut args = Args::new(raw);

    // ---- subcommand -------------------------------------------------------
    let subcmd_str = args
        .next()
        .ok_or_else(|| {
            format!(
                "no subcommand provided; run `{} --help` for usage",
                BIN
            )
        })?;

    // Let --help / --version fire before the subcommand is required.
    if subcmd_str == "--help" || subcmd_str == "-h" {
        print_help();
        process::exit(0);
    }
    if subcmd_str == "--version" || subcmd_str == "-V" {
        print_version();
        process::exit(0);
    }

    let subcommand = match subcmd_str {
        "build" => Subcommand::Build,
        "run" => Subcommand::Run,
        "clean" => Subcommand::Clean,
        other => {
            return Err(format!(
                "unknown subcommand '{}'; expected build, run, or clean",
                other
            ))
        }
    };

    // ---- flags ------------------------------------------------------------
    let mut release = false;
    let mut verbose = false;
    let mut jobs: Option<u32> = None;
    let mut target: Option<String> = None;
    let mut output: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();
    let mut only_positional = false;

    // Short flags that consume the next token as their value.
    const SHORT_VALUE_FLAGS: &[char] = &['j', 'o'];

    while let Some(tok) = args.peek() {
        if only_positional {
            positional.push(args.next().unwrap().to_owned());
            continue;
        }

        if tok == "--" {
            args.next(); // consume "--"
            only_positional = true;
            continue;
        }

        if tok == "--help" || tok == "-h" {
            print_help();
            process::exit(0);
        }

        if tok == "--version" || tok == "-V" {
            print_version();
            process::exit(0);
        }

        if let Some(long) = tok.strip_prefix("--") {
            args.next(); // consume the token

            // Split on first '=' for --key=value form.
            if let Some(eq) = long.find('=') {
                let (key, val) = long.split_at(eq);
                let val = &val[1..]; // drop '='
                match key {
                    "jobs" => {
                        jobs = Some(val.parse::<u32>().map_err(|_| {
                            format!("--jobs expects a positive integer, got '{}'", val)
                        })?);
                    }
                    "target" => target = Some(val.to_owned()),
                    "output" => output = Some(val.to_owned()),
                    "release" | "verbose" => {
                        return Err(format!(
                            "--{} is a boolean flag and does not take a value",
                            key
                        ))
                    }
                    other => {
                        return Err(format!("unknown flag '--{}'", other))
                    }
                }
            } else {
                // --key value  or  boolean --flag
                match long {
                    "release" => release = true,
                    "verbose" => verbose = true,
                    "jobs" => {
                        let v = args.next_value("--jobs")?;
                        jobs = Some(v.parse::<u32>().map_err(|_| {
                            format!("--jobs expects a positive integer, got '{}'", v)
                        })?);
                    }
                    "target" => {
                        target = Some(args.next_value("--target")?);
                    }
                    "output" => {
                        output = Some(args.next_value("--output")?);
                    }
                    other => {
                        return Err(format!("unknown flag '--{}'", other))
                    }
                }
            }
            continue;
        }

        if let Some(short) = tok.strip_prefix('-') {
            if short.is_empty() {
                // Bare "-" treated as a positional (stdin convention).
                positional.push(args.next().unwrap().to_owned());
                continue;
            }
            args.next(); // consume the token

            // Walk each character so "-rv" expands to -r -v.
            let chars: Vec<char> = short.chars().collect();
            let mut ci = 0;
            while ci < chars.len() {
                let c = chars[ci];
                if SHORT_VALUE_FLAGS.contains(&c) {
                    // The value is either the remainder of this token or the next.
                    let val = if ci + 1 < chars.len() {
                        // e.g. -j4
                        chars[ci + 1..].iter().collect::<String>()
                    } else {
                        args.next_value(&format!("-{}", c))?
                    };
                    match c {
                        'j' => {
                            jobs = Some(val.parse::<u32>().map_err(|_| {
                                format!("-j expects a positive integer, got '{}'", val)
                            })?);
                        }
                        'o' => output = Some(val),
                        _ => unreachable!(),
                    }
                    break; // rest of chars consumed as the value
                } else {
                    match c {
                        'r' => release = true,
                        'v' => verbose = true,
                        other => {
                            return Err(format!("unknown flag '-{}'", other))
                        }
                    }
                }
                ci += 1;
            }
            continue;
        }

        // Not a flag — positional.
        positional.push(args.next().unwrap().to_owned());
    }

    Ok(Config {
        subcommand,
        release,
        verbose,
        jobs,
        target,
        output,
        positional,
    })
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

fn main() {
    // Skip argv[0] (the binary name).
    let raw: Vec<String> = env::args().skip(1).collect();

    let config = parse_args(raw).unwrap_or_else(|err| {
        eprintln!("error: {}", err);
        eprintln!("Run `{} --help` for usage.", BIN);
        process::exit(2);
    });

    println!("{:#?}", config);
}
