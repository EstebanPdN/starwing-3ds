# Updater dependency provenance

The release links curl 8.4.0, Jansson 2.14 and mbedTLS 2.28.8. `sources.json`
records source archives, official devkitPro 3DS patches and the public CA set.
The retained licenses govern their respective components.

Prepare static ARMv6K/hard-float libraries with the devkitPro 3DS toolchain and
pass their include/lib prefix as `STARWING_UPDATE_DEPS_ROOT`. No precompiled
libraries or workstation build directories are committed.

Apply `curl.patch` and `mbedtls.patch` to the corresponding pinned sources.
The TLS configuration enables hardware entropy and CMAC and disables platform
entropy, self-tests and timing support for the console. Build curl with
mbedTLS, HTTP/1.1 and synchronous DNS. Exclude optional compression, HTTP/2,
IDN, SSH, LDAP, NTLM helpers, Unix sockets, socketpair and threaded resolver.
Build Jansson statically without its examples and tests. Disable TLS programs
and tests. Keep matching headers with each library prefix.

On CMake 4, older dependency projects may need
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`. Normalize source paths with the compiler's
`-ffile-prefix-map` when producing distributable libraries. The final artifact
checks and manifest identify the actual linked archives independently of
source version labels.

The certificate bundle is installed as `romfs:/update-ca.crt`; it contains only
public CA certificates. HTTPS hostname and certificate validation stay enabled.
