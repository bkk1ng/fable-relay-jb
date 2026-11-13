use std::collections::HashMap;
use std::env;
use std::process;

const VERSION: &str = "0.1.0";
const HELP: &str = "\
Usage: mytool <SUBCOMMAND> [OPTIONS] [ARGS...]

Subcommands:
  build    Compile the project
  run      Build and execute the project
  clean    Remove build artifacts

Options:
  -v, --verbose        Enable verbose output
  -r, --release        Build in release mode
  -j, --jobs <N>       Number of parallel jobs
      --target <T>     Target triple
      --out-dir <DIR>  Output directory
  -h, --help           Print this help and exit
      --version        Print version and exit

Examples:
  mytool build --release --target x86_64-unknown-linux-gnu
  mytool run -v -- arg1 arg2
  mytool clean --out-dir ./dist
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
    extra: Vec<String>, // args after --
}

/// Split a single `--key=value` or `-k=value` token into (key, value).
/// Returns `None` if no `=` is present.
fn split_eq(token: &str) -> Option<(&str, &str)> {
    token.find('=').map(|pos| (&token[..pos], &token[pos + 1..]))
}

fn parse_args(raw: &[String]) -> Result<Config, String> {
    // First token must be a subcommand.
    let mut iter = raw.iter().peekable();

    let subcommand_str = iter
        .next()
        .ok_or_else(|| "No subcommand provided. Try --help.".to_string())?;

    let subcommand = match subcommand_str.as_str() {
        "build" => Subcommand::Build,
        "run" => Subcommand::Run,
        "clean" => Subcommand::Clean,
        other => {
            return Err(format!(
                "Unknown subcommand '{}'. Expected build, run, or clean.",
                other
            ))
        }
    };

    let mut verbose = false;
    let mut release = false;
    let mut jobs: Option<u32> = None;
    let mut target: Option<String> = None;
    let mut out_dir: Option<String> = None;
    let mut positional: Vec<String> = Vec::new();
    let mut extra: Vec<String> = Vec::new();
    let mut past_dashdash = false;

    // Known flags that take a following value argument.
    // We need this to distinguish `-j 4` from positional `-j`.
    let value_long: &[&str] = &["--jobs", "--target", "--out-dir"];
    let value_short: &[&str] = &["-j"];

    while let Some(token) = iter.next() {
        if past_dashdash {
            extra.push(token.clone());
            continue;
        }

        if token == "--" {
            past_dashdash = true;
            continue;
        }

        // Long flags (-- prefix, but not bare "--")
        if token.starts_with("--") {
            // Try --key=value form first.
            if let Some((key, val)) = split_eq(token) {
                match key {
                    "--verbose" => {
                        return Err(format!("Flag '{}' does not accept a value.", key))
                    }
                    "--release" => {
                        return Err(format!("Flag '{}' does not accept a value.", key))
                    }
                    "--help" => {
                        return Err(format!("Flag '{}' does not accept a value.", key))
                    }
                    "--version" => {
                        return Err(format!("Flag '{}' does not accept a value.", key))
                    }
                    "--jobs" => {
                        jobs = Some(val.parse::<u32>().map_err(|_| {
                            format!("'{}' is not a valid integer for --jobs.", val)
                        })?);
                    }
                    "--target" => target = Some(val.to_string()),
                    "--out-dir" => out_dir = Some(val.to_string()),
                    other => {
                        return Err(format!("Unknown flag '{}'.", other))
                    }
                }
                continue;
            }

            // --key value form (or bare boolean flag)
            match token.as_str() {
                "--verbose" => verbose = true,
                "--release" => release = true,
                "--help" => {
                    print!("{}", HELP);
                    process::exit(0);
                }
                "--version" => {
                    println!("{}", VERSION);
                    process::exit(0);
                }
                flag if value_long.contains(&flag) => {
                    let val = iter.next().ok_or_else(|| {
                        format!("Flag '{}' requires a value.", flag)
                    })?;
                    match flag {
                        "--jobs" => {
                            jobs = Some(val.parse::<u32>().map_err(|_| {
                                format!("'{}' is not a valid integer for --jobs.", val)
                            })?);
                        }
                        "--target" => target = Some(val.clone()),
                        "--out-dir" => out_dir = Some(val.clone()),
                        _ => unreachable!(),
                    }
                }
                other => {
                    return Err(format!("Unknown flag '{}'.", other));
                }
            }
            continue;
        }

        // Short flags (-x or -x=value or combined -xyz)
        if token.starts_with('-') && token.len() > 1 {
            // -key=value form
            if let Some((key, val)) = split_eq(token) {
                match key {
                    "-v" => return Err(format!("Flag '{}' does not accept a value.", key)),
                    "-r" => return Err(format!("Flag '{}' does not accept a value.", key)),
                    "-h" => return Err(format!("Flag '{}' does not accept a value.", key)),
                    "-j" => {
                        jobs = Some(val.parse::<u32>().map_err(|_| {
                            format!("'{}' is not a valid integer for -j.", val)
                        })?);
                    }
                    other => return Err(format!("Unknown flag '{}'.", other)),
                }
                continue;
            }

            // Strip leading '-' and iterate characters for combined short flags,
            // e.g. `-vr` means verbose + release.
            let chars: Vec<char> = token[1..].chars().collect();
            let mut ci = 0;
            while ci < chars.len() {
                match chars[ci] {
                    'v' => verbose = true,
                    'r' => release = true,
                    'h' => {
                        print!("{}", HELP);
                        process::exit(0);
                    }
                    'j' => {
                        // Value may be remainder of this token or the next token.
                        let remainder: String = chars[ci + 1..].iter().collect();
                        if !remainder.is_empty() {
                            jobs = Some(remainder.parse::<u32>().map_err(|_| {
                                format!("'{}' is not a valid integer for -j.", remainder)
                            })?);
                            ci = chars.len(); // consumed all remaining chars
                        } else {
                            let val = iter.next().ok_or_else(|| {
                                "-j requires a value.".to_string()
                            })?;
                            jobs = Some(val.parse::<u32>().map_err(|_| {
                                format!("'{}' is not a valid integer for -j.", val)
                            })?);
                        }
                    }
                    other => {
                        return Err(format!("Unknown flag '-{}'.", other));
                    }
                }
                ci += 1;
            }
            continue;
        }

        // Anything else is a positional argument.
        positional.push(token.clone());
    }

    Ok(Config {
        subcommand,
        verbose,
        release,
        jobs,
        target,
        out_dir,
        positional,
        extra,
    })
}

fn main() {
    let raw: Vec<String> = env::args().skip(1).collect();

    if raw.is_empty()
        || raw[0] == "--help"
        || raw[0] == "-h"
    {
        print!("{}", HELP);
        process::exit(0);
    }

    if raw[0] == "--version" {
        println!("{}", VERSION);
        process::exit(0);
    }

    let config = match parse_args(&raw) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: {}", e);
            eprintln!("Run with --help for usage.");
            process::exit(2);
        }
    };

    println!("Resolved config:");
    println!("  subcommand : {:?}", config.subcommand);
    println!("  verbose    : {}", config.verbose);
    println!("  release    : {}", config.release);
    println!("  jobs       : {:?}", config.jobs);
    println!("  target     : {:?}", config.target);
    println!("  out_dir    : {:?}", config.out_dir);
    println!("  positional : {:?}", config.positional);
    println!("  extra (--) : {:?}", config.extra);
}
