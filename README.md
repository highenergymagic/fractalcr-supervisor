# Appliance services

ROMMON and the OE system compile the same supervisor, dependency graph, and local
control protocol. PID 1 only bootstraps the root bind and proc/sys/dev/run mounts,
owns the service table, drains bounded service output, reaps children, and handles
lifecycle requests. Console input, storage discovery, configuration, networking,
kernel logging, and container diagnostics execute in supervised child processes.

The authenticated image contains the service definitions in `manager.c`. Their
dependency order is checked before any service starts. Startup configuration cannot
replace executables or inject shell commands into the service table.

| System service | Requires | Responsibility |
|---|---|---|
| platform | bootstrap | Runtime mounts, immutable root, container daemon configuration |
| logging | bootstrap | Kernel ring collection with restart checkpoint |
| datastore | platform | Discover and mount existing FRDATA Btrfs; clean unmount on stop |
| configuration | datastore | Validate JSON, set hostname, publish the runtime configuration |
| networking | configuration | Apply configured addresses/routes/DNS; remove on stop |
| container-engine | datastore, networking | Delegate cgroup controllers, place Docker in its own cgroup, API readiness and health checks |
| diagnostics | container-engine | Optional, supervised one-shot container test job |
| management | bootstrap | Configuration parser, transactional apply and operational actions |
| ssh | datastore, networking, management | Local AAA, persistent host identity, interactive appliance VTY sessions |
| buildkit | datastore, networking | Optional standalone build service with persistent state |
| debug-container | container-engine | Optional privileged Debian support container |
| console | bootstrap | Serial CLI and service control; available in degraded operation |

| ROMMON service | Requires | Responsibility |
|---|---|---|
| logging | bootstrap | Kernel ring collection |
| recovery-storage | bootstrap | Discover/mount flash and hotplug USB; publish atomic inventory |
| recovery-network | bootstrap | Validate/apply the selected recovery adapter configuration |
| recovery-console | bootstrap | Monitor CLI, saved environment, authenticated recovery operations |

These services are separate processes even when several entry points share an
executable. Mounts remain in the host mount namespace. They are lifecycle units,
not security isolation boundaries. Bootstrap mounts themselves survive service
restarts; replacing PID 1 or the kernel requires booting another image.

The OS checks mount identities with `statx` before attaching filesystems. Failed
mount queries stop initialization instead of hiding existing bootstrap mounts.

## Lifecycle

Children announce readiness through a private inherited pipe, after successful
initialization. Docker instead has an HTTP API probe. A running dependency must be
ready before its consumer starts. An unavailable dependency blocks consumers.

Docker's HTTP probe is nonblocking, permits three seconds for a complete status
line, and handles fragmented replies. A pending probe neither declares readiness
nor changes the failure count. Three completed failed probes trigger recovery;
restarting the daemon discards any response from its previous process.

Stopping or restarting a provider drains its transitive dependents first, then
stops the provider. Re-enabling it restores enabled consumers in dependency order.
Explicitly stopped consumers remain stopped. Shutdown uses the same reverse order.
ROMMON remains available independently. The OS console retains its recovery
subset when storage is absent, but cannot bypass configured or unreadable AAA.
Use ROMMON 0x2142 for explicit physical configuration recovery.
Unexpected exits use bounded exponential backoff; four consecutive failures latch
the service failed until an explicit start/restart. Only sustained health resets
that failure budget. Readiness and termination deadlines prevent indefinite hangs.
One-shot jobs complete after their process group is gone and do not restart on
success. Diagnostics have a single-attempt policy.
Completed one-shot jobs remain complete across provider restart or stop/start.
A provider restart cancels an active one-shot job; it requires explicit retry.

The supervisor uses process groups and reaps adopted children. It does not claim
cgroup containment for processes that deliberately escape their group. Moby owns
containerd's separate session and arranges its parent-death signal.

Service stdout/stderr go to `/run/log/SERVICE.log`, with a 256 KiB active file and
one rotated file. Only the console service writes interactive serial output.
Boot-stage messages and service events are appliance messages; raw logs appear
only through explicit `show logging` commands. Service logs are volatile; AAA
accounting persists separately on the datastore and is visible with `show logging aaa`.

The Unix seqpacket control socket is mode 0600 and checks peer UID. Requests have
a version, fixed maximum size, enumerated operation, and bounded service name.
Accepted clients have one second to send a packet; pending reads are revisited
between service ticks, so a delayed client neither loses its connection to a
short polling race nor blocks supervision. OS service logs can still be read
when this control socket is unavailable.
There is no arbitrary execution endpoint. Stopping the console or a provider it
requires is refused; restarting either is supported. ROMMON console restart reloads saved monitor
settings and stays at the monitor prompt instead of repeating autoboot. Unsaved
monitor settings and command history belong to the console process.

## CLI

System:

```
show platform software service
request platform software service logging restart
request platform software service datastore restart
request platform software service console restart
show logging networking
```

ROMMON:

```
show services
service recovery-storage restart
service recovery-network stop
service recovery-console restart
show logging recovery-network
```

`tools/check-services.sh` compiles both profiles and runs process-backed fault
tests. Image tests additionally exercise real dependency restarts, console
reconnection, USB recovery, signed handoff, Docker readiness, and persistent data.

## Reproducing image bytes

```
.\tools\remote-build.ps1 -Stage all -BuildAction Reproduce
```

This uses the exact existing builder image by immutable Docker image ID. Each
stage regenerates its appliance code, root filesystem, and kernel twice, compares
every resulting image byte, then runs the normal VM tests. Downloaded dependency
sources and previously built dependency toolchains are retained. This is a forced
appliance rebuild test, not a claim that every upstream dependency has been rebuilt
independently on a second machine.

`reproducibility.json` is written only after both passes match. It records source
hashes, builder ID and package versions, public certificate/key hashes, the source
epoch, and output hashes. Private keys remain in the build volume. The same signing
keys and certificate bytes are required inputs; generating new development keys
intentionally produces different images.

Buildroot reproducibility mode normalizes filesystem metadata. The ROMMON kernel
build number, user, host, and timestamp are fixed. OE supplies its reproducible
build settings. The PE signing wrapper freezes only the signing process's clock
because the builder's sbsign version embeds signing time. The instant is the later
of the source epoch and certificate validity start, so Linux's certificate-time
check still succeeds. It does not change
signature algorithms, authentication policy, or kernel randomness sources.

Writable test flash/datastore images and OVMF variable stores are runtime fixtures,
not reproducible release artifacts. Their filesystems contain changing metadata,
and VM tests deliberately modify them. Release comparisons cover ROMMON EFI,
witness EFI and bundle, and the OE system EFI and bundle.
