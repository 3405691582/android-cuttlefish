# vhost-device-vsock on OpenBSD (work in progress)

Status: prototype, proven end to end outside of this tree; not wired into
the Bazel build yet. Nothing here changes QEMU.

## What this is

OpenBSD has no kernel vhost, so cuttlefish's usual `vhost-vsock-pci` device
cannot exist there, and the OS has no `AF_VSOCK`. The way out is the
transport cuttlefish already has for crosvm: `--vhost_user_vsock`, where
QEMU's `vhost-user-vsock-pci` front-end hands the device to this daemon over
a Unix socket and the host tools talk to the daemon over Unix sockets with
the Firecracker-style `CONNECT <port>` handshake (`fd.cc` `VsockServer`,
`shared_fd.cpp` `VsockClient`). Two things had to be shown:

1. QEMU exposes `vhost-user-vsock-pci` on OpenBSD when built with
   `--enable-vhost-user --disable-vhost-net`. It does, unchanged (QEMU
   10.2.1): `meson.build` only auto-disables vhost-user off Linux, kicks
   and calls fall back to pipe pairs, and `-object memory-backend-shm,
   share=on -machine memory-backend=mem` gives the daemon fd-backed guest
   RAM. The package QEMU is built without the flag.
2. This daemon runs on OpenBSD. It does, with the patches in this
   directory. The vsock engine itself is portable; what assumed Linux were
   the utility crates: `eventfd(2)` and `epoll(7)` in `vmm-sys-util`, the
   `epoll` crate, and a `getsockopt(SO_DOMAIN)` in `vhost`.

Verified with an Alpine guest under TCG on OpenBSD 7.9: `/dev/vsock` in
the guest, connections in both directions, 32 MiB transfers in both
directions with matching checksums; the crates' own test suites pass on
OpenBSD (vmm-sys-util 62 + 17, vhost 42, vhost-user-backend 21,
vhost-device-vsock 32).

## Files

The crate versions match the ones resolved for the pinned `vhost-device`
revision (`vmm-sys-util` 0.15.0, `vhost` 0.17.0, `vhost-user-backend`
0.23.0, `epoll` 4.4.0); the patches apply to the crates.io sources with
`-p1`.

* `vmm-sys-util-openbsd.patch`: new `src/bsd/{mod,eventfd,epoll}.rs`,
  selected in `src/lib.rs` for `target_os = "openbsd"`. `EventFd` is a
  `socketpair(2)` with eventfd read/write semantics; `from_raw_fd` wraps a
  foreign descriptor (QEMU's pipe ends) used one way. `Epoll` sits on
  `kqueue(2)` and keeps epoll's observable behaviour where consumers depend
  on it: one merged event per descriptor per wait (kqueue reports the read
  and write filters separately; the daemon unregisters a descriptor on its
  first event and would otherwise meet a stale second one), and `EEXIST`
  on a repeated add / `ENOENT` on modify or delete of an unregistered
  descriptor (checked with a side-effect-free `EV_ENABLE|EV_RECEIPT` probe;
  `vhost-user-backend`'s tests rely on it). Also a fix in
  `src/unix/sock_ctrl_msg.rs` that is not OpenBSD-specific: `CmsgHdr` was
  sized as `sizeof(struct cmsghdr)`, but the BSDs pad the header to
  `CMSG_LEN(0)`, so SCM_RIGHTS messages were laid out wrong on every BSD
  and macOS. Unit tests included.
* `vhost-openbsd.patch`: `backend_req_handler.rs` checks the socket family
  with `SO_DOMAIN`, which only Linux has; `getsockname()` elsewhere.
* `epoll-kq/`: a stand-in for the `epoll` crate (the subset of its 4.4.0
  API the daemon uses) over the kqueue helpers that the vmm-sys-util patch
  exports. The real crate wraps `libc::epoll_*` and cannot build off
  Linux. The cleaner end state is for `vhost-device-vsock` to use
  `vmm_sys_util::epoll` directly, which removes the need for this crate.

## Left to do

* Wire the patches into `vhost_device_vsock.MODULE.bazel`:
  `vhost_device_vsock_crates.annotation(crate = "vmm-sys-util", patches =
  [...], patch_args = ["-p1"])` and the same for `vhost`; both are no-ops
  on Linux (the new modules are `cfg`-gated and `CMSG_LEN(0)` equals
  `sizeof(struct cmsghdr)` there). Decide how the `epoll` crate is
  replaced (annotation patch on the crate, or the upstream change to
  `vhost-device-vsock`).
* Upstream the crate changes to rust-vmm (the cmsg fix first; then the
  BSD modules, the `SO_DOMAIN` change, the `epoll` crate removal) and ask
  the OpenBSD `emulators/qemu` port to enable vhost-user.
* `qemu_manager.cpp`: a `--vhost_user_vsock` branch (`-chardev socket,
  id=vsock,path=<vhost.socket> -device vhost-user-vsock-pci,chardev=vsock`
  plus the shared memory backend) and drop the `#ifdef __linux__` around
  the vsock device and the vsock proxies in favour of `vhost_user_vsock()`.
  Linux QEMU mode lacks that branch as well.
* Not exercised yet: daemon reconnect, many concurrent connections, guest
  reboot.
