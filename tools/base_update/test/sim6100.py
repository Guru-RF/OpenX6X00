#!/usr/bin/env python3
"""A simulated X6100 base for base_flash, on this computer: base_flash.c built with its port on a pseudo-terminal, its
GPIO lines as a log (GPIO_LOG) and its base's I2C answer made up (SIM_BASE_VERSION), run as on the radio, against a
fake base that streams like its application until its key line is low while its reset is let go, then greets as the
bootloader does ('A'), answers 'r' and 'p', asks for XMODEM with 'C's and takes the packets, or fails in one way.
tools/base_update/test/run.sh runs them all.

    sim6100.py CC SRC STUB_DIR WORK_DIR XGF MODE [TAG]

Prints one line, "PASS ..." or "FAIL ...", and exits 0 or 1.

SPDX-License-Identifier: LGPL-2.1-or-later
"""
import binascii
import os
import pty
import select
import subprocess
import sys
import time
import tty

# mode: (exit status, a line base_flash must log, the bytes received == the file, the GPIO log must end with the
# application started again (key line up, reset pulsed), extra arguments)
MODES = {
    'ok':       (0, '1k xmodem send finished.', True, False, []),
    'zeros':    (0, '1k xmodem send finished.', True, False, []),        # 0 bytes before the answers: passed over
    'dry':      (0, 'dry run done', False, True, ['--dry-run']),
    'nogreet':  (2, '[ERR] Wait IAP respond timeout', False, True, []),
    'noerase':  (3, "[ERR] IAP failed, can't erase flash", False, False, []),
    'noprog':   (3, "[ERR] IAP failed, can't start xmodem", False, False, []),
    'nostart':  (3, '1k xmodem timeout.', False, False, []),
    'nak':      (3, '1k xmodem bad packet, pn= 100', False, False, []),
    'eot':      (3, '1k xmodem error, = 0 (after EOT)', True, False, []),
    'x6200dt':  (1, 'refused: nothing was sent to the base', False, False, []),   # an X6100 file on an X6200
}

cc, src, stub, work, xgf_path, mode = sys.argv[1:7]
tag = sys.argv[7] if len(sys.argv) > 7 else mode
want_rc, want_line, want_data, want_restart, extra = MODES[mode]
xgf = open(xgf_path, 'rb').read()
m, s = pty.openpty()
tty.setraw(s)
binp = os.path.join(work, 'sim6100_' + tag)
logp = os.path.join(work, 'sim6100_' + tag + '.log')
gpiop = os.path.join(work, 'gpio6100_' + tag + '.log')
chipp = os.path.join(work, 'gpiochip6100_' + tag)
dtp = os.path.join(work, 'dt6100_' + tag)
i2cp = os.path.join(work, 'i2c6100_' + tag)
with open(dtp, 'wb') as f:
    f.write((b'xiegu,x6200' if mode == 'x6200dt' else b'xiegu,x6100') + b'\0allwinner,sun8i-r16\0')
for p in (gpiop,):
    if os.path.exists(p):
        os.remove(p)
open(chipp, 'wb').close()
open(i2cp, 'wb').close()
subprocess.run([cc, '-O1', '-w', '-I', stub, '-DUART="%s"' % os.ttyname(s), '-DI2C_BUS="%s"' % i2cp, '-DBACK_WAIT_S=2',
                '-DDT_COMPAT="%s"' % dtp, '-DGPIO_CHIP="%s"' % chipp, '-DGPIO_LOG="%s"' % gpiop, '-DGREET_TRIES=3',
                '-DERASE_MS=1500', '-DSIM_BASE_VERSION="V1.1.8 Sep 15 2024,15:22:37"', '-o', binp, src], check=True)
log = open(logp, 'w')
p = subprocess.Popen([binp, '--flash', xgf_path, '--yes', '--test-ok', '--held-by', str(os.getpid())] + extra,
                     stdout=log, stderr=subprocess.STDOUT)


def rd(n, t):
    b = b''
    end = time.time() + t
    while len(b) < n and time.time() < end:
        r, _, _ = select.select([m], [], [], 0.02)
        if r:
            try:
                b += os.read(m, n - len(b))
            except OSError:
                break
    return b


def gpio():
    try:
        return open(gpiop).read().split('\n')
    except OSError:
        return []


def in_boot():
    """The key line low while the reset was let go: the base came out of reset into its bootloader"""
    g, strap, reset = gpio(), None, None
    for line in g:
        if line == '203=0':
            strap = 0
        elif line == '203=1':
            strap = 1
        elif line == '204=1':
            reset = 1
        elif line == '204=0':
            if reset == 1 and strap == 0:
                return True
            reset = 0
    return False


t0 = time.time()
state, got, seq, sent = 'app', bytearray(), 1, b''
while p.poll() is None and time.time() - t0 < 60:
    if mode == 'x6200dt':
        sent += rd(4096, 0.05)
        continue
    if state == 'app':
        try:
            os.write(m, b'\x55' * 32)           # the application's stream, until the base is reset into its bootloader
        except OSError:
            pass
        sent += rd(4096, 0.01)
        if in_boot():
            state = 'greet'
        continue
    if state == 'greet':
        time.sleep(0.05)
        if mode != 'nogreet':
            os.write(m, b'A')
        state = 'boot'
        continue
    b = rd(1, 0.2)
    if not b:
        continue
    if state == 'boot':
        if b == b'r' and mode != 'noerase':
            time.sleep(0.5)                     # an erase takes a while
            os.write(m, (b'\x00\x00' if mode == 'zeros' else b'') + b'g')
        elif b == b'p' and mode != 'noprog':
            os.write(m, (b'\x00' if mode == 'zeros' else b'') + b'g')
            time.sleep(0.1)
            if mode != 'nostart':
                os.write(m, b'CC')
            state = 'xmodem'
        elif b in (b'r', b'p'):
            pass
        else:
            sent += b
    elif state == 'xmodem':
        if b == b'\x04':
            if mode != 'eot':
                os.write(m, b'\x06')
            continue
        pkt = b + rd(1028, 5)
        ok = len(pkt) == 1029 and pkt[0] == 2 and pkt[1] == seq & 0xff and pkt[2] == (~seq) & 0xff and \
            pkt[1027:] == binascii.crc_hqx(pkt[3:1027], 0).to_bytes(2, 'big')
        if (mode, seq) == ('nak', 100):
            os.write(m, b'\x15')
        else:
            os.write(m, b'\x06' if ok else b'\x15')
            if ok:
                got += pkt[3:1027]
                seq += 1
if p.poll() is None:
    p.kill()
p.wait()
log.close()
same = bytes(got) == xgf + b'\xff' * (-len(xgf) % 1024)
g = [x for x in gpio() if x]
restarted = g[-6:] == ['203=1', '204=1', '204 free', '204=0', '204 free', '203 free']
# The way in, always first when the base was asked at all: key low, reset high, reset low and let go
way_in = g[:4] == ['203=0', '204=1', '204 free', '204=0'] and '203=1' in g and '203 free' in g
logged = want_line in open(logp).read()
good = p.returncode == want_rc and logged and same == want_data and \
    (restarted if want_restart else True) and \
    (not g if mode == 'x6200dt' else way_in) and not (mode == 'x6200dt' and sent)
print('%s %-8s exit %d (want %d), %3d packets, data %s, gpio %s, "%s" %s, %.0f s' % (
    'PASS' if good else 'FAIL', 'x6100-' + tag, p.returncode, want_rc, seq - 1, '== file' if same else '!= file',
    ','.join(g) or '-', want_line, 'logged' if logged else 'NOT logged', time.time() - t0))
sys.exit(0 if good else 1)
