// SPDX-License-Identifier: CC0-1.0
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

//! Write OnAir plugins in Rust. No dependencies: this file mirrors `sdk/include/onair_plugin.h` (ABI version 1) by hand and wraps the
//! host callbacks in a small safe API. A plugin implements [`Plugin`] and calls [`export_plugin!`]; build the crate as a `cdylib`.
//!
//! The C ABI is the only thing shared with OnAir, so a plugin written against it is a separate work (see docs/plugins.md).
//! Panics must not cross the boundary: the macro catches them and the plugin is reported as failed.
#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub const ABI_VERSION: u32 = 1;
pub const CAP_IQ_SINK: u32 = 0x01;
pub const CAP_EVENTS: u32 = 0x02;
pub const CAP_TICK: u32 = 0x04;
pub const CAP_SETTINGS: u32 = 0x08;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Lamp { Off = 0, Ok = 1, Warn = 2, Bad = 3 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum ColType { Text = 0, Int = 1, Float = 2, FreqHz = 3, Time = 4, Bool = 5 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum PlotKind { Spectrum = 0, Series = 1, Constellation = 2 }

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 }

// ---- the C layout (must match onair_plugin.h) ----
#[repr(C)] pub struct RawColumn { pub name: *const c_char, pub ty: i32 }
#[repr(C)] pub struct RawCell { pub ty: i32, pub i: i64, pub d: f64, pub s: *const c_char }
#[repr(C)] pub struct RawField { pub key: *const c_char, pub is_text: i32, pub d: f64, pub s: *const c_char }
#[repr(C)] #[derive(Clone, Copy, Debug)] pub struct IqMeta { pub struct_size: u32, pub centre_hz: f64, pub sample_rate: f64, pub timestamp: f64, pub first_sample_index: u64 }
#[repr(C)] pub struct RawEvent { pub struct_size: u32, pub ty: *const c_char, pub mode: *const c_char, pub time: f64, pub freq_hz: f64, pub n_fields: u32, pub fields: *const RawField }

#[repr(C)]
pub struct HostApi {
    pub struct_size: u32,
    pub abi_version: u32,
    pub caps: u32,
    pub ctx: *mut c_void,
    pub log: Option<unsafe extern "C" fn(*mut c_void, i32, *const c_char)>,
    pub get_setting_text: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *mut c_char, u32) -> i32>,
    pub get_setting_number: Option<unsafe extern "C" fn(*mut c_void, *const c_char, f64) -> f64>,
    pub get_setting_flag: Option<unsafe extern "C" fn(*mut c_void, *const c_char, i32) -> i32>,
    pub request_iq: Option<unsafe extern "C" fn(*mut c_void, f64, f64) -> i32>,
    pub table_define: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, u32, *const RawColumn) -> i32>,
    pub table_row: Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, *const RawCell) -> i32>,
    pub table_clear: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> i32>,
    pub status: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, i32) -> i32>,
    pub result_log: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> i32>,
    pub plot: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, i32, *const f32, u32, f64, f64) -> i32>,
    pub image: Option<unsafe extern "C" fn(*mut c_void, *const c_char, i32, u32, u32, *const u8, u32) -> i32>,
    pub map_point: Option<unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char, f64, f64) -> i32>,
    pub emit_event: Option<unsafe extern "C" fn(*mut c_void, *const c_char, u32, *const RawField) -> i32>,
    pub audio_out: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, f64) -> i32>,
    pub data_dir: Option<unsafe extern "C" fn(*mut c_void, *mut c_char, u32) -> i32>,
}

#[repr(C)]
pub struct VTable {
    pub struct_size: u32,
    pub abi_version: u32,
    pub caps: u32,
    pub id: *const c_char,
    pub create: Option<unsafe extern "C" fn(*const HostApi, *const c_char) -> *mut c_void>,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    pub configure: Option<unsafe extern "C" fn(*mut c_void, f64, f64)>,
    pub process_iq: Option<unsafe extern "C" fn(*mut c_void, *const f32, u32, *const IqMeta)>,
    pub on_event: Option<unsafe extern "C" fn(*mut c_void, *const RawEvent)>,
    pub tick: Option<unsafe extern "C" fn(*mut c_void, f64)>,
    pub setting_changed: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
}
unsafe impl Sync for VTable {}

// ---- the safe side ----
/// One cell of a table row.
#[derive(Clone, Debug)]
pub enum Cell<'a> { Text(&'a str), Int(i64), Float(f64), FreqHz(f64), Time(f64), Bool(bool) }

/// An automation event delivered to an action plugin.
pub struct Event<'a> { pub kind: &'a str, pub mode: &'a str, pub time: f64, pub freq_hz: f64, raw: &'a RawEvent }
impl<'a> Event<'a> {
    /// The named fields: (key, number or text).
    pub fn fields(&self) -> Vec<(String, Result<f64, String>)> {
        let mut v = Vec::new();
        if self.raw.fields.is_null() { return v; }
        for i in 0..self.raw.n_fields as usize {
            let f = unsafe { &*self.raw.fields.add(i) };
            let key = cstr(f.key);
            v.push((key, if f.is_text != 0 { Err(cstr(f.s)) } else { Ok(f.d) }));
        }
        v
    }
}

fn cstr(p: *const c_char) -> String { if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() } }
fn c(s: &str) -> CString { CString::new(s.replace('\0', " ")).unwrap_or_default() }

/// The host callbacks. Every call returns 0 when it worked and a negative number when the host refused it (limit, permission, bad argument).
#[derive(Clone, Copy)]
pub struct Host { api: *const HostApi }
unsafe impl Send for Host {}

impl Host {
    #[doc(hidden)] pub unsafe fn from_raw(api: *const HostApi) -> Host { Host { api } }
    fn a(&self) -> &HostApi { unsafe { &*self.api } }
    pub fn log(&self, level: LogLevel, msg: &str) { let a = self.a(); if let Some(f) = a.log { unsafe { f(a.ctx, level as i32, c(msg).as_ptr()) } } }
    pub fn setting_number(&self, key: &str, default: f64) -> f64 { let a = self.a(); a.get_setting_number.map_or(default, |f| unsafe { f(a.ctx, c(key).as_ptr(), default) }) }
    pub fn setting_flag(&self, key: &str, default: bool) -> bool { let a = self.a(); a.get_setting_flag.map_or(default, |f| unsafe { f(a.ctx, c(key).as_ptr(), default as i32) != 0 }) }
    pub fn setting_text(&self, key: &str) -> Option<String> {
        let a = self.a();
        let f = a.get_setting_text?;
        let mut buf = vec![0 as c_char; 600];
        let n = unsafe { f(a.ctx, c(key).as_ptr(), buf.as_mut_ptr(), buf.len() as u32) };
        if n < 0 { None } else { Some(cstr(buf.as_ptr())) }
    }
    pub fn request_iq(&self, centre_hz: f64, sample_rate: f64) -> i32 { let a = self.a(); a.request_iq.map_or(-1, |f| unsafe { f(a.ctx, centre_hz, sample_rate) }) }
    pub fn define_table(&self, id: &str, title: &str, cols: &[(&str, ColType)]) -> i32 {
        let a = self.a();
        let names: Vec<CString> = cols.iter().map(|x| c(x.0)).collect();
        let raw: Vec<RawColumn> = names.iter().zip(cols).map(|(n, x)| RawColumn { name: n.as_ptr(), ty: x.1 as i32 }).collect();
        a.table_define.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), c(title).as_ptr(), raw.len() as u32, raw.as_ptr()) })
    }
    pub fn table_row(&self, id: &str, cells: &[Cell]) -> i32 {
        let a = self.a();
        let texts: Vec<CString> = cells.iter().map(|x| if let Cell::Text(s) = x { c(s) } else { CString::default() }).collect();
        let raw: Vec<RawCell> = cells.iter().zip(&texts).map(|(x, t)| match *x {
            Cell::Text(_) => RawCell { ty: 0, i: 0, d: 0.0, s: t.as_ptr() },
            Cell::Int(v) => RawCell { ty: 1, i: v, d: 0.0, s: std::ptr::null() },
            Cell::Float(v) => RawCell { ty: 2, i: 0, d: v, s: std::ptr::null() },
            Cell::FreqHz(v) => RawCell { ty: 3, i: 0, d: v, s: std::ptr::null() },
            Cell::Time(v) => RawCell { ty: 4, i: 0, d: v, s: std::ptr::null() },
            Cell::Bool(v) => RawCell { ty: 5, i: v as i64, d: 0.0, s: std::ptr::null() },
        }).collect();
        a.table_row.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), raw.len() as u32, raw.as_ptr()) })
    }
    pub fn clear_table(&self, id: &str) -> i32 { let a = self.a(); a.table_clear.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr()) }) }
    pub fn status(&self, key: &str, text: &str, lamp: Lamp) -> i32 { let a = self.a(); a.status.map_or(-1, |f| unsafe { f(a.ctx, c(key).as_ptr(), c(text).as_ptr(), lamp as i32) }) }
    pub fn result_log(&self, line: &str) -> i32 { let a = self.a(); a.result_log.map_or(-1, |f| unsafe { f(a.ctx, c(line).as_ptr()) }) }
    pub fn plot(&self, id: &str, title: &str, kind: PlotKind, data: &[f32], x0: f64, dx: f64) -> i32 {
        let a = self.a();
        a.plot.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), c(title).as_ptr(), kind as i32, data.as_ptr(), data.len() as u32, x0, dx) })
    }
    pub fn image_rgba(&self, id: &str, width: u32, height: u32, rgba: &[u8]) -> i32 {
        let a = self.a();
        a.image.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), 0, width, height, rgba.as_ptr(), rgba.len() as u32) })
    }
    pub fn image_png(&self, id: &str, png: &[u8]) -> i32 { let a = self.a(); a.image.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), 1, 0, 0, png.as_ptr(), png.len() as u32) }) }
    pub fn map_point(&self, id: &str, label: &str, lat: f64, lon: f64) -> i32 { let a = self.a(); a.map_point.map_or(-1, |f| unsafe { f(a.ctx, c(id).as_ptr(), c(label).as_ptr(), lat, lon) }) }
    /// Becomes the automation event `plugin.<kind>`.
    pub fn emit_event(&self, kind: &str, fields: &[(&str, Result<f64, &str>)]) -> i32 {
        let a = self.a();
        let keys: Vec<CString> = fields.iter().map(|x| c(x.0)).collect();
        let texts: Vec<CString> = fields.iter().map(|x| if let Err(s) = x.1 { c(s) } else { CString::default() }).collect();
        let raw: Vec<RawField> = fields.iter().zip(keys.iter().zip(&texts)).map(|(x, (k, t))| match x.1 {
            Ok(d) => RawField { key: k.as_ptr(), is_text: 0, d, s: std::ptr::null() },
            Err(_) => RawField { key: k.as_ptr(), is_text: 1, d: 0.0, s: t.as_ptr() },
        }).collect();
        a.emit_event.map_or(-1, |f| unsafe { f(a.ctx, c(kind).as_ptr(), raw.len() as u32, raw.as_ptr()) })
    }
    /// Needs the "audio-out" permission.
    pub fn audio_out(&self, pcm: &[f32], sample_rate: f64) -> i32 { let a = self.a(); a.audio_out.map_or(-1, |f| unsafe { f(a.ctx, pcm.as_ptr(), pcm.len() as u32, sample_rate) }) }
    /// The plugin's own data folder. Needs the "filesystem" permission.
    pub fn data_dir(&self) -> Option<String> {
        let a = self.a();
        let f = a.data_dir?;
        let mut buf = vec![0 as c_char; 1024];
        if unsafe { f(a.ctx, buf.as_mut_ptr(), buf.len() as u32) } == 0 { Some(cstr(buf.as_ptr())) } else { None }
    }
}

/// Implement this and call [`export_plugin!`].
pub trait Plugin: Send + Sized + 'static {
    /// Called once per running mode (mode_id is "" for an action plugin). Return None to refuse to start.
    fn create(host: Host, mode_id: &str) -> Option<Self>;
    fn configure(&mut self, _centre_hz: f64, _sample_rate: f64) {}
    /// Interleaved I, Q samples. Return quickly: a call that takes over two seconds gets the plugin switched off.
    fn process_iq(&mut self, _iq: &[f32], _meta: &IqMeta) {}
    fn on_event(&mut self, _event: &Event) {}
    fn tick(&mut self, _now_unix: f64) {}
    fn setting_changed(&mut self, _key: &str) {}
}

#[doc(hidden)]
pub mod glue {
    use super::*;
    pub unsafe extern "C" fn create<P: Plugin>(api: *const HostApi, mode: *const c_char) -> *mut c_void {
        catch_unwind(AssertUnwindSafe(|| match P::create(Host::from_raw(api), &cstr(mode)) { Some(p) => Box::into_raw(Box::new(p)) as *mut c_void, None => std::ptr::null_mut() })).unwrap_or(std::ptr::null_mut())
    }
    pub unsafe extern "C" fn destroy<P: Plugin>(p: *mut c_void) { if !p.is_null() { let _ = catch_unwind(AssertUnwindSafe(|| drop(Box::from_raw(p as *mut P)))); } }
    pub unsafe extern "C" fn configure<P: Plugin>(p: *mut c_void, hz: f64, sr: f64) { let _ = catch_unwind(AssertUnwindSafe(|| (*(p as *mut P)).configure(hz, sr))); }
    pub unsafe extern "C" fn process_iq<P: Plugin>(p: *mut c_void, iq: *const f32, n: u32, meta: *const IqMeta) {
        let _ = catch_unwind(AssertUnwindSafe(|| (*(p as *mut P)).process_iq(std::slice::from_raw_parts(iq, n as usize * 2), &*meta)));
    }
    pub unsafe extern "C" fn on_event<P: Plugin>(p: *mut c_void, e: *const RawEvent) {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            let raw = &*e;
            let ev = Event { kind: &cstr(raw.ty), mode: &cstr(raw.mode), time: raw.time, freq_hz: raw.freq_hz, raw };
            (*(p as *mut P)).on_event(&ev)
        }));
    }
    pub unsafe extern "C" fn tick<P: Plugin>(p: *mut c_void, now: f64) { let _ = catch_unwind(AssertUnwindSafe(|| (*(p as *mut P)).tick(now))); }
    pub unsafe extern "C" fn setting_changed<P: Plugin>(p: *mut c_void, k: *const c_char) { let _ = catch_unwind(AssertUnwindSafe(|| (*(p as *mut P)).setting_changed(&cstr(k)))); }
}

/// `export_plugin!(MyPlugin, "my-plugin-id", CAP_IQ_SINK | CAP_TICK);` exports `onair_plugin_entry`. The id must equal manifest.json's.
#[macro_export]
macro_rules! export_plugin {
    ($ty:ty, $id:expr, $caps:expr) => {
        #[no_mangle]
        pub extern "C" fn onair_plugin_entry(host_abi: u32) -> *const $crate::VTable {
            static ID: &[u8] = concat!($id, "\0").as_bytes();
            static VT: $crate::VTable = $crate::VTable {
                struct_size: std::mem::size_of::<$crate::VTable>() as u32,
                abi_version: $crate::ABI_VERSION,
                caps: $caps,
                id: ID.as_ptr() as *const std::ffi::c_char,
                create: Some($crate::glue::create::<$ty>),
                destroy: Some($crate::glue::destroy::<$ty>),
                configure: Some($crate::glue::configure::<$ty>),
                process_iq: Some($crate::glue::process_iq::<$ty>),
                on_event: Some($crate::glue::on_event::<$ty>),
                tick: Some($crate::glue::tick::<$ty>),
                setting_changed: Some($crate::glue::setting_changed::<$ty>),
            };
            if host_abi >= $crate::ABI_VERSION { &VT } else { std::ptr::null() }
        }
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn layout_matches_the_c_header() {
        // sizes of the C structs on a 64-bit target (onair_plugin.h): the same numbers are asserted in C by tests/plugin/test_plugin.cpp
        assert_eq!(std::mem::size_of::<IqMeta>(), 40);
        assert_eq!(std::mem::size_of::<RawCell>(), 32);
        assert_eq!(std::mem::size_of::<RawField>(), 32);
        assert_eq!(std::mem::size_of::<RawEvent>(), 56);
        assert_eq!(std::mem::size_of::<VTable>(), 80);
        assert_eq!(std::mem::size_of::<HostApi>(), 152);
    }
}
