//! Offsets & pattern scanning for Counter-Strike 2 (latest build).
//! Dependencies: windows-sys with these features:
//! [dependencies]
//! windows-sys = { version = "0.59", features = [
//!     "Win32_System_Diagnostics_ToolHelp",
//!     "Win32_System_Threading",
//!     "Win32_System_Memory",
//!     "Win32_Foundation",
//! ] }
#![cfg(windows)]

use std::mem;
use windows_sys::Win32::Foundation::{CloseHandle, HANDLE, INVALID_HANDLE_VALUE};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, Module32First, Module32Next, MODULEENTRY32, TH32CS_SNAPMODULE,
};
use windows_sys::Win32::System::Memory::ReadProcessMemory;
use windows_sys::Win32::System::Threading::{
    OpenProcess, PROCESS_QUERY_INFORMATION, PROCESS_VM_READ,
};

// ---------------------------------------------------------------------------
// Offsets for the current live CS2 build (as of April 2025)
// ---------------------------------------------------------------------------
pub struct Offsets;

impl Offsets {
    /// dwGameObjectManager (global GOM pointer)
    pub const GOM: u64 = 0x1962C00;
    /// dwEntityList (base entity list, inside GOM)
    pub const ENTITY_LIST: u64 = 0x19B5B30;
    /// dwLocalPlayerPawn (local player pawn index)
    pub const LOCAL_PLAYER_PAWN: u64 = 0x18A2BF0;
    /// dwViewAngles (client.dll + offset)
    pub const VIEW_ANGLES: u64 = 0x1A1B220;
    /// dwViewMatrix
    pub const VIEW_MATRIX: u64 = 0x1A10360;

    // You can add more as needed.
}

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------
#[derive(Debug)]
pub enum Error {
    ModuleNotFound(String),
    PatternNotFound(String),
    ReadMemoryFailed,
    InvalidProcessHandle,
    WinApi(String),
}

// ---------------------------------------------------------------------------
// Helper: get module base & size by name
// ---------------------------------------------------------------------------
pub fn get_module_base_size(pid: u32, module_name: &str) -> Result<(u64, u32), Error> {
    let snapshot = unsafe {
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    };
    if snapshot == INVALID_HANDLE_VALUE {
        return Err(Error::WinApi("CreateToolhelp32Snapshot failed".into()));
    }

    let mut entry: MODULEENTRY32 = unsafe { mem::zeroed() };
    entry.dwSize = mem::size_of::<MODULEENTRY32>() as u32;

    if unsafe { Module32First(snapshot, &mut entry) } == 0 {
        unsafe { CloseHandle(snapshot) };
        return Err(Error::ModuleNotFound(module_name.to_string()));
    }

    loop {
        let name = String::from_utf8_lossy(&entry.szModule[..entry.szModule.iter().position(|&c| c == 0).unwrap_or(entry.szModule.len())]);
        if name.eq_ignore_ascii_case(module_name) {
            let base = entry.modBaseAddr as u64;
            let size = entry.modBaseSize as u32;
            unsafe { CloseHandle(snapshot) };
            return Ok((base, size));
        }
        if unsafe { Module32Next(snapshot, &mut entry) } == 0 {
            break;
        }
    }

    unsafe { CloseHandle(snapshot) };
    Err(Error::ModuleNotFound(module_name.to_string()))
}

// ---------------------------------------------------------------------------
// Pattern parser: "48 8B 0D ? ? ? ? 48 85 C9" -> (Vec<u8>, String mask)
// ---------------------------------------------------------------------------
fn parse_pattern(pattern: &str) -> Result<(Vec<u8>, Vec<bool>), Error> {
    let tokens: Vec<&str> = pattern.split_whitespace().collect();
    let mut bytes = Vec::new();
    let mut mask = Vec::new();
    for tok in tokens {
        if tok == "?" || tok == "??" {
            bytes.push(0x00);
            mask.push(false);
        } else {
            let byte = u8::from_str_radix(tok, 16).map_err(|_| Error::WinApi("Invalid pattern byte".into()))?;
            bytes.push(byte);
            mask.push(true);
        }
    }
    Ok((bytes, mask))
}

// ---------------------------------------------------------------------------
// Scan memory in a given range
// ---------------------------------------------------------------------------
pub fn scan_range(
    handle: HANDLE,
    start: u64,
    size: usize,
    pattern: &[u8],
    mask: &[bool],
) -> Result<u64, Error> {
    let mut buffer: Vec<u8> = vec![0; size];
    let mut bytes_read: usize = 0;
    let ret = unsafe {
        ReadProcessMemory(
            handle,
            start as *const _,
            buffer.as_mut_ptr() as *mut _,
            size,
            &mut bytes_read,
        )
    };
    if ret == 0 {
        return Err(Error::ReadMemoryFailed);
    }

    let pat_len = pattern.len();
    for i in 0..(bytes_read.saturating_sub(pat_len)) {
        let mut found = true;
        for j in 0..pat_len {
            if mask[j] && buffer[i + j] != pattern[j] {
                found = false;
                break;
            }
        }
        if found {
            return Ok(start + i as u64);
        }
    }
    Err(Error::PatternNotFound("Pattern not found in range".into()))
}

// ---------------------------------------------------------------------------
// Find a pattern inside a module of a given process
// ---------------------------------------------------------------------------
pub fn find_pattern_in_module(
    pid: u32,
    module_name: &str,
    pattern_str: &str,
) -> Result<u64, Error> {
    let (base, size) = get_module_base_size(pid, module_name)?;

    let handle = unsafe { OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 0, pid) };
    if handle == 0 {
        return Err(Error::InvalidProcessHandle);
    }

    let (pattern, mask) = parse_pattern(pattern_str)?;
    let result = scan_range(handle, base, size as usize, &pattern, &mask);
    unsafe { CloseHandle(handle) };
    result
}

// ---------------------------------------------------------------------------
// Resolve RIP-relative displacement
// addr = address of the matched pattern (first byte of the instruction)
// offset = byte offset within the pattern where the displacement starts
// insn_len = length of the whole instruction (e.g., 7 for "48 8B 0D ?? ?? ?? ??")
// ---------------------------------------------------------------------------
pub fn resolve_rip_relative(handle: HANDLE, addr: u64, disp_offset: usize, insn_len: usize) -> Result<u64, Error> {
    let mut disp: i32 = 0;
    let ret = unsafe {
        ReadProcessMemory(
            handle,
            (addr + disp_offset as u64) as *const _,
            &mut disp as *mut i32 as *mut _,
            mem::size_of::<i32>(),
            &mut 0,
        )
    };
    if ret == 0 {
        return Err(Error::ReadMemoryFailed);
    }
    Ok((addr + insn_len as u64).wrapping_add(disp as u32 as u64))
}

// ---------------------------------------------------------------------------
// Convenience: scan for a pattern and resolve RIP-relative at once
// ---------------------------------------------------------------------------
pub fn scan_and_resolve(
    pid: u32,
    module_name: &str,
    pattern: &str,
    disp_offset: usize,
    insn_len: usize,
) -> Result<u64, Error> {
    let found = find_pattern_in_module(pid, module_name, pattern)?;
    let handle = unsafe { OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, 0, pid) };
    if handle == 0 {
        return Err(Error::InvalidProcessHandle);
    }
    let resolved = resolve_rip_relative(handle, found, disp_offset, insn_len);
    unsafe { CloseHandle(handle) };
    resolved
}

// ---------------------------------------------------------------------------
// Example signatures for CS2 (April 2025)
// ---------------------------------------------------------------------------

/// dwGameObjectManager: 48 8B 0D ? ? ? ? 48 85 C9 74 0A 8B 15
pub const SIG_GOM: &str = "48 8B 0D ? ? ? ? 48 85 C9 74 0A 8B 15";
pub const GOM_DISP_OFFSET: usize = 3;  // displacement starts at byte 3
pub const GOM_INS_LEN: usize = 7;      // "48 8B 0D [disp32]"

/// dwEntityList: 48 8B 0D ? ? ? ? 48 8B D3 48 89 5C 24
pub const SIG_ENTITY_LIST: &str = "48 8B 0D ? ? ? ? 48 8B D3 48 89 5C 24";
pub const ENTITY_DISP_OFFSET: usize = 3;
pub const ENTITY_INS_LEN: usize = 7;

// ---------------------------------------------------------------------------
// Optional: get GOM / entity list directly using signatures (live)
// ---------------------------------------------------------------------------
pub fn get_gom(pid: u32, client_module: &str) -> Result<u64, Error> {
    scan_and_resolve(pid, client_module, SIG_GOM, GOM_DISP_OFFSET, GOM_INS_LEN)
}

pub fn get_entity_list(pid: u32, client_module: &str) -> Result<u64, Error> {
    scan_and_resolve(pid, client_module, SIG_ENTITY_LIST, ENTITY_DISP_OFFSET, ENTITY_INS_LEN)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn print_offsets() {
        println!("Offsets for current CS2 build:");
        println!("GOM: 0x{:X}", Offsets::GOM);
        println!("EntityList: 0x{:X}", Offsets::ENTITY_LIST);
        println!("LocalPlayerPawn: 0x{:X}", Offsets::LOCAL_PLAYER_PAWN);
        println!("ViewAngles: 0x{:X}", Offsets::VIEW_ANGLES);
        println!("ViewMatrix: 0x{:X}", Offsets::VIEW_MATRIX);
    }
}
