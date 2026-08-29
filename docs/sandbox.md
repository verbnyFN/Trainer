# Submission sandbox

Python and C++20 submissions execute outside the backend process. For every test case, the backend
starts the fixed `algorithm-trainer-judge-worker` executable with an empty environment and one
Unix-domain socket. All unrelated inherited file descriptors are closed before `execve()`. A
bounded, length-prefixed JSON protocol carries only the language, source, standard input, and the
normalized result. The worker starts pinned NsJail directly; submitted text is never interpolated
into a command. Worker or protocol failure is an infrastructure error, with no in-process or
unsandboxed fallback.

## Limits

Each runtime execution has:

- a 2-second backend wall limit and 4-second NsJail backstop;
- 3 seconds of `RLIMIT_CPU` and one CPU of cgroup bandwidth;
- 128 MiB total cgroup memory with swap disabled, plus `RLIMIT_AS` as defense in depth;
- 4 processes for the complete cgroup, plus `RLIMIT_NPROC`;
- 16 open files, a 16 MiB stack, a 1 MiB file-size limit, and 64 KiB combined output.

C++ compilation has separate 15-second wall, 12-second CPU, 1 GiB cgroup-memory, 32-process,
16-open-file, and 16 MiB output-file limits. The compiler and linker may create their required child
processes. The trusted worker opens the output with `O_NOFOLLOW`, verifies that it is a regular file
owned by the service user, and makes only that file executable. Compiler details are normalized to
`Compilation Error` and are not returned to clients.

CPU, memory, swap, and PID limits apply through cgroup v2 to the complete NsJail process tree.
Rlimits remain per-process backstops; they are not the primary accounting mechanism.

## Filesystem, namespaces, and environment

Every execution gets a fresh mode-0700 temporary directory. Its private root is read-only and only
`/workspace` is writable. The exact pinned Nix runtime/compiler closure is mounted one store path at
a time, read-only. The repository, home directory, database, `/etc`, `/proc`, `/sys`, and general
`/dev` tree are absent. Standard input is opened by the trusted parent. RAII cleanup removes the
workspace on normal exit, error, timeout, and output-limit termination.

NsJail creates user, mount, PID, IPC, UTS, cgroup, and network namespaces. Loopback is disabled,
capabilities are dropped, and `NO_NEW_PRIVS` remains enabled. The worker receives no backend
environment. Python receives only:

```text
HOME=/workspace
PATH=
PYTHONHASHSEED=0
PYTHONDONTWRITEBYTECODE=1
PYTHONNOUSERSITE=1
```

## Syscall policy

Seccomp uses distinct default-deny Kafel allowlists for runtime programs and compilation. Runtime
allows file reading, memory management, signals, clocks, and other operations required by pinned
Python and ordinary C++ programs. It contains no networking, process creation, namespace, mount,
ptrace/process-memory, BPF, performance-event, or kernel-keyring syscall. Compilation adds only the
filesystem and process operations required by pinned Clang and its linker, while still excluding
networking, namespace, mount, ptrace, BPF, and keyring operations. Unknown syscalls fail with
`EPERM`.

## Host and deployment requirements

The host must be Linux with unprivileged user, mount, PID, IPC, UTS, cgroup, and network namespaces,
plus seccomp-BPF. It must provide a writable delegated cgroup v2 subtree with `cpu`, `memory`, and
`pids`. The backend moves itself into a trusted child cgroup and gives NsJail the empty delegated
parent. Missing delegation fails closed.

`deployment/algorithm-trainer.service` supplies `Delegate=cpu memory pids` and baseline systemd
hardening. Local launchers request an equivalent transient user scope. Containers must explicitly
provide the same writable delegation and namespace/seccomp facilities; a read-only cgroup mount or
container without delegation is unsupported.

## Verification and remaining boundary

The sandbox suite executes real Python and C++ and verifies reference solutions, timeouts, memory and
output exhaustion, compilation errors, hidden host files, blocked network sockets and child
processes, and absence of backend secrets/descriptors. Run it through CTest inside `nix develop`;
the test launcher creates its delegated transient service.

Runtime closures are read-only and contain no application data, but are broader than purpose-built
minimal images. NsJail and cgroups cannot guarantee safety against a kernel vulnerability.
Internet-facing deployments should use the dedicated service account/unit as a baseline, keep the
kernel and NsJail current, add host-appropriate mandatory access control, and monitor worker/cgroup
failures independently.
