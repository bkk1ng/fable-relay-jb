use std::env;
use std::process;

// ---------------------------------------------------------------------------
// Subcommand
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, PartialEq)]
enum Subcommand {
    Build,
    Run,
    Clean,
}

impl Subcommand {
    fn from_str(s: &str) -> Option<Self> {
        match s {
            "build" => Some(Self::Build),
            "run"   => Some(Self::Run),
            "clean" => Some(Self::Clean),
            _       => None,
        }
    }
}

impl std::fmt::Display for Subcommand {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Build => write!(f, "build"),
            Self::Run   => write!(f, "run"),
            Self::Clean => write!(f, "clean"),
        }
    }
}

// ---------------------------------------------------------------------------
// Resolved config
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct Config {
    subcommand: Subcommand,
    /// --release / -r
    release: bool,
    /// --verbose / -v
    verbose: bool,
    /// --jobs N / -j N
    jobs: Option<u32>,
    /// --target <value>
    target: Option<String>,
    /// --output <value> / -o <value>
    output: Option<String>,
    /// Positional arguments after the subcommand
    positional: Vec<String>,
}

// ---------------------------------------------------------------------------
// Help / version text
// ---------------------------------------------------------------------------

const VERSION: &str = "0.1.0";

fn print_version() {
    println!("mytool {VERSION}");
}

fn print_help(program: &str) {
    println!(
        "Usage: {program} <SUBCOMMAND> [OPTIONS] [ARGS...]

Subcommands:
  build   Compile the project
  run     Build and run the project
  clean   Remove build artifacts

Options:
  -r, --release         Build in release mode
  -v, --verbose         Enable verbose output
  -j, --jobs <N>        Number of parallel jobs
      --target <VALUE>  Target triple
  -o, --output <VALUE>  Output path
  -h, --help            Print this help message
      --version         Print version information

Examples:
  {program} build --release
  {program} run --jobs=4 -- my_binary arg1 arg2
  {program} clean -v"
    );
}

// ---------------------------------------------------------------------------
// Argument parser
// ---------------------------------------------------------------------------

/// Thin wrapper so we can carry position info in error messages.
struct Args {
    argv: Vec<String>,
    pos: usize,
}

impl Args {
    fn new(argv: Vec<String>) -> Self {
        Self { argv, pos: 0 }
    }

    fn peek(&self) -> Option<&str> {
        self.argv.get(self.pos).map(String::as_str)
    }

    fn next(&mut self) -> Option<&str> {
        let v = self.argv.get(self.pos).map(String::as_str);
        if v.is_some() {
            self.pos += 1;
        }
        v
    }

    /// Consume the next token as a value for `flag`, or error.
    fn require_value(&mut self, flag: &str) -> Result<String, String> {
        match self.next() {
            Some(v) if !v.starts_with('-') => Ok(v.to_owned()),
            Some(v) => Err(format!(
                "flag '{flag}' requires a value, got '{v}'"
            )),
            None => Err(format!("flag '{flag}' requires a value")),
        }
    }
}

fn parse_args(program: &str, mut args: Args) -> Result<Config, String> {
    // ---- subcommand -------------------------------------------------------
    let sub_token = loop {
        match args.peek() {
            None => {
                return Err(format!(
                    "no subcommand given; try '{program} --help'"
                ))
            }
            // Let --help / --version fire before we demand a subcommand.
            Some("--help") | Some("-h") => {
                print_help(program);
                process::exit(0);
            }
            Some("--version") => {
                print_version();
                process::exit(0);
            }
            Some(t) if t.starts_with('-') => {
                return Err(format!(
                    "unexpected flag '{t}' before subcommand; try '{program} --help'"
                ))
            }
            Some(_) => break args.next().unwrap(),
        }
    };

    let subcommand = Subcommand::from_str(sub_token).ok_or_else(|| {
        format!("unknown subcommand '{sub_token}'; expected build, run, or clean")
    })?;

    // ---- flags ------------------------------------------------------------
    let mut release = false;
    let mut verbose = false;
    let mut jobs: Option<u32> = None;
    let mut target: Option<String> = None;
    let mut output: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();
    let mut end_of_flags = false;

    while let Some(tok) = args.next() {
        if end_of_flags {
            positional.push(tok.to_owned());
            continue;
        }

        // "--" terminates flag parsing
        if tok == "--" {
            end_of_flags = true;
            continue;
        }

        // Long flags: --key=value or --key value
        if let Some(rest) = tok.strip_prefix("--") {
            let (key, inline_val) = match rest.split_once('=') {
                Some((k, v)) => (k, Some(v)),
                None => (rest, None),
            };

            match key {
                "help" => {
                    print_help(program);
                    process::exit(0);
                }
                "version" => {
                    print_version();
                    process::exit(0);
                }
                "release" => {
                    if inline_val.is_some() {
                        return Err("flag '--release' takes no value".to_owned());
                    }
                    release = true;
                }
                "verbose" => {
                    if inline_val.is_some() {
                        return Err("flag '--verbose' takes no value".to_owned());
                    }
                    verbose = true;
                }
                "jobs" => {
                    let raw = match inline_val {
                        Some(v) => v.to_owned(),
                        None => args.require_value("--jobs")?,
                    };
                    jobs = Some(raw.parse::<u32>().map_err(|_| {
                        format!("'--jobs' expects a positive integer, got '{raw}'")
                    })?);
                }
                "target" => {
                    target = Some(match inline_val {
                        Some(v) => v.to_owned(),
                        None => args.require_value("--target")?,
                    });
                }
                "output" => {
                    output = Some(match inline_val {
                        Some(v) => v.to_owned(),
                        None => args.require_value("--output")?,
                    });
                }
                unknown => {
                    return Err(format!("unknown flag '--{unknown}'"));
                }
            }
            continue;
        }

        // Short flags: -r, -v, -j N, -o VALUE  (may be clustered: -rv)
        if let Some(shorts) = tok.strip_prefix('-') {
            if shorts.is_empty() {
                // bare '-' — treat as positional
                positional.push(tok.to_owned());
                continue;
            }

            let chars: Vec<char> = shorts.chars().collect();
            let mut i = 0;
            while i < chars.len() {
                match chars[i] {
                    'h' => {
                        print_help(program);
                        process::exit(0);
                    }
                    'r' => release = true,
                    'v' => verbose = true,
                    'j' => {
                        // Value is remainder of this token or next token.
                        let raw = if i + 1 < chars.len() {
                            let tail: String = chars[i + 1..].iter().collect();
                            i = chars.len(); // consume rest
                            tail
                        } else {
                            args.require_value("-j")?
                        };
                        jobs = Some(raw.parse::<u32>().map_err(|_| {
                            format!("'-j' expects a positive integer, got '{raw}'")
                        })?);
                    }
                    'o' => {
                        let raw = if i + 1 < chars.len() {
                            let tail: String = chars[i + 1..].iter().collect();
                            i = chars.len();
                            tail
                        } else {
                            args.require_value("-o")?
                        };
                        output = Some(raw);
                    }
                    c => {
                        return Err(format!("unknown short flag '-{c}'"));
                    }
                }
                i += 1;
            }
            continue;
        }

        // Not a flag — positional.
        positional.push(tok.to_owned());
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
    let mut argv: Vec<String> = env::args().collect();
    let program = argv.remove(0);

    match parse_args(&program, Args::new(argv)) {
        Ok(cfg) => {
            println!("Resolved config:");
            println!("  subcommand : {}", cfg.subcommand);
            println!("  release    : {}", cfg.release);
            println!("  verbose    : {}", cfg.verbose);
            println!("  jobs       : {}", cfg.jobs.map_or("(default)".into(), |n| n.to_string()));
            println!("  target     : {}", cfg.target.as_deref().unwrap_or("(none)"));
            println!("  output     : {}", cfg.output.as_deref().unwrap_or("(none)"));
            println!("  positional : {:?}", cfg.positional);
        }
        Err(e) => {
            eprintln!("error: {e}");
            process::exit(2);
        }
    }
}
