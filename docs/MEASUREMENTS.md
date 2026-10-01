# Measurements on real APKs

What `tools/apkscan.py` reports for real apps, and what it means for the design.

## Qalculate! 5.x (F-Droid `com.jherkenhoff.qalculate`, arm64-v8a build, 15 MB)

1 `classes.dex` (2.9 MB, Kotlin + Jetpack Compose) and 9 arm64-v8a native libraries,
about 3.2 million instructions in total.

| library | instructions | `svc #0` | `mrs tpidr_el0` | x18 shadow stack |
|---|---:|---:|---:|---:|
| libqalculate.so | 1,765,232 | 3 | 0 | 0 |
| libqalculate_swig.so | 362,076 | 2 | 692 | 0 |
| libc++_shared.so | 332,478 | 0 | 0 | 0 |
| libxml2.so | 305,340 | 0 | 0 | 0 |
| libiconv.so | 232,048 | 0 | 0 | 0 |
| libgmp.so | 110,620 | 0 | 0 | 0 |
| libmpfr.so | 109,576 | 0 | 0 | 0 |
| libandroidx.graphics.path.so | 1,808 | 0 | 6 | 0 |
| libdatastore_shared_counter.so | 1,136 | 0 | 1 | 0 |

What it says:

- **Every PT_LOAD is 16 KB aligned** (`min align 0x4000`), matching iOS's page size, so the
  libraries can be mapped as they are. (Google Play has required 16 KB-compatible native code
  since late 2025.)
- **Only 5 direct syscalls** in 3.2 M instructions. Apps reach the kernel through bionic's
  `libc.so`, which is not in the APK: we supply it, so the syscall layer serves our own libc,
  not thousands of sites in app code.
- **tpidr_el0 is read, never written** (699 reads, 0 writes): stack-protector canaries and TLS
  lookups. Only libc sets the thread pointer. In the interpreter this is a guest register and
  costs nothing; the native backend has to redirect these reads.
- **No shadow call stack** in any library, so no x18 rewriting for this app.

So the per-app patching is small. The real work is the platform around it: bionic
(`libc`, `libm`, `libdl`, `liblog`), the dynamic linker, ART with JNI, and — for this app —
Jetpack Compose's rendering stack, which is the largest single piece.

## cube.run (libGDX game, 1 dex of 7 MB, 1 arm64-v8a library)

| library | instructions | `svc #0` | `mrs tpidr_el0` | x18 shadow stack |
|---|---:|---:|---:|---:|
| libgdx.so | 40,260 | 0 | 40 | 0 |

Native code is small: libGDX's JNI glue (buffers, matrices, pixmaps). It imports
only plain libc/libm (`malloc`, `memcpy`, `pow`, `ldexp`, `strtol`,
`__stack_chk_fail` and a few more), and the 40 thread-pointer reads are
stack-protector canaries. The game logic is all in `classes.dex`, so this app
waits on ART and a GLES surface, not on more native-code work.
