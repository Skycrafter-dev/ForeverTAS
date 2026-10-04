# Debugger Runtime

The reference debugger discovers LLDB and a terminal bridge on the machine
running ForeverTAS. It does not use build-machine executable paths.

Precedence: `FOREVERTAS_DEBUG_LLDB` / `FOREVERTAS_DEBUG_TERMINAL`, the paths in
**Debugger tools**, bundled `debugger/`, adjacent executables, bundled `bin/`,
then `PATH`. A nonempty explicit path that is invalid is an error, not a request
to silently choose another tool. Empty paths select automatic discovery.

- Linux: LLVM LLDB with Python support and the util-linux `script` executable.
- macOS: LLDB with Python support and the BSD `script` executable.
- Windows: LLVM LLDB with Python support and `winpty.exe` from a complete
  MSYS2/Cygwin winpty installation. Keep its runtime DLLs, `winpty.dll`, and
  `winpty-agent.exe` with that installation; copying only the adapter is not
  sufficient. Configure its absolute path in **Debugger tools**.

The matching `forevertas-simulation-debug-worker` must remain in the application
directory or its `bin/` directory. A background, time-limited capability probe
checks that LLDB can execute Python through the terminal bridge. **Recheck**
refreshes runtime availability without discarding edited source or breakpoints.
Actual startup also has a prompt timeout and reports terminal failures.

The bridge remains necessary for interactive pause/Ctrl-C; a plain pipe is not
treated as a working terminal. Package/OS permissions can still prevent an
inferior process from launching even after a successful capability probe.

References: [LLDB command-line options](https://lldb.llvm.org/man/lldb.html),
[winpty runtime and adapter](https://github.com/rprichard/winpty),
[winpty adapter options](https://github.com/rprichard/winpty/blob/master/src/unix-adapter/main.cc).
