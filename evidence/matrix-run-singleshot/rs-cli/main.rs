use std::collections::HashMap;
use std::env;
use std::process;

const VERSION: &str = "0.1.0";
const HELP: &str = "\
Usage: tool <SUBCOMMAND> [OPTIONS] [ARGS]...

Subcommands:
  build    Compile the project
  run      Build and run the project
  clean    Remove build artifacts

Options:
  -v, --verbose        Enable verbose output
  -r, --release        Build in release mode
  -j, --jobs <N>       Number of parallel jobs
  -o, --output <PATH>  Output path
      --target <NAME>  Target triple
  -h, --help           Print help information
  -V, --version        Print version information

Examples:
  tool build --release
  tool run --verbose -- arg1 arg2
  tool clean
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
    output: Option<String>,
    target: Option<String>,
    positional: Vec<String>,
    extra: Vec<String>, // args after --
    flags: HashMap<String, Option<String>>,
}

#[derive(Debug)]
enum ParseError {
    NoSubcommand,
    UnknownSubcommand(String),
    UnknownFlag(String),
    MissingValue(String),
    InvalidValue { flag: String, value: String },
}

impl ParseError {
    fn message(&self) -> String {
        match self {
            Self::NoSubcommand => {
                "error: no subcommand provided; expected one of: build, run, clean".to_string()
            }
            Self::UnknownSubcommand(s) => {
                format!("error: unknown subcommand '{s}'; expected one of: build, run, clean")
            }
            Self::UnknownFlag(f) => {
                format!("error: unknown flag '{f}'")
            }
            Self::MissingValue(f) => {
                format!("error: flag '{f}' requires a value")
            }
            Self::InvalidValue { flag, value } => {
                format!("error: invalid value '{value}' for flag '{flag}'")
            }
        }
    }
}

/// Known flags and whether they take a value.
/// (long, short, takes_value)
const FLAG_SPECS: &[(&str, Option<char>, bool)] = &[
    ("verbose", Some('v'), false),
    ("release", Some('r'), false),
    ("jobs",    Some('j'), true),
    ("output",  Some('o'), true),
    ("target",  None,      true),
    ("help",    Some('h'), false),
    ("version", Some('V'), false),
];

fn flag_takes_value(name: &str) -> Option<bool> {
    FLAG_SPECS
        .iter()
        .find(|(long, short, _)| {
            *long == name
                || short.map_or(false, |c| c.to_string() == name)
        })
        .map(|(_, _, tv)| *tv)
}

fn short_to_long(c: char) -> Option<&'static str> {
    FLAG_SPECS
        .iter()
        .find(|(_, short, _)| *short == Some(c))
        .map(|(long, _, _)| *long)
}

fn parse_args(args: &[String]) -> Result<Config, ParseError> {
    let mut iter = args.iter().peekable();

    // ---- subcommand -------------------------------------------------------
    let raw_sub = iter.next().ok_or(ParseError::NoSubcommand)?;

    // Allow --help / --version before the subcommand.
    if raw_sub == "--help" || raw_sub == "-h" {
        print!("{HELP}");
        process::exit(0);
    }
    if raw_sub == "--version" || raw_sub == "-V" {
        println!("{VERSION}");
        process::exit(0);
    }

    let subcommand = match raw_sub.as_str() {
        "build" => Subcommand::Build,
        "run"   => Subcommand::Run,
        "clean" => Subcommand::Clean,
        other   => return Err(ParseError::UnknownSubcommand(other.to_string())),
    };

    // ---- flags and positional args ----------------------------------------
    let mut verbose  = false;
    let mut release  = false;
    let mut jobs: Option<u32>    = None;
    let mut output: Option<String> = None;
    let mut target: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();
    let mut extra:      Vec<String> = Vec::new();
    let mut flags:      HashMap<String, Option<String>> = HashMap::new();

    let mut past_separator = false;

    while let Some(arg) = iter.next() {
        if past_separator {
            extra.push(arg.clone());
            continue;
        }

        if arg == "--" {
            past_separator = true;
            continue;
        }

        if arg.starts_with("--") {
            // Long flag: --key, --key=value, --key value
            let body = &arg[2..];
            let (key, inline_val) = if let Some(eq) = body.find('=') {
                (&body[..eq], Some(body[eq + 1..].to_string()))
            } else {
                (body, None)
            };

            match flag_takes_value(key) {
                None => return Err(ParseError::UnknownFlag(format!("--{key}"))),
                Some(false) => {
                    if inline_val.is_some() {
                        return Err(ParseError::UnknownFlag(format!("--{key}")));
                    }
                    apply_bool_flag(key, &mut verbose, &mut release, &mut flags);
                }
                Some(true) => {
                    let val = if let Some(v) = inline_val {
                        v
                    } else {
                        iter.next()
                            .cloned()
                            .ok_or_else(|| ParseError::MissingValue(format!("--{key}")))?
                    };
                    apply_value_flag(
                        key, val, &mut jobs, &mut output, &mut target, &mut flags,
                    )?;
                }
            }
        } else if arg.starts_with('-') && arg.len() > 1 {
            // Short flags: -v, -j4, -j 4, -vr (stacked booleans)
            let chars: Vec<char> = arg[1..].chars().collect();
            let mut ci = 0;
            while ci < chars.len() {
                let c = chars[ci];
                let long = short_to_long(c)
                    .ok_or_else(|| ParseError::UnknownFlag(format!("-{c}")))?;

                match flag_takes_value(long).unwrap() {
                    false => {
                        apply_bool_flag(long, &mut verbose, &mut release, &mut flags);
                        ci += 1;
                    }
                    true => {
                        // Remainder of the token is the value, or next token.
                        let val = if ci + 1 < chars.len() {
                            let v: String = chars[ci + 1..].iter().collect();
                            ci = chars.len(); // consumed
                            v
                        } else {
                            iter.next()
                                .cloned()
                                .ok_or_else(|| ParseError::MissingValue(format!("-{c}")))?
                        };
                        apply_value_flag(
                            long, val, &mut jobs, &mut output, &mut target, &mut flags,
                        )?;
                        ci = chars.len();
                    }
                }
            }
        } else {
            positional.push(arg.clone());
        }
    }

    Ok(Config {
        subcommand,
        verbose,
        release,
        jobs,
        output,
        target,
        positional,
        extra,
        flags,
    })
}

fn apply_bool_flag(
    key: &str,
    verbose: &mut bool,
    release: &mut bool,
    flags: &mut HashMap<String, Option<String>>,
) {
    match key {
        "verbose" => *verbose = true,
        "release" => *release = true,
        "help" => {
            print!("{HELP}");
            process::exit(0);
        }
        "version" => {
            println!("{VERSION}");
            process::exit(0);
        }
        other => {
            flags.insert(other.to_string(), None);
        }
    }
}

fn apply_value_flag(
    key: &str,
    val: String,
    jobs:   &mut Option<u32>,
    output: &mut Option<String>,
    target: &mut Option<String>,
    flags:  &mut HashMap<String, Option<String>>,
) -> Result<(), ParseError> {
    match key {
        "jobs" => {
            let n = val.parse::<u32>().map_err(|_| ParseError::InvalidValue {
                flag:  "--jobs".to_string(),
                value: val.clone(),
            })?;
            *jobs = Some(n);
        }
        "output" => *output = Some(val),
        "target" => *target = Some(val),
        other => {
            flags.insert(other.to_string(), Some(val));
        }
    }
    Ok(())
}

fn main() {
    let raw: Vec<String> = env::args().skip(1).collect();

    if raw.is_empty() {
        eprintln!("{}", ParseError::NoSubcommand.message());
        eprintln!("Run with --help for usage.");
        process::exit(2);
    }

    let config = match parse_args(&raw) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("{}", e.message());
            eprintln!("Run with --help for usage.");
            process::exit(2);
        }
    };

    println!("{config:#?}");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(s: &str) -> Vec<String> {
        s.split_whitespace().map(str::to_string).collect()
    }

    #[test]
    fn build_no_flags() {
        let c = parse_args(&args("build")).unwrap();
        assert!(matches!(c.subcommand, Subcommand::Build));
        assert!(!c.verbose);
        assert!(!c.release);
    }

    #[test]
    fn run_verbose_release() {
        let c = parse_args(&args("run --verbose --release")).unwrap();
        assert!(matches!(c.subcommand, Subcommand::Run));
        assert!(c.verbose);
        assert!(c.release);
    }

    #[test]
    fn short_flags_stacked() {
        let c = parse_args(&args("build -vr")).unwrap();
        assert!(c.verbose);
        assert!(c.release);
    }

    #[test]
    fn jobs_eq_syntax() {
        let c = parse_args(&args("build --jobs=4")).unwrap();
        assert_eq!(c.jobs, Some(4));
    }

    #[test]
    fn jobs_space_syntax() {
        let c = parse_args(&args("build --jobs 8")).unwrap();
        assert_eq!(c.jobs, Some(8));
    }

    #[test]
    fn short_jobs_adjacent() {
        let c = parse_args(&args("build -j4")).unwrap();
        assert_eq!(c.jobs, Some(4));
    }

    #[test]
    fn short_jobs_space() {
        let c = parse_args(&args("build -j 2")).unwrap();
        assert_eq!(c.jobs, Some(2));
    }

    #[test]
    fn output_and_target() {
        let c = parse_args(&args("build --output=dist --target x86_64-unknown-linux-gnu")).unwrap();
        assert_eq!(c.output.as_deref(), Some("dist"));
        assert_eq!(c.target.as_deref(), Some("x86_64-unknown-linux-gnu"));
    }

    #[test]
    fn positional_args() {
        let c = parse_args(&args("run foo bar baz")).unwrap();
        assert_eq!(c.positional, vec!["foo", "bar", "baz"]);
    }

    #[test]
    fn extra_args_after_separator() {
        let c = parse_args(&args("run -- --some-arg value")).unwrap();
        assert_eq!(c.extra, vec!["--some-arg", "value"]);
        assert!(c.positional.is_empty());
    }

    #[test]
    fn unknown_subcommand() {
        let err = parse_args(&args("deploy")).unwrap_err();
        assert!(matches!(err, ParseError::UnknownSubcommand(_)));
    }

    #[test]
    fn unknown_long_flag() {
        let err = parse_args(&args("build --frobnicate")).unwrap_err();
        assert!(matches!(err, ParseError::UnknownFlag(_)));
    }

    #[test]
    fn unknown_short_flag() {
        let err = parse_args(&args("build -z")).unwrap_err();
        assert!(matches!(err, ParseError::UnknownFlag(_)));
    }

    #[test]
    fn missing_value_for_flag() {
        let err = parse_args(&args("build --jobs")).unwrap_err();
        assert!(matches!(err, ParseError::MissingValue(_)));
    }

    #[test]
    fn invalid_jobs_value() {
        let err = parse_args(&args("build --jobs=abc")).unwrap_err();
        assert!(matches!(err, ParseError::InvalidValue { .. }));
    }

    #[test]
    fn no_subcommand() {
        let err = parse_args(&[]).unwrap_err();
        assert!(matches!(err, ParseError::NoSubcommand));
    }
}
