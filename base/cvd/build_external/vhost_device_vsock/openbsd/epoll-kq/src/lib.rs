// Copyright 2026 Google LLC
// SPDX-License-Identifier: BSD-3-Clause

//! kqueue-backed stand-in for the `epoll` crate on OpenBSD.  Only the API
//! surface used by the rust-vmm daemons is provided; the translation lives in
//! vmm-sys-util's `epoll` module so both crates agree on it.
#[macro_use]
extern crate bitflags;

use std::io;
use std::os::fd::RawFd;

use vmm_sys_util::epoll::{epoll_event, kq_create, kq_ctl, kq_wait, ControlOperation};

#[allow(non_camel_case_types)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum ControlOptions {
    EPOLL_CTL_ADD = 1,
    EPOLL_CTL_MOD = 3,
    EPOLL_CTL_DEL = 2,
}

bitflags! {
    pub struct Events: u32 {
        const EPOLLET = 1 << 31;
        const EPOLLIN = 0x001;
        const EPOLLERR = 0x008;
        const EPOLLHUP = 0x010;
        const EPOLLOUT = 0x004;
        const EPOLLPRI = 0x002;
        const EPOLLRDHUP = 0x2000;
        const EPOLLWAKEUP = 1 << 29;
        const EPOLLONESHOT = 1 << 30;
        const EPOLLEXCLUSIVE = 1 << 28;
    }
}

#[repr(C, packed)]
#[derive(Clone, Copy, Debug)]
pub struct Event {
    pub events: u32,
    pub data: u64,
}

impl Event {
    pub fn new(events: Events, data: u64) -> Event {
        Event { events: events.bits(), data }
    }
}

pub fn create(_cloexec: bool) -> io::Result<RawFd> {
    kq_create()
}

pub fn ctl(epfd: RawFd, op: ControlOptions, fd: RawFd, event: Event) -> io::Result<()> {
    let op = match op {
        ControlOptions::EPOLL_CTL_ADD => ControlOperation::Add,
        ControlOptions::EPOLL_CTL_MOD => ControlOperation::Modify,
        ControlOptions::EPOLL_CTL_DEL => ControlOperation::Delete,
    };
    let (events, data) = (event.events, event.data);
    kq_ctl(epfd, op, fd, events, data)
}

pub fn wait(epfd: RawFd, timeout: i32, buf: &mut [Event]) -> io::Result<usize> {
    // SAFETY: Event and epoll_event have the same packed layout.
    let raw: &mut [epoll_event] =
        unsafe { std::slice::from_raw_parts_mut(buf.as_mut_ptr() as *mut epoll_event, buf.len()) };
    kq_wait(epfd, timeout, raw)
}

pub fn close(epfd: RawFd) -> io::Result<()> {
    // SAFETY: closing a descriptor handed out by create().
    if unsafe { libc::close(epfd) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
