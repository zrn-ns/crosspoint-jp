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
"""

import subprocess
import sys

SYMBOL = 'verifyRollbackLater'


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
    elf = str(target[0])
    nm = find_nm(env)
    if not nm:
        fail(f'could not derive nm from CC={env.subst("$CC")!r}')

    try:
        out = subprocess.check_output([nm, elf], text=True, stderr=subprocess.PIPE)
    except FileNotFoundError:
        fail(f'{nm} not found; cannot verify the {SYMBOL}() override')
    except subprocess.CalledProcessError as e:
        fail(f'nm failed on {elf}: {e.stderr.strip()}')

    matches = [
        line.split()
        for line in out.splitlines()
        if line.split()[-1:] == [SYMBOL]
    ]
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
    env.AddPostAction(  # noqa: F821  # type: ignore[name-defined]
        '$BUILD_DIR/${PROGNAME}.elf',
        # VerboseAction keeps SCons from echoing the whole object-file list.
        env.VerboseAction(  # noqa: F821  # type: ignore[name-defined]
            check_rollback_hook, 'Checking the OTA rollback hook'
        ),
    )
except NameError:
    print('check_rollback_hook.py is a PlatformIO post-build script; nothing to do.')
