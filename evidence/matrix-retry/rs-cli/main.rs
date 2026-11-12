use std::env;
use std::process;

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

#[derive(Debug)]
enum Subcommand {
    Build,
    Run,
    Clean,
}

impl Subcommand {
    fn parse(s: &str) -> Option<Self> {
        match s {
            "build" => Some(Self::Build),
            "run" => Some(Self::Run),
            "clean" => Some(Self::Clean),
            _ => None,
        }
    }

    fn as_str(&self) -> &'static str {
        match self {
            Self::Build => "build",
            Self::Run => "run",
            Self::Clean => "clean",
        }
    }
}

// ---------------------------------------------------------------------------
// Resolved configuration
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct Config {
    subcommand: Subcommand,
    /// --release / -r
    release: bool,
    /// --verbose / -v
    verbose: bool,
    /// --jobs=N / --jobs N / -j N
    jobs: Option<u32>,
    /// --target=TRIPLE / --target TRIPLE
    target: Option<String>,
    /// --output=PATH / --output PATH / -o PATH
    output: Option<String>,
    /// Positional arguments after the subcommand (e.g. binary name, extra paths)
    positional: Vec<String>,
}

// ---------------------------------------------------------------------------
// Help / version text
// ---------------------------------------------------------------------------

const VERSION: &str = "0.1.0";

fn print_version() {
    println!("mytool {VERSION}");
}

fn print_help(subcommand: Option<&str>) {
    match subcommand {
        Some("build") => println!(
            "Usage: mytool build [OPTIONS] [ARGS]...

Build the project.

Options:
  -r, --release          Build in release mode
  -v, --verbose          Enable verbose output
  -j, --jobs <N>         Number of parallel jobs
      --target <TRIPLE>  Cross-compile for the given target triple
  -o, --output <PATH>    Write output to PATH
  -h, --help             Print this help
"
        ),
        Some("run") => println!(
            "Usage: mytool run [OPTIONS] [ARGS]...

Build and run the project.

Options:
  -r, --release          Run the release build
  -v, --verbose          Enable verbose output
  -j, --jobs <N>         Number of parallel jobs
      --target <TRIPLE>  Cross-compile for the given target triple
  -h, --help             Print this help
"
        ),
        Some("clean") => println!(
            "Usage: mytool clean [OPTIONS]

Remove build artifacts.

Options:
  -v, --verbose  Enable verbose output
  -h, --help     Print this help
"
        ),
        _ => println!(
            "Usage: mytool <SUBCOMMAND> [OPTIONS] [ARGS]...

Subcommands:
  build   Build the project
  run     Build and run the project
  clean   Remove build artifacts

Options:
  -h, --help     Print this help
  -V, --version  Print version

Run `mytool <SUBCOMMAND> --help` for subcommand-specific options.
"
        ),
    }
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// A minimal iterator-based argument parser.
struct ArgParser {
    args: Vec<String>,
    pos: usize,
}

impl ArgParser {
    fn new(args: Vec<String>) -> Self {
        Self { args, pos: 0 }
    }

    fn peek(&self) -> Option<&str> {
        self.args.get(self.pos).map(String::as_str)
    }

    fn next(&mut self) -> Option<&str> {
        let v = self.args.get(self.pos).map(String::as_str);
        if v.is_some() {
            self.pos += 1;
        }
        v
    }

    /// Consume the next token as the value for `flag`, or report an error.
    fn require_value(&mut self, flag: &str) -> Result<String, String> {
        match self.next() {
            Some(v) if !v.starts_with('-') => Ok(v.to_owned()),
            Some(other) => Err(format!(
                "flag `{flag}` requires a value, got `{other}`"
            )),
            None => Err(format!("flag `{flag}` requires a value")),
        }
    }
}

// ---------------------------------------------------------------------------
// Top-level parse entry point
// ---------------------------------------------------------------------------

fn parse_args(raw: Vec<String>) -> Result<Config, String> {
    let mut parser = ArgParser::new(raw);

    // ---- Look for top-level --help / --version before the subcommand -------
    // We do a quick pre-scan so `mytool --help` and `mytool --version` work
    // even before a subcommand is provided.
    {
        let first = parser.peek();
        if matches!(first, Some("-h") | Some("--help")) {
            print_help(None);
            process::exit(0);
        }
        if matches!(first, Some("-V") | Some("--version")) {
            print_version();
            process::exit(0);
        }
    }

    // ---- Subcommand --------------------------------------------------------
    let sub_str = match parser.next() {
        Some(s) => s.to_owned(),
        None => {
            print_help(None);
            process::exit(0);
        }
    };

    let subcommand = match Subcommand::parse(&sub_str) {
        Some(s) => s,
        None => {
            return Err(format!("unknown subcommand `{sub_str}`"));
        }
    };

    // ---- Flags & positional args ------------------------------------------
    let mut release = false;
    let mut verbose = false;
    let mut jobs: Option<u32> = None;
    let mut target: Option<String> = None;
    let mut output: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();

    // Once we see `--`, everything after is positional.
    let mut end_of_flags = false;

    while let Some(arg) = parser.next() {
        if end_of_flags {
            positional.push(arg.to_owned());
            continue;
        }

        if arg == "--" {
            end_of_flags = true;
            continue;
        }

        // --- Long flags (--flag or --flag=value) ----------------------------
        if let Some(rest) = arg.strip_prefix("--") {
            // Split on first `=` for --key=value syntax.
            let (key, inline_val) = match rest.split_once('=') {
                Some((k, v)) => (k, Some(v)),
                None => (rest, None),
            };

            match key {
                "help" | "h" => {
                    print_help(Some(subcommand.as_str()));
                    process::exit(0);
                }
                "version" | "V" => {
                    print_version();
                    process::exit(0);
                }
                "release" => {
                    if inline_val.is_some() {
                        return Err(format!("`--release` does not take a value"));
                    }
                    release = true;
                }
                "verbose" => {
                    if inline_val.is_some() {
                        return Err(format!("`--verbose` does not take a value"));
                    }
                    verbose = true;
                }
                "jobs" => {
                    let raw_val = match inline_val {
                        Some(v) => v.to_owned(),
                        None => parser.require_value("--jobs")?,
                    };
                    let n: u32 = raw_val.parse().map_err(|_| {
                        format!("`--jobs` expects a positive integer, got `{raw_val}`")
                    })?;
                    jobs = Some(n);
                }
                "target" => {
                    let val = match inline_val {
                        Some(v) => v.to_owned(),
                        None => parser.require_value("--target")?,
                    };
                    target = Some(val);
                }
                "output" => {
                    let val = match inline_val {
                        Some(v) => v.to_owned(),
                        None => parser.require_value("--output")?,
                    };
                    output = Some(val);
                }
                other => {
                    return Err(format!("unknown flag `--{other}`"));
                }
            }

            continue;
        }

        // --- Short flags (-x or combined -xyz) ------------------------------
        if let Some(shorts) = arg.strip_prefix('-') {
            if shorts.is_empty() {
                // A bare `-` is treated as a positional (stdin convention).
                positional.push(arg.to_owned());
                continue;
            }

            let chars: Vec<char> = shorts.chars().collect();
            let mut i = 0;
            while i < chars.len() {
                match chars[i] {
                    'h' => {
                        print_help(Some(subcommand.as_str()));
                        process::exit(0);
                    }
                    'V' => {
                        print_version();
                        process::exit(0);
                    }
                    'r' => {
                        release = true;
                    }
                    'v' => {
                        verbose = true;
                    }
                    'j' => {
                        // Value may be glued (-j4) or separate (-j 4).
                        let raw_val = if i + 1 < chars.len() {
                            // Rest of the cluster is the value: -j4
                            let v: String = chars[i + 1..].iter().collect();
                            i = chars.len(); // consume rest
                            v
                        } else {
                            parser.require_value("-j")?
                        };
                        let n: u32 = raw_val.parse().map_err(|_| {
                            format!("`-j` expects a positive integer, got `{raw_val}`")
                        })?;
                        jobs = Some(n);
                        break; // already advanced i past end of cluster
                    }
                    'o' => {
                        let val = if i + 1 < chars.len() {
                            let v: String = chars[i + 1..].iter().collect();
                            i = chars.len();
                            v
                        } else {
                            parser.require_value("-o")?
                        };
                        output = Some(val);
                        break;
                    }
                    other => {
                        return Err(format!("unknown flag `-{other}`"));
                    }
                }
                i += 1;
            }

            continue;
        }

        // --- Positional argument --------------------------------------------
        positional.push(arg.to_owned());
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
// Main
// ---------------------------------------------------------------------------

fn main() {
    // Skip argv[0] (the binary name).
    let raw: Vec<String> = env::args().skip(1).collect();

    let config = parse_args(raw).unwrap_or_else(|err| {
        eprintln!("error: {err}");
        eprintln!("Run `mytool --help` for usage.");
        process::exit(2);
    });

    println!("{config:#?}");
}
