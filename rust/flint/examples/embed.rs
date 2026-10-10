//! Embed Flint in a Rust program: run a snippet, report the result.
//!
//! ```sh
//! cargo run --example embed -- 'print("two plus two is " + str(2 + 2))'
//! ```
//!
//! No arguments runs a small built-in demo. The exit code mirrors the
//! run: 0 for success, 65 for a compile error, 70 for a runtime error.

use flint::{Engine, Error};
use std::process::ExitCode;

fn main() -> ExitCode {
    let source: String = std::env::args().skip(1).collect::<Vec<_>>().join(" ");
    let source = if source.is_empty() {
        r#"print("two plus two is " + str(2 + 2))"#.to_string()
    } else {
        source
    };
    let engine = match Engine::new() {
        Ok(engine) => engine,
        Err(error) => {
            eprintln!("embed: cannot create the engine: {error}");
            return ExitCode::from(74);
        }
    };
    match engine.run(&source, Some("embed")) {
        Ok(()) => ExitCode::SUCCESS,
        Err(Error::Compile) => {
            eprintln!("embed: the snippet did not compile");
            ExitCode::from(65)
        }
        Err(Error::Runtime) => {
            eprintln!("embed: the snippet failed at run time");
            ExitCode::from(70)
        }
        Err(other) => {
            eprintln!("embed: {other}");
            ExitCode::from(74)
        }
    }
}
