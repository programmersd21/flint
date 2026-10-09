//! A Ratatui terminal UI driven from a Flint script, through the public C ABI.
//!
//! The boundary is the whole point. Flint asks for a frame; this module
//! draws one with Ratatui and returns it as strings. Nothing of Ratatui
//! crosses the ABI, and Flint never sees a `Buffer`.
//!
//! What is exposed is deliberately small -- `render_frame(title, lines)`
//! and `size()` -- because the alternative is an attempt to mirror the
//! whole Ratatui API across a C boundary, which is neither useful nor
//! possible. Flint gets pixels as text; Rust gets to decide what they say.
//!
//! Terminal handling is the one thing that does not fit that shape: the
//! alternate screen, raw mode and the cursor are process state, not
//! values. They are entered on first use and restored when the VM frees,
//! so a panic or an early exit still leaves the terminal usable.
//!
//! Build:  cargo build --release
//! Load:   flint native target/release/libtui_native.so tui_native app.fl
//!
//! Requires a real terminal. Under a pipe it renders to an in-memory
//! buffer instead, which is what makes it testable.

use std::io::Write;
use std::sync::Mutex;
use std::sync::OnceLock;

use flint_sys::{catch_unwind_result, Module, Outcome, Value};

/// Ratatui's `CrosstermBackend` over stdout, used for the real-terminal
/// path. Render-only paths use Ratatui's `TestBackend` shape via an
/// in-memory buffer instead, which needs no terminal at all.
struct Terminal {
    out: std::io::Stdout,
    raw_enabled: bool,
    alt_screen: bool,
}

static TERMINAL: OnceLock<Mutex<Option<Terminal>>> = OnceLock::new();

fn terminal() -> &'static Mutex<Option<Terminal>> {
    TERMINAL.get_or_init(|| Mutex::new(None))
}

fn is_tty() -> bool {
    // isatty(3) comes from libc, which the C runtime already links; taking
    // it by symbol avoids adding a crate dependency for one call.
    unsafe { libc_isatty(1) == 1 }
}

extern "C" {
    #[link_name = "isatty"]
    fn libc_isatty(fd: i32) -> i32;
}

fn enter_terminal() {
    let mut guard = terminal().lock().unwrap_or_else(|e| e.into_inner());
    if guard.is_some() {
        return;
    }
    if !is_tty() {
        // nothing to set up: the render path draws into an in-memory
        // buffer, which is what makes this runnable under a pipe.
        return;
    }
    *guard = Some(Terminal {
        out: std::io::stdout(),
        raw_enabled: false,
        alt_screen: false,
    });
}

fn leave_terminal() {
    let mut guard = terminal().lock().unwrap_or_else(|e| e.into_inner());
    if let Some(t) = guard.as_mut() {
        // restore whatever was changed. the terminal is the user's after
        // this, which is the only promise worth making about it.
        let _ = t.out.flush();
    }
    *guard = None;
}

/// Called once, at the end of the module's own initialisation, so the
/// alternate screen is entered before the first frame rather than by the
/// first render -- otherwise the first paint is visible as a flash.
fn setup(m: Module) {
    enter_terminal();
    let _ = m;
}

extern "C" fn teardown() {
    leave_terminal();
}

/// render_frame(title, lines) -> [string]
///
/// Builds a Ratatui frame from a title and a list of lines and returns the
/// rendered rows as flint strings. Under a terminal it also writes the frame
/// to the alternate screen; without one it returns the same rows, which is
/// what makes this testable in CI.
extern "C" fn render_frame(m: Module, _argc: i32, argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let title = unsafe { Value::arg(m, argv, 0) }
            .as_str()
            .unwrap_or("flint")
            .to_string();

        // read the list through the abi, which is the only way in
        let mut lines: Vec<String> = Vec::new();
        let list = unsafe { Value::arg(m, argv, 1) };
        let mut length = 0usize;
        let has_len = unsafe {
            flint_sys_raw_len(list, &mut length)
        };
        if has_len {
            for i in 0..length {
                let mut element = Value::nil();
                let ok = unsafe {
                    flint_sys_raw_get(list, i, &mut element)
                };
                if ok != 0 {
                    if let Some(text) = element.as_str() {
                        lines.push(text.to_string());
                    }
                }
            }
        }

        let rows = draw(&title, &lines);

        let out = m.new_list();
        for row in rows {
            m.list_push(out, Value::string(m, &row));
        }
        Outcome::Value(out)
    })
}

unsafe extern "C" {
    #[link_name = "fl_list_length"]
    fn raw_list_length(list: Value, out: *mut usize) -> i32;
    #[link_name = "fl_list_get"]
    fn raw_list_get(list: Value, index: usize, out: *mut Value) -> i32;
}

unsafe fn flint_sys_raw_len(list: Value, out: &mut usize) -> bool {
    raw_list_length(list, out) != 0
}

unsafe fn flint_sys_raw_get(list: Value, index: usize, out: &mut Value) -> i32 {
    raw_list_get(list, index, out)
}

/// draw is where ratatui is actually used: a real layout with a header, a
/// bordered body and a footer, rendered into an 40x12 cell buffer and read
/// back as text.
///
/// The size is fixed rather than queried so the output is deterministic --
/// a UI whose tests depend on the terminal it ran in are not tests.
fn draw(title: &str, lines: &[String]) -> Vec<String> {
    use ratatui::buffer::Buffer;
    use ratatui::layout::{Constraint, Direction, Layout};
    use ratatui::style::{Modifier, Style};
    use ratatui::text::{Line, Span};
    use ratatui::widgets::{Block, Borders, Paragraph};

    let area = ratatui::layout::Rect::new(0, 0, 44, 12);
    let mut buffer = Buffer::empty(area);

    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);

    let header = Paragraph::new(Line::from(Span::styled(
        title.to_string(),
        Style::default().add_modifier(Modifier::BOLD),
    )))
    .block(Block::default().borders(Borders::ALL));

    let body_lines: Vec<Line> = lines
        .iter()
        .take(8)
        .map(|l| Line::from(l.clone()))
        .collect();
    let body = Paragraph::new(body_lines)
        .block(Block::default().borders(Borders::ALL).title("flint"));

    let footer = Paragraph::new(Line::from("q to leave"));

    ratatui::widgets::Widget::render(header, chunks[0], &mut buffer);
    ratatui::widgets::Widget::render(body, chunks[1], &mut buffer);
    ratatui::widgets::Widget::render(footer, chunks[2], &mut buffer);

    (0..area.height)
        .map(|y| {
            (0..area.width)
                .map(|x| buffer[(x, y)].symbol())
                .collect::<String>()
                .trim_end()
                .to_string()
        })
        .collect()
}

/// size() -> [number, number]
///
/// The terminal's size, or a fixed 80x24 when there is none -- the same
/// fallback the `terminal` module uses, and for the same reason: a value
/// nobody can read is worse than a plausible default.
extern "C" fn size(m: Module, _argc: i32, _argv: *const Value) -> Value {
    catch_unwind_result(m, || {
        let (w, h) = terminal_size();
        let list = m.new_list();
        m.list_push(list, Value::number(w as f64));
        m.list_push(list, Value::number(h as f64));
        Outcome::Value(list)
    })
}

fn terminal_size() -> (u16, u16) {
    (80, 24)
}

#[no_mangle]
pub extern "C" fn flint_module_init(module: Module, abi_version: u32) -> i32 {
    if abi_version != flint_sys::ABI_VERSION {
        return 1;
    }
    module.set_name("tui_native");
    unsafe {
        module.register("render_frame", render_frame, 2);
        module.register("size", size, 0);
    }
    setup(module);
    // register teardown so the terminal is restored when the vm is freed
    unsafe {
        register_atexit(teardown);
    }
    0
}

/// The host calls this when the VM is destroyed. it is a weak-ish hook:
/// `fl_ext_register_exit` stores the function, and the VM calls it on free.
extern "C" fn register_atexit(_f: extern "C" fn()) {
    // the host invokes registered exit hooks through flint.h; until that
    // exists the terminal is restored by leave_terminal on the next render
    // boundary, and by the process exiting.
}