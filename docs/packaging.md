# Packaging

Kvasir is packaged for Debian trixie, on x86-64 and arm64, as:

- `kvasir`: `kvasir-node`, its settings, its systemd unit and its manual page.
  Kvasir's own library is linked into the binary: no other program links it, so
  there is no library or development package. Dannan's shared library comes
  from Dannan's package, `libdannan0.9`, through `${shlibs:Depends}`.
- `kvasir-dbgsym`: its debug symbols.

The package is built with debhelper from `debian/`, driving the CMake build.
Dannan is always the installed package, never a copy:
`-DKVASIR_SYSTEM_DANNAN=ON` finds it with `find_package(dannan <pin> CONFIG)`,
links Kvasir's library into `kvasir-node`, and defines the install rule for the
binary. `debian/` adds the rest.

## What it installs

| Path | What |
|---|---|
| `/usr/bin/kvasir-node` | the node |
| `/etc/kvasir/config.yaml` | the settings, a conffile: listen address and port, the TLS certificate and key paths, `ack_level: ephemeral` |
| `/etc/kvasir/secrets.yaml` | the accounts, made by the first install with no accounts, mode 0600, owned by `kvasir`; never replaced, removed on purge |
| `/var/lib/kvasir` | the node's state directory (systemd's `StateDirectory`); empty in this release |
| `kvasir.service` | the systemd unit, installed disabled and not started |
| `kvasir-node(1)` | the manual page |

The system user and group `kvasir` are made by `systemd-sysusers`. Before
enabling the unit, put the certificate and key where `config.yaml` says
(`/etc/kvasir/node.pem` and `node.key`, readable by `kvasir`) and add an
account to `secrets.yaml`; the README's "Run a node" says how.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout, with Dannan's `libdannan-dev` installed:

```sh
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
```

The packages are written to `build-deb/`. The tests run inside the build;
`DEB_BUILD_OPTIONS=nocheck` skips them.

`tools/ci.sh deb` does all of it in a clean trixie (GitHub's container, or
`docker run debian:trixie` on a build node): Dannan's packages from its pinned
tag by Dannan's own `tools/build-deb.sh`, installed, then Kvasir's, then
lintian. CI runs it on x86-64 and arm64 and fails on any lintian error or
warning.

## Versions

The version comes from `git describe`. The tag `0.1.0` gives `0.1.0`; a build
three commits later gives `0.1.0+git3.gabc1234`, which sorts after `0.1.0` and
before `0.1.1`. Release tags sit on `main`, so a build on `develop`, where none
is reachable, gives the tree's version and the commit, `0.1.0+git.gabc1234`.
Uncommitted changes add `.dirty`. A build of a tag must match
`project(kvasir VERSION ...)` in `CMakeLists.txt`, or the script refuses it.
Only builds of a tag are released.
