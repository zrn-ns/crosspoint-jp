"""
PlatformIO post-build check: the verifyRollbackLater() override must win the link.

src/OtaRollback.cpp overrides a weak symbol that the Arduino core defines in C
(cores/esp32/esp32-hal-misc.c) and declares in no header. Drop the `extern "C"`,
move the definition somewhere --gc-sections can discard, or let the file fall out
of the build, and the link still succeeds — but initArduino() goes back to
cancelling the rollback at boot, which is the exact bug src/OtaRollback.cpp
exists to fix. Nothing about that failure is visible short of bricking a device
with a bad OTA and watching it not recover.

So check the linked ELF: the symbol must resolve to a strong definition ('T'),
not to the core's weak fallback ('W').

Two things make this survive a clean build. With custom_sdkconfig, PlatformIO
links firmware.elf twice — once to bootstrap ESP-IDF, before src/ is compiled at
all, and once for real. The check therefore hangs off firmware.bin, which is
only produced from the final link, and additionally refuses to judge an ELF that
does not contain setup() (i.e. one the sketch was never linked into).
"""

import subprocess
import sys

SYMBOL = 'verifyRollbackLater'
# From src/main.cpp, in a different translation unit than the override. Present
# in the real firmware, absent from the bootstrap link (src/ is compiled after
# it). setup()/loop() are no use here: they get inlined into loopTask.
# Matched as a substring so the C++ mangling (name length prefix, signature)
# does not have to be spelled out here.
APP_MARKER = 'setupDisplayAndFonts'
# Mangled fragment of the ota_rollback namespace. Used only to tell a genuine
# bootstrap link apart from a stale APP_MARKER.
MODULE_MARKER = '12ota_rollback'


def fail(msg):
    print(f'ERROR [check_rollback_hook.py]: {msg}', file=sys.stderr)
    sys.exit(1)


def find_nm(env):
    # The toolchain prefix comes from the compiler PlatformIO resolved for this
    # board, so this keeps working if the target architecture ever changes.
    cc = env.subst('$CC')
    for suffix in ('-gcc', '-clang', '-cc'):
        if cc.endswith(suffix):
            return cc[: -len(suffix)] + '-nm'
    return None


def check_rollback_hook(source, target, env):
    elf = env.subst('$BUILD_DIR/${PROGNAME}.elf')
    nm = find_nm(env)
    if not nm:
        fail(f'could not derive nm from CC={env.subst("$CC")!r}')

    try:
        out = subprocess.check_output([nm, elf], text=True, stderr=subprocess.PIPE)
    except FileNotFoundError:
        fail(f'{nm} not found; cannot verify the {SYMBOL}() override')
    except subprocess.CalledProcessError as e:
        fail(f'nm failed on {elf}: {e.stderr.strip()}')

    symbols = [line.split() for line in out.splitlines() if line.split()]

    names = [parts[-1] for parts in symbols]

    # The bootstrap link contains framework code only. Judging it would fail
    # every clean build, because the override cannot be there yet.
    if not any(APP_MARKER in name for name in names):
        # ...but do not take that on faith. If the module's own symbols are
        # here, this is the real firmware and APP_MARKER simply went stale —
        # say so rather than skipping the check for good.
        if any(MODULE_MARKER in name for name in names):
            fail(
                f'{APP_MARKER} is missing from {elf} but {MODULE_MARKER} symbols are '
                f'present, so this IS the application link. Update APP_MARKER in '
                f'this script to a symbol src/main.cpp still exports.'
            )
        print(f'OTA rollback hook: {elf} is the ESP-IDF bootstrap link, skipping')
        return

    matches = [parts for parts in symbols if parts[-1] == SYMBOL]
    if not matches:
        fail(
            f'{SYMBOL} not present in {elf}. The OTA rollback deferral is not '
            f'linked in — automatic recovery from an unbootable firmware is off.'
        )

    # nm prints "<addr> <type> <name>"; an undefined symbol has no address.
    types = {parts[-2] for parts in matches}
    if 'T' not in types and 't' not in types:
        fail(
            f'{SYMBOL} resolved as {sorted(types)}, not a strong definition. '
            f'The Arduino core\'s weak fallback won the link, so initArduino() '
            f'still cancels the rollback at boot. Check that '
            f'src/OtaRollback.cpp defines it inside `extern "C"`.'
        )

    print(f'OTA rollback hook: {SYMBOL} overridden by the project (ok)')


# PlatformIO/SCons entry point.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
    # Hooked on the .bin, not the .elf: with custom_sdkconfig the ELF is linked
    # once before src/ is even compiled, and that bootstrap link has no override
    # in it. The .bin only ever comes from the final link.
    env.AddPostAction(  # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.bin',
        # VerboseAction keeps SCons from echoing the whole object-file list.
        env.VerboseAction(  # noqa: F821  # type: ignore[name-defined]
            check_rollback_hook, 'Checking the OTA rollback hook'
        ),
    )
except NameError:
    print('check_rollback_hook.py is a PlatformIO post-build script; nothing to do.')
