#!/usr/bin/env python3
"""待ち IPL (driver/bootwait1.S + bootwait.S) を Unicorn の 68000 で動かし、Pico 側 (src/bootwait.c をそのまま
ホスト向けに組んだ libbw) と組み合わせて試す: make -C test/bootwait waiter

IOCS (trap #15) は ここで真似る: B_BITSNS (F1)、B_PRINT (画面の記録)、ONTIME (偽の時計)、SCSIDRV S_READEXT。
SCSI の読み込みは Z の Linux と同じく LBA ごとにキャッシュし、同じ LBA は二度と Pico に聞かない。
"""
import ctypes, os, shutil, struct, sys, tempfile
from unicorn import Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UcError
from unicorn.m68k_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, 'libbw.dylib')
BW_BAND, BW_BAND_END, BW_STRIDE, BW_STAGE2 = 0x40000, 0x100000, 64, 0x80
FLASH_SIZE = 4096 + 32 * 512
PATH = b'//raspi3/x68000z/x68kcli.hds'
SIZE = 104890368
IP = (192 << 24) | (168 << 16) | (1 << 8) | 118
READFN = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8))
D = [UC_M68K_REG_D0 + i for i in range(8)]
A = [UC_M68K_REG_A0 + i for i in range(8)]

fails = 0


def check(cond, what):
    global fails
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        fails += 1


def fake_ipl(marker):
    """本物の IPL の代わり: d7 = marker にして IOCS 0xff (試験の終わり) を呼ぶ"""
    code = struct.pack('>HHH', 0x7e00 | marker, 0x70ff, 0x4e4f)   # moveq #marker,d7 / moveq #-1,d0 / trap #15
    return code + bytes(1024 - len(code))


def hds_image(variant=0):
    img = bytearray(32 * 512)
    img[0:8] = b'X68SCSI1'
    img[0x400:0x800] = fake_ipl(0x55)
    img[0x800:0x804] = b'X68K'
    img[0x810:0x818] = b'Human68k'
    for i in range(0xc00, len(img)):
        img[i] = (i * 7 + 3) & 0xff                 # SCSI ドライバの代わり
    if variant:
        img[0x818] = variant                        # 分割表が変わった
    return bytes(img)


class Pico:
    """libbw を 1 回の「起動」ごとに読み込み直す (static な状態を消すため)。フラッシュは持ち越す"""

    def __init__(self, flash=None):
        self.dir = tempfile.mkdtemp()
        p = os.path.join(self.dir, 'libbw.dylib')
        shutil.copy(LIB, p)
        self.lib = ctypes.CDLL(p)
        self.lib.fake_flash_init()
        if flash is not None:
            self.lib.fake_flash_set(flash)
        self.lib.bw_usable.restype = ctypes.c_bool
        self.lib.bw_state.restype = ctypes.c_char_p
        self.hds_open = 0
        self.sysstat = 0
        self.image = None
        self._rd = READFN(self._read)

    def _read(self, ctx, lba, buf):
        ctypes.memmove(buf, self.image[lba * 512:(lba + 1) * 512], 512)
        return 0

    def flash(self):
        b = ctypes.create_string_buffer(FLASH_SIZE)
        self.lib.fake_flash_get(b)
        return b.raw

    def check(self, image, size=SIZE, path=PATH):
        self.image = image
        self.lib.bw_check(path, ctypes.c_uint64(size), self._rd, None)

    def disk(self, lba):
        b = (ctypes.c_uint8 * 512)()
        rc = self.lib.bw_disk_read(lba, b)
        return bytes(b), rc

    def id6(self, lba, uptime):
        b = (ctypes.c_uint8 * 512)()
        if BW_STAGE2 <= lba < BW_STAGE2 + 8:
            self.lib.bw_stage2_read(lba, b)
        elif BW_BAND <= lba < BW_BAND_END:
            self.lib.bw_reply(lba, b, self.hds_open, self.sysstat, IP if self.sysstat >= 2 else 0, uptime,
                              self._rd, None)
        return bytes(b)

    def state(self):
        return self.lib.bw_state(PATH).decode()


class Z:
    """X68000 Z の側: 68000 + ROM の真似 + Linux の読み込みキャッシュ"""

    def __init__(self, pico, on_poll=None, f1_at=None, noreply=0, clock=1234500):
        self.pico = pico
        self.on_poll = on_poll or (lambda z, n: None)
        self.f1_at = f1_at
        self.noreply = noreply
        self.clock = clock                          # ONTIME (1/100 秒)
        self.mono = 0                               # Pico の起動からの時間 (戻らない)
        self.cache = {}
        self.polls = []
        self.poll_clock = []
        self.screen = []
        self.result = None
        u = self.uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        u.ctl_set_cpu_model(UC_CPU_M68K_M68000)
        u.mem_map(0, 0x100000)
        u.hook_add(UC_HOOK_CODE, self._hook)
        self.trap = False

    def _hook(self, uc, addr, size, data):
        if uc.mem_read(addr, 2) == b'\x4e\x4f':
            self.trap = True
            uc.emu_stop()

    def read(self, scsi_id, lba):
        key = (scsi_id, lba)
        if key not in self.cache:                   # Linux のページキャッシュ
            if scsi_id == 0:
                self.cache[key] = self.pico.disk(lba)[0]
            else:
                self.cache[key] = self.pico.id6(lba, self.mono * 10)
        return self.cache[key]

    def iocs(self):
        u = self.uc
        d = [u.reg_read(r) for r in D]
        a = [u.reg_read(r) for r in A]
        fn = d[0] & 0xff
        if fn == 0x04:                              # B_BITSNS
            v = 0x08 if (self.f1_at is not None and len(self.polls) >= self.f1_at and d[1] == 0x0c) else 0
            u.reg_write(D[0], v)
        elif fn == 0x21:                            # B_PRINT
            s = bytearray()
            p = a[1]
            while True:
                c = u.mem_read(p, 1)[0]
                if c == 0:
                    break
                s.append(c)
                p += 1
            self.screen.append(bytes(s).decode('cp932', 'replace'))
        elif fn == 0x7f:                            # ONTIME
            self.clock += 2
            self.mono += 2
            u.reg_write(D[0], self.clock % 8640000)
            u.reg_write(D[1], 0)
        elif fn == 0xf5 and (d[1] & 0xff) == 0x26:  # SCSIDRV S_READEXT
            lba, n, sid, buf = d[2], d[3], d[4] & 7, a[1]
            if sid == 6 and BW_BAND <= lba < BW_BAND_END:
                self.polls.append(lba)
                self.poll_clock.append(self.clock)
                self.on_poll(self, len(self.polls))
                if self.noreply > 0:
                    self.noreply -= 1
                    u.reg_write(D[0], 0xffffffff)
                    return
            if sid == 6 and lba == 0x100:           # 設定メニュー: "HU" の見出し + 印を付けて終わる小片
                hdr = struct.pack('>HHIIIII', 0x4855, 0, 0x6800, 0x6800, 6, 0, 0) + bytes(0x40 - 24)
                body = struct.pack('>HHH', 0x7e77, 0x70ff, 0x4e4f)
                u.mem_write(buf, (hdr + body).ljust(512, b'\0'))
            else:
                for i in range(n):
                    u.mem_write(buf + i * 512, self.read(sid, lba + i))
            u.reg_write(D[0], 0)
        elif fn == 0xff:                            # 試験の終わり (偽の IPL / 設定メニュー)
            self.result = ('jumped', u.reg_read(UC_M68K_REG_PC), d[7] & 0xff, d[4])
        else:
            raise RuntimeError('IOCS %#x' % fn)

    def boot(self, max_traps=20000):
        """ROM の真似: ID 0 の LBA 2 (1KB 読む ROM もあるので 3 も) を $2000 に置き、d4 = 0 で飛ぶ"""
        u = self.uc
        u.mem_write(0x2000, self.read(0, 2) + self.read(0, 3))
        u.reg_write(UC_M68K_REG_SR, 0x2700)
        u.reg_write(UC_M68K_REG_A7, 0x1800)
        for r in D:
            u.reg_write(r, 0x11110000)
        u.reg_write(D[4], 0)
        pc = 0x2000
        idle = 0
        for _ in range(max_traps):
            self.trap = False
            try:
                u.emu_start(pc, 0xfffff, count=20000)
            except UcError as e:
                self.result = ('error', u.reg_read(UC_M68K_REG_PC), str(e))
                return self.result
            pc = u.reg_read(UC_M68K_REG_PC)
            if not self.trap:
                idle += 1
                if idle > 3:                        # IOCS を呼ばずに回り続けている = 止まった
                    self.result = ('halted', pc)
                    return self.result
                continue
            idle = 0
            self.iocs()
            if self.result:
                return self.result
            pc += 2
        self.result = ('timeout', pc)
        return self.result

    def text(self):
        return '\n'.join(self.screen)


def first_boot_flash():
    """1 回目の起動: HDS が先に開けた (控えを書く)"""
    p = Pico()
    check(not p.lib.bw_usable(PATH), 'no snapshot in an erased flash')
    p.check(hds_image())
    check(p.state() == 'updated', 'first open writes the snapshot (%s)' % p.state())
    fl = p.flash()
    check(fl[4096:] == hds_image(), 'snapshot holds HDS LBA 0-31 as is')
    p2 = Pico(fl)
    check(p2.lib.bw_usable(PATH), 'snapshot is usable after a reboot')
    check(not p2.lib.bw_usable(b'//raspi3/other.hds'), 'snapshot is not used for another HDS path')
    bad = bytearray(fl)
    bad[4096 + 100] ^= 1
    check(not Pico(bytes(bad)).lib.bw_usable(PATH), 'corrupted snapshot (hash) is not used')
    p2.check(hds_image())
    check(p2.state() == 'ok', 'same HDS: no rewrite (%s)' % p2.state())
    p3 = Pico(fl)
    p3.check(hds_image(), size=SIZE + 1024)
    check(p3.state() == 'updated', 'different size: rewritten (%s)' % p3.state())
    return fl


def scenario_wait_then_ready(fl):
    p = Pico(fl)
    steps = {3: 1, 6: 2, 9: 3}

    def on_poll(z, n):
        if n in steps:
            p.sysstat = steps[n]
        if n == 12:
            p.check(hds_image())                    # SMB がつながり HDS を開けた
            p.sysstat = 5
            p.hds_open = 1

    z = Z(p, on_poll)
    r = z.boot()
    check(z.read(0, 0)[:8] == b'X68SCSI1' and z.read(0, 4)[:4] == b'X68K', 'served LBA 0/4 from the snapshot')
    check(z.read(0, 2)[:2] == b'\x60\x00', 'LBA 2 is the waiter (bra.w)')
    check(r[0] == 'jumped' and r[1] == 0x2004 and r[2] == 0x55, 'chain-loaded the real IPL at $2000 (%r)' % (r,))
    check(r[0] == 'jumped' and r[3] == 0, 'd4 (SCSI ID from the ROM) restored to 0')
    t = z.text()
    check('Pico: SMB 接続待ち' in t, 'title shown')
    for s in ('Wi-Fi 未接続', 'Wi-Fi 接続中', 'SMB 接続待ち', 'SMB 接続中'):
        check(s in t, 'status "%s" shown' % s)
    check('IP 192.168.1.118' in t, 'IP shown once Wi-Fi is up')
    check(' 秒 ' in t and any(c in t for c in '|/-\\'), 'elapsed seconds and spinner shown')
    check(len(z.polls) >= 12 and len(set(z.polls)) == len(z.polls), 'every poll at a fresh LBA (%d polls)' % len(z.polls))
    check(all(BW_BAND <= l < BW_BAND_END and (l - BW_BAND) % BW_STRIDE == 0 for l in z.polls), 'polls in the band, 64-sector aligned')
    gaps = [b - a for a, b in zip(z.polls, z.polls[1:])]
    check(all(g >= BW_STRIDE or g < 0 for g in gaps), 'consecutive polls at least 32KB apart')
    check(p.state() == 'served', 'status snap=served (%s)' % p.state())
    return z


def scenario_changed(fl):
    p = Pico(fl)

    def on_poll(z, n):
        if n == 4:
            p.check(hds_image(variant=0x42))       # 同じパスに別の HDS
            p.sysstat = 5
            p.hds_open = 1

    z = Z(p, on_poll)
    r = z.boot()
    check(r[0] == 'halted', 'changed HDS: the waiter stops (%r)' % (r,))
    check('HDS が変わりました' in z.text(), 'changed message shown')
    check(p.state() == 'changed', 'status snap=changed (%s)' % p.state())
    check(p.flash()[4096:] == hds_image(variant=0x42), 'snapshot updated for the next power-on')


def scenario_noreply(fl):
    p = Pico(fl)

    def on_poll(z, n):
        if n == 6:
            p.check(hds_image())
            p.sysstat = 5
            p.hds_open = 1

    z = Z(p, on_poll, noreply=3)
    r = z.boot()
    check('Pico の応答がありません' in z.text(), 'no-reply message on SCSI errors')
    check(r[0] == 'jumped' and r[2] == 0x55, 'recovers and boots after errors (%r)' % (r,))


def scenario_stale(fl):
    """前の待ちの答えがキャッシュに残っている番地を読んだら (時計が 1 日戻ったことにする)、古いものとして飛ばす"""
    p = Pico(fl)
    z = Z(p, clock=1234500)
    state = {'phase': 0}

    def on_poll(zz, n):
        if n == 5 and state['phase'] == 0:
            state['phase'] = 1
            zz.clock = zz.poll_clock[1] - 2         # 次は 2 回目と同じ番地 (キャッシュの「待ち」) を読む
        elif n == 6:
            p.check(hds_image())                    # その間に開けた: キャッシュの古い答えを信じずに次で起動する
            p.sysstat = 5
            p.hds_open = 1

    z.on_poll = on_poll
    r = z.boot()
    check(r[0] == 'jumped' and r[2] == 0x55, 'stale cached "waiting" pages are skipped, then boots (%r)' % (r,))
    check(len(z.polls) != len(set(z.polls)), 'the test really re-read cached LBAs')


def scenario_ready_at_once(fl):
    """待って起動したあとのソフトリセット: キャッシュの LBA 2 は待ち IPL のまま、Pico はもう開けている"""
    p = Pico(fl)
    p.disk(0)                                       # 控えを返した (served)
    p.check(hds_image())
    p.sysstat = 5
    p.hds_open = 1
    z = Z(p)
    r = z.boot()
    check(r[0] == 'jumped' and r[2] == 0x55 and len(z.polls) == 1, 'already open: chain-loads on the first poll (%r, %d polls)' % (r, len(z.polls)))


def scenario_f1(fl):
    p = Pico(fl)
    z = Z(p, f1_at=2)
    r = z.boot()
    check(r[0] == 'jumped' and r[2] == 0x77, 'F1 loads the setting menu from ID 6 (%r)' % (r,))


def scenario_nohds(fl):
    p = Pico(fl)
    p.sysstat = 5                                   # つながったが HDS が開けない

    def on_poll(z, n):
        if n == 5:
            z.f1_at = 0                             # 抜け出すために F1

    z = Z(p, on_poll)
    z.boot()
    check('HDS を開けません' in z.text(), 'HDS-not-found message')


def main():
    fl = first_boot_flash()
    scenario_wait_then_ready(fl)
    scenario_changed(fl)
    scenario_noreply(fl)
    scenario_stale(fl)
    scenario_ready_at_once(fl)
    scenario_f1(fl)
    scenario_nohds(fl)
    print('FAILED %d' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
