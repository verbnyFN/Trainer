# Submission sandbox

Python and C++20 submissions are executed through the NsJail-backed `Executor`. The backend starts
the pinned NsJail binary directly with `fork()` and `execve()` and passes every argument separately.
Submitted source and test input are written to fixed filenames and are never interpolated into a
shell command. C++ compilation runs inside its own sandbox before execution. There is no shell
invocation and no unsandboxed fallback.

## Limits

Each test-case execution has these limits:

- 2 seconds of wall-clock time, enforced by the backend supervisor;
- 4 seconds as an additional NsJail wall-time backstop;
- 3 seconds of CPU time through `RLIMIT_CPU`;
- one available CPU;
- 128 MiB address space through `RLIMIT_AS`;
- 4 processes through `RLIMIT_NPROC`, with `clone`, `fork`, and `vfork` also denied by seccomp;
- 16 open files;
- 16 MiB stack;
- 1 MiB file size through `RLIMIT_FSIZE`;
- 64 KiB combined captured stdout and stderr.

The backend terminates the NsJail supervisor when the output limit is crossed and returns a non-zero
execution result. The current public verdict model has no Output Limit Exceeded verdict, so the judge
maps this condition to **Runtime Error**.

C++ compilation has a separate 15-second wall limit, 12-second CPU limit, 1 GiB address-space limit,
32-process limit, and 16 MiB file-size limit. It may create compiler/linker subprocesses, but retains
the filesystem, namespace, network, ptrace, mount, and capability restrictions. Compiler diagnostics
are reduced to a normalized `Compilation Error` category and are not returned to clients.

## Filesystem and environment

Every execution gets a new mode-0700 directory created with `mkdtemp()`. Its private root is mounted
read-only. The only writable bind mount visible to submitted programs and the compiler is
`/workspace`, containing the current source, input, and—after successful C++ compilation—the output
binary. Standard input is opened by the trusted parent and connected to the jailed process.

The exact transitive Nix closure of the selected pinned runtime/compiler is mounted one store path at
a time, read-only, at its original `/nix/store/...` location. The complete host `/nix/store` is not mounted.
No repository, home directory, database, `/etc`, `/proc`, `/sys`, or general `/dev` tree is mounted.

NsJail creates new user, mount, PID, IPC, UTS, cgroup, and network namespaces. The loopback interface
is disabled. Seccomp additionally denies socket creation and connection, process creation, namespace
changes, mounting, ptrace/process-memory access, BPF, performance events, and kernel keyring access.
Capabilities are dropped and `NO_NEW_PRIVS` remains enabled.

The host environment is cleared. Python receives only:

```text
HOME=/workspace
PATH=
PYTHONHASHSEED=0
PYTHONDONTWRITEBYTECODE=1
PYTHONNOUSERSITE=1
```

An RAII workspace owner recursively removes the temporary directory on every return path, including
normal exits, timeouts, output-limit termination, and infrastructure errors.

## NixOS and kernel requirements

Enter the pinned environment with `nix develop`. It provides NsJail, Python, and Clang without global
installation. The development-shell `nsjail --version` shim reports the pinned package version because
upstream NsJail 3.6 does not implement that option; all other arguments are forwarded to the package.
The backend is compiled with the absolute path of the real NsJail binary, not the shim.

The host must be Linux and permit the service user to create unprivileged user, mount, PID, IPC, UTS,
cgroup, and network namespaces. It must support seccomp-BPF and the required bind mounts/pivot-root
operations. On NixOS, ensure unprivileged user namespaces have not been disabled by hardening policy.
Containerized deployments must delegate the same namespace and seccomp capabilities; otherwise the
executor returns an infrastructure error and never runs submitted code outside NsJail.

The current executor uses rlimits rather than writable cgroup controllers, so it does not require a
delegated cgroup subtree.

## Known limitations

- The seccomp policies are targeted denylists, not minimal syscall allowlists. A production security
  review should replace them with runtime-specific allowlists and regression tests.
- Runtime closures are intentionally readable and include transitive dependencies. They
  contains no application data, but it is broader than a purpose-built minimal Python image.
- `RLIMIT_AS` limits virtual address space for submitted processes; it is not cgroup
  accounting across a process tree. Process-creation syscalls are denied as an additional control.
- Judging is synchronous, so deployment must also impose request and concurrency limits to prevent
  many simultaneous sandboxes from exhausting host resources.
- NsJail is one defense layer, not a guarantee against kernel vulnerabilities. An internet-facing
  deployment should use a dedicated unprivileged service account, timely kernel/NsJail updates,
  mandatory access control, and independent host-level monitoring.
