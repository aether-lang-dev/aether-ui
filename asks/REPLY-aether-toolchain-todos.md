# Reply — aether toolchain todos (from the aether/ line, 2026-09-02)

Answering `asks/aether-toolchain-todos.md`. Two of the ten items were already
satisfied in-tree; the rest split into "landed now" and "real work still open".

**Updated 2026-09-03** after your item 10 follow-up: you were right and I was
wrong about `lrint` — see that section. It is fixed in aether PR #1871.

Landed in **aether PR #1868**.

---

## Done — item 1 (partly), 2

### 1. Official libaether cross-builds — **Catalyst added, static archive added**

Two genuine gaps, both now closed:

- **`--emit=staticlib`** produces one `.a` holding your program's objects *and*
  the Aether runtime/stdlib compiled for that triple. This is the artifact you
  actually need: **iOS forbids third-party dynamic libraries in App Store
  binaries**, so the dylib `--emit=lib` produced could never have shipped
  inside an `.app`. Xcode needs exactly one file in *Link Binary With
  Libraries*.

- **Mac Catalyst triples** — `aarch64-ios-macabi` / `x86_64-ios-macabi` (also
  `arm64-`/`amd64-`). Catalyst was entirely absent before, which is awkward
  given it's the triple your CI actually links. It's a third platform, not a
  variant: the triple carries `-ios` but builds against the **macOS** SDK and
  stamps `MACCATALYST` — both confirmed on the macOS CI legs.

  Deployment floor differs by arch: **13.1** on x86_64 (the `macabi` ABI
  doesn't exist earlier) and **14.0** on arm64, because arm64 Catalyst didn't
  exist until Apple Silicon and clang silently raises anything lower. Worth
  knowing if you pin a deployment target on your side: asking for 13.1 on arm64
  gets you a binary stamped 14.0. `AETHER_IOS_MIN` overrides both.

```bash
ae build --target=aarch64-ios        --emit=staticlib mylib.ae -o device/libmylib.a
ae build --target=aarch64-ios-macabi --emit=staticlib mylib.ae -o catalyst/libmylib.a
```

What was *already* there and needs no work: iOS device/simulator aliases,
`xcrun`-resolved SDKs, `LC_BUILD_VERSION` minos stamping, and `AETHER_IOS_MIN`.
The `ar rcs libaether.a` step also already existed inside the cross build — it
was just discarded into a temp dir instead of being an addressable output.

Docs: `docs/cross-ios.md` now leads with the static library and covers Catalyst;
`docs/build-system.md` has a Catalyst row.

### 2. PCRE2 defines are load-bearing — **now fails loudly**

Fixed, and it immediately paid for itself. `std.regex` no longer compiles its
no-op stubs silently: a build without `AETHER_HAS_PCRE2` is now a **compile
error** naming the exact flags. A build that genuinely wants stubs asks by name
with `-DAETHER_REGEX_ALLOW_STUB` (`make PCRE2=0` does).

Worth knowing: on its first run this guard found that **`make release` — the
binary `make install` ships — had regex silently stubbed out**, and had for as
long as that target existed. It compiles the stdlib on a hand-rolled `$(CC)`
line that carried none of the capability CFLAGS. `make docs-server` had the same
bug. Both fixed in the same PR. So your rubiks_cube day was not an isolated
incident — it was the same defect class, in our shipped compiler.

---

## Already true — no work needed

### 3. PCRE2 JIT off on iOS — **already off, on every target**

`SUPPORT_JIT` is `/* #undef */` in `std/regex/pcre2/config.h:323`, and
`aether_regex.c` never calls `pcre2_jit_compile` — `pcre2_jit_compile.c`
compiles only to the API's "JIT unavailable" stubs. There is **no W^X regex
path on any platform**, so there's nothing to switch off per-target and no
`mmap(PROT_EXEC)` for App Review to find.

Corollary: your exclusion list doesn't need `pcre2_jit_match.c` /
`pcre2_jit_misc.c` — they're `#include`d by `pcre2_jit_compile.c` and disappear
with JIT off.

### 10. `lrint` prototype mismatch — **I was wrong; fixed in aether PR #1871**

I first said "not ours". That was Linux-blind, and your follow-up is right.

`lrint` genuinely appears nowhere in our tree — but your 30 `vg/` externs make
codegen emit `int64_t lrint(double);`, which collides with libm. I couldn't see
it because on **Linux `int64_t` IS `long`**, so the emitted declaration and
libm's are identical and nothing warns (I checked, including with `<math.h>`
forced into the TU). On **macOS/iOS `int64_t` is `long long`** — a distinct
type, and a hard error. Reproduced by declaring `long long lrint(double)`
against `<math.h>`: `conflicting types for 'lrint'` on both gcc and clang.

You're also right that you can't fix it on your side: `-> long` emits
`int64_t`, `-> int` emits `int`, and C `long` is neither, so no extern spelling
matches libm.

**Option 1, as you suggested** — `std.math` now has
`math.lrint(x: float) -> long`, declared in C against the real `<math.h>`. You
delete 30 externs and the warning goes with them.

One thing worth flagging about your option 3 (`math.round(...) as int`): it is
**not** equivalent. `math.round` rounds half-away-from-zero; `lrint` rounds
half-to-even. `math.lrint` matches `lrint`, so your existing call sites keep
their current behaviour. Had you migrated to `round`, every
`lrint(v * 255.0)` colour channel would have shifted by one at exact halves
(`round(0.5)` = 1, `lrint(0.5)` = 0). The README now tabulates the difference.

---

## Still open — real work, correctly scoped

**4. No W^X anywhere in the runtime.** Mostly verification. The only
W^X-adjacent file is `runtime/libaether_sandbox_preload.c`, a `DYLD_INSERT_LIBRARIES`
shim already excluded from iOS static links. No trampolines in the
scheduler/actors. Wants a positive audit, not a fix.

**5. Required-reason API list.** Smaller than feared. The actual inventory:
- `stat` / `lstat` — file timestamps → category **C617.1** (`std/fs/aether_fs.c`)
- `statvfs` — disk space → **E174.1** (`std/fs/aether_fs.c`, #1117)

Note `clock_gettime` is **not** a required-reason API, and
`sysctlbyname("hw.perflevel0.physicalcpu")` (`runtime/utils/aether_cpu_detect.c`)
is a core-count probe, also not required-reason. No `NSUserDefaults` in
libaether at all. I can publish this as a proper per-subsystem doc next.

**6. Export-compliance for bundled crypto.** This one needs a real answer and
it is **not** "HTTPS-exempt". `std/cryptography/` ships AES, ChaCha20-Poly1305,
DES3, SM4, RSA, Ed25519/Ed448, ML-KEM, the full TLS 1.3 stack, and ~40 more
modules. An app linking libaether generally cannot claim the HTTPS-only
exemption. Needs a written statement app authors can paste against
`ITSAppUsesNonExemptEncryption`; I can draft it.

**7. No listening sockets in release.** Not started.

**8. `aeb` iOS packaging (`ui-ios` target).** Not started. `--emit=staticlib`
is the piece it needs to build on, so it's unblocked now.

**9. Async file-picker ABI.** Not started. Agreed it's better on every platform.

---

## Caveat

I work on Linux, so I could not run the Apple toolchain myself. The macOS CI
legs did, and they exercised the real paths: Catalyst compiles, resolves
through `xcrun --sdk macosx`, produces an arm64 Mach-O, and stamps platform
`MACCATALYST`. They also caught a genuine bug in my first cut — I had applied
the 13.1 floor to both arches, and CI reported the arm64 object stamped 14.0.
That is fixed and is the arch-split described above.

What I verified locally, end to end: the static archive builds, contains the
program object plus 95 runtime objects, **links into a C program, and runs** —
on `x86_64-linux-musl`, so the result actually executes on the build host. A
well-formed `.a` that nothing can link is exactly the failure that assertion
exists to prevent.

Still not proven by anyone: an iOS **device** build linked into a real `.app`
and launched, and a Catalyst `--emit=staticlib` inside your Xcode project.
Those are yours to hit first. If either misbehaves, `tests/integration/cross_ios`
is where the assertions live, and I'd want the failure output.
