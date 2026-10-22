#!/usr/bin/env python3
"""待ち IPL (driver/bootwait1.S + bootwait.S) を Unicorn の 68000 で動かし、Pico 側 (src/bootwait.c をそのまま
ホスト向けに組んだ libbw) と組み合わせて試す: make -C test/bootwait waiter

IOCS (trap #15) は ここで真似る: B_BITSNS (F1)、B_KEYSNS / B_KEYINP (S)、B_PRINT (画面の記録)、ONTIME (偽の時計)、
SCSIDRV S_READEXT。
SCSI の読み込みは Z の Linux と同じく LBA ごとにキャッシュし、同じ LBA は二度と Pico に聞かない。
"""
import ctypes, os, re, shutil, struct, sys, tempfile
from unicorn import Uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UcError
from unicorn.m68k_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(HERE, '..', '..', 'driver'))
import mkbootwait as G                                  # 配置と紋章 (bootwait_gen.S と同じものを作る)
ROM_PAL = [0x0000, 0xf83e, 0xffc0, 0xfffe, 0xde6c, 0x4022, 0x0f80, 0xf83f,
           0x0001, 0x003e, 0x07c0, 0x07fe, 0xf800, 0xf83e, 0xffc0, 0xfffe]   # 試験の「元のパレット」 (何でもよい)
TVRAM, TPAL = 0xe00000, 0xe82200
LIB = os.path.join(HERE, 'libbw.dylib')
BW_BAND, BW_BAND_END, BW_STRIDE, BW_STAGE2 = 0x40000, 0x100000, 64, 0x80
PREVIEW = os.environ.get('BW_PREVIEW', os.path.join(HERE, 'waiter_preview.png'))
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
        self.retry = 0xffffffff                     # 次の再接続までの ms
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
        if BW_STAGE2 <= lba < BW_STAGE2 + 32:
            self.lib.bw_stage2_read(lba, b)
        elif BW_BAND <= lba < BW_BAND_END:
            self.lib.bw_reply(lba, b, self.hds_open, self.sysstat, IP if self.sysstat >= 2 else 0, uptime,
                              ctypes.c_uint32(self.retry if callable(self.retry) is False else self.retry()),
                              self._rd, None)
        return bytes(b)

    def state(self):
        return self.lib.bw_state(PATH).decode()


class Screen:
    """IOCS の画面 (96 桁 x 32 行) の真似: ESC[2J、ESC[y;xH、Shift_JIS の全角は 2 桁"""

    def __init__(self):
        self.clear()
        self.attr = 3
        self.cur = 0

    def clear(self):
        self.cell = [[(' ', 3)] * 96 for _ in range(32)]
        self.x = self.y = 0

    def put(self, data):
        i = 0
        while i < len(data):
            c = data[i]
            if c == 0x1b:
                m = re.match(rb'\[(\d*)(?:;(\d*))?([A-Za-z])', data[i + 1:])
                if m.group(3) == b'J':
                    self.clear()
                elif m.group(3) == b'H':
                    self.y, self.x = int(m.group(1)) - 1, int(m.group(2)) - 1
                i += 1 + m.end()
                continue
            if 0x81 <= c <= 0x9f or 0xe0 <= c <= 0xfc:
                ch, w = data[i:i + 2].decode('cp932'), 2
                i += 2
            else:
                ch, w = chr(c), 1
                i += 1
            if self.y < 32 and self.x + w <= 96:
                self.cell[self.y][self.x] = (ch, self.attr)
                if w == 2:
                    self.cell[self.y][self.x + 1] = ('', self.attr)
            self.x += w

    def row(self, r):
        """1 桁 1 文字の文字列 (全角の右半分は '\\0')。桁で切り出せる"""
        return ''.join(ch or '\0' for ch, a in self.cell[r - 1])

    def attr_at(self, r, c):
        return self.cell[r - 1][c - 1][1]


def render(z, path):
    """テキスト (IOCS の画面の真似) と紋章 (テキスト VRAM のプレーン 2・3) をパレットどおりに PNG に"""
    from PIL import Image, ImageDraw, ImageFont
    pal = [struct.unpack('>H', bytes(z.uc.mem_read(TPAL + i * 2, 2)))[0] for i in range(16)]
    rgb = [(((v >> 6) & 31) * 255 // 31, ((v >> 11) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31) for v in pal]
    img = Image.new('RGB', (768, 512), rgb[0])
    px = img.load()
    p2 = bytes(z.uc.mem_read(TVRAM + 0x40000, 512 * 128))
    p3 = bytes(z.uc.mem_read(TVRAM + 0x60000, 512 * 128))
    for y in range(512):
        for xb in range(96):
            a, b = p2[y * 128 + xb], p3[y * 128 + xb]
            if a | b:
                for k in range(8):
                    v = ((a >> (7 - k)) & 1) * 4 + ((b >> (7 - k)) & 1) * 8
                    if v:
                        px[xb * 8 + k, y] = rgb[v]
    d = ImageDraw.Draw(img)
    fa = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 13)
    fj = ImageFont.truetype('/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc', 15)
    for r in range(32):
        for c in range(96):
            ch, at = z.scr.cell[r][c]
            if ch == '':
                continue
            fg = rgb[at & 3]
            wide = len(ch.encode('cp932')) == 2
            if at & 8:
                d.rectangle([c * 8, r * 16, c * 8 + (16 if wide else 8) - 1, r * 16 + 15], fill=fg)
                fg = rgb[0]
            if ch != ' ':
                d.text((c * 8, r * 16 + (0 if wide else 1)), ch, fill=fg, font=fj if wide else fa)
    img.resize((1536, 1152), Image.LANCZOS).save(path)


class Z:
    """X68000 Z の側: 68000 + ROM の真似 + Linux の読み込みキャッシュ"""

    def __init__(self, pico, on_poll=None, f1_at=None, key_at=None, noreply=0, clock=1234500):
        self.pico = pico
        self.on_poll = on_poll or (lambda z, n: None)
        self.f1_at = f1_at
        self.key_at = key_at                        # (何回目の問い合わせのあと, キーコード)
        self.noreply = noreply
        self.clock = clock                          # ONTIME (1/100 秒)
        self.mono = 0                               # Pico の起動からの時間 (戻らない)
        self.step = 2                               # ONTIME 1 回で進む 1/100 秒
        self.cache = {}
        self.polls = []
        self.poll_clock = []
        self.screen = []
        self.result = None
        u = self.uc = Uc(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN)
        u.ctl_set_cpu_model(UC_CPU_M68K_M68000)
        u.mem_map(0, 0x100000)
        u.mem_map(0xe00000, 0x80000)                # テキスト VRAM
        u.mem_map(0xe80000, 0x4000)                 # CRTC とパレット
        u.mem_write(TPAL, struct.pack('>16H', *ROM_PAL))
        self.scr = Screen()
        self.snaps = []
        self.on_snap = None
        u.mem_write(0xe8002a, b'\x01\xf0')         # 設定画面が残した同時アクセスとビットマスクの真似
        u.mem_write(0xe8002e, b'\xff\xff')
        self.dirty_prints = 0
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
        elif fn == 0x01:                            # B_KEYSNS
            k = self.key_at
            u.reg_write(D[0], k[1] if (k and len(self.polls) >= k[0]) else 0)
        elif fn == 0x00:                            # B_KEYINP
            k = self.key_at
            u.reg_write(D[0], k[1] if k else 0)
            self.key_at = None
        elif fn == 0x22:                            # B_COLOR
            u.reg_write(D[0], self.scr.attr)
            self.scr.attr = d[1] & 0xffff
        elif fn in (0x1e, 0x1f):                    # B_CURON / B_CUROFF
            self.scr.cur = fn == 0x1e
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
            self.scr.put(bytes(s))
            if u.mem_read(0xe8002a, 2) != b'\0\0' or u.mem_read(0xe8002e, 2) != b'\0\0':
                self.dirty_prints += 1
        elif fn == 0x7f:                            # ONTIME
            self.clock += self.step
            self.mono += 2
            u.reg_write(D[0], self.clock % 8640000)
            u.reg_write(D[1], 0)
        elif fn == 0xf5 and (d[1] & 0xff) == 0x26:  # SCSIDRV S_READEXT
            lba, n, sid, buf = d[2], d[3], d[4] & 7, a[1]
            if sid == 6 and BW_BAND <= lba < BW_BAND_END:
                self.snaps.append([self.scr.row(r) for r in range(1, 33)])   # 前の回に描いたもの
                if self.on_snap:
                    self.on_snap(self, len(self.polls) + 1)
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
            self.at_jump = self.restored()
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
                if idle > 30:                       # IOCS を呼ばずに 60 万命令 = 止まった (画面の消去は 6 万命令ほど)
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

    def restored(self):
        """本物の IPL / 設定画面に渡すときの画面: プレーン 2・3 が空、パレットが元どおり、カーソル表示、色 3、消去済み"""
        u = self.uc
        p23 = bytes(u.mem_read(TVRAM + 0x40000, 512 * 128)) + bytes(u.mem_read(TVRAM + 0x60000, 512 * 128))
        pal = list(struct.unpack('>16H', bytes(u.mem_read(TPAL, 32))))
        blank = all(self.scr.row(r).strip() == '' for r in range(1, 33))
        return {'planes': not any(p23), 'palette': pal == ROM_PAL, 'cursor': self.scr.cur,
                'color': self.scr.attr == 3, 'blank': blank}

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
    p.retry = 0

    def on_poll(z, n):
        p.retry = {7: 3200, 8: 2700}.get(n, 0)     # 7, 8 回目は「次の再接続まで」
        if n in steps:
            p.sysstat = steps[n]
        if n == 12:
            p.check(hds_image())                    # SMB がつながり HDS を開けた
            p.sysstat = 5
            p.hds_open = 1

    z = Z(p, on_poll)
    z.art_ok = z.pal_ok = z.cur_off = False
    z.bar_attr = 0

    def on_snap(zz, n):
        if n == 8:                                  # 待っている最中の画面を確かめて PNG にする
            W_, H_, a2, a3 = G.art()
            off = (G.ART_ROW - 1) * 16 * 128 + (768 - W_) // 2 // 8
            ok = True
            for y in range(H_):
                for pl, im in ((0x40000, a2), (0x60000, a3)):
                    row = bytes(zz.uc.mem_read(TVRAM + pl + off + y * 128, W_ // 8))
                    ok &= row == im.tobytes()[y * W_ // 8:(y + 1) * W_ // 8]
            zz.art_ok = ok
            pal = struct.unpack('>16H', bytes(zz.uc.mem_read(TPAL, 32)))
            zz.pal_ok = all(pal[i] == G.grb(*c) for i, c in G.PALETTE.items())
            zz.cur_off = not zz.scr.cur
            zz.bar_attr = max(zz.scr.attr_at(G.ROW_BAR, c) for c in range(1, 97))
            render(zz, PREVIEW)
    z.on_snap = on_snap
    r = z.boot()
    check(z.read(0, 0)[:8] == b'X68SCSI1' and z.read(0, 4)[:4] == b'X68K', 'served LBA 0/4 from the snapshot')
    check(z.read(0, 2)[:2] == b'\x60\x00', 'LBA 2 is the waiter (bra.w)')
    check(r[0] == 'jumped' and r[1] == 0x2004 and r[2] == 0x55, 'chain-loaded the real IPL at $2000 (%r)' % (r,))
    check(r[0] == 'jumped' and r[3] == 0, 'd4 (SCSI ID from the ROM) restored to 0')
    t = z.text()
    S = z.snaps
    rows = lambda r: [sn[r - 1] for sn in S]
    T = lambda x: x.replace('\0', '')
    check('\\' not in t, 'no backslash printed (a yen sign on the X68k)')
    check(z.dirty_prints == 0, 'CRTC R21/R23 cleared before every print (%d dirty)' % z.dirty_prints)
    last = S[-1]
    L, W = G.FRAME_LEFT, G.FRAME_WIDTH
    check(last[G.FRAME_TOP - 1][L - 1] == '┏' and last[G.FRAME_TOP - 1][L + W - 3] == '┓' and
          last[G.FRAME_BOTTOM - 1][L - 1] == '┗' and all(last[r - 1][L - 1] == '┃' and last[r - 1][L + W - 3] == '┃'
                                                       for r in range(G.FRAME_TOP + 1, G.FRAME_BOTTOM)), 'frame drawn with box characters')
    check('Pico: SMB 接続待ち' in T(last[G.ROW_TITLE - 1]) and 'x68kzremotedrv' in last[G.ROW_SUB - 1], 'title and subtitle')
    for r, lab in ((G.ROW_WIFI, 'Wi-Fi'), (G.ROW_SMB, 'SMB'), (G.ROW_HDS, 'HDS'), (G.ROW_TIME, '経過')):
        check(T(last[r - 1][G.COL_LABEL - 1:]).startswith(lab), 'label %s aligned at column %d' % (lab, G.COL_LABEL))
    val = lambda r: set(T(x[G.COL_VALUE - 1:G.COL_VALUE - 1 + G.VALUE_W]).rstrip() for x in rows(r))
    check({'○ 未接続', '○ 接続中', '● 接続済み'} <= val(G.ROW_WIFI), 'Wi-Fi values (%s)' % val(G.ROW_WIFI))
    check({'○ 接続待ち', '○ 接続中'} <= val(G.ROW_SMB), 'SMB values (%s)' % val(G.ROW_SMB))
    check('○ 待っています' in val(G.ROW_HDS), 'HDS value (%s)' % val(G.ROW_HDS))
    check(any('IP 192.168.1.118' in x for x in rows(G.ROW_WIFI)), 'IP shown once Wi-Fi is up')
    secs = [int(m.group(1)) for x in rows(G.ROW_TIME) for m in [re.search(r'(\d+) 秒', T(x))] if m]
    check(len(secs) >= 10 and secs == sorted(secs) and secs[-1] > secs[0], 'elapsed seconds count up (%s)' % secs)
    sm = ' '.join(T(x) for x in rows(G.ROW_SMB))
    check('(次の再接続まで 4 秒)' in sm and '(次の再接続まで 3 秒)' in sm and '(接続を試しています)' in sm, 'next-retry countdown and "trying" shown')
    trk = rows(G.ROW_TRACK)
    pos = [x.index('◆') for x in trk if '◆' in x]
    check(all(x.count('◆') == 1 for x in trk[1:]) and len(set(pos)) >= 5 and '[Pico W]' in trk[-1] and '[X68000 Z]' in trk[-1],
          'packet travels between [Pico W] and [X68000 Z] (%d positions)' % len(set(pos)))
    bar = last[G.ROW_BAR - 1]
    check('S: 設定画面' in T(bar) and '電源を入れ直してください' in T(bar) and z.bar_attr & 8, 'bottom bar in reverse video')
    check(z.art_ok, 'emblem bitmap in text VRAM planes 2-3 while waiting')
    check(z.pal_ok, 'waiter palette set while waiting')
    check(z.cur_off, 'text cursor hidden while waiting')
    j = getattr(z, 'at_jump', {})
    check(j and all(j.values()), 'before the real IPL: planes 2-3 clear, palette restored, cursor on, colour 3, screen cleared (%s)' % j)
    print('     preview: %s' % PREVIEW)
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


def scenario_clock_backwards(fl):
    """Z の ONTIME が逆に進んでも 0.5 秒ごとに新しい番地を読み、経過秒は Pico の時計で増える"""
    p = Pico(fl)

    def on_poll(z, n):
        if n == 10:
            p.check(hds_image())
            p.sysstat = 5
            p.hds_open = 1

    z = Z(p, on_poll)
    z.step = -2
    r = z.boot()
    secs = [int(m) for m in re.findall(r'経過 (\d+) 秒', z.text())]
    check(r[0] == 'jumped' and len(set(z.polls)) == len(z.polls) and secs == sorted(secs),
          'backwards ONTIME: fresh polls, seconds still count up (%r, %s)' % (r[0], secs))


def scenario_f1(fl):
    p = Pico(fl)
    z = Z(p, f1_at=2)
    r = z.boot()
    check(r[0] == 'jumped' and r[2] == 0x77, 'F1 loads the setting menu from ID 6 (%r)' % (r,))


def scenario_key_s(fl):
    p = Pico(fl)
    z = Z(p, key_at=(3, 0x1f73))                   # 's' (スキャンコード 0x1f)
    r = z.boot()
    check(r[0] == 'jumped' and r[2] == 0x77 and r[3] == 6, 'S loads the setting UI from ID 6 (%r)' % (r,))
    j = getattr(z, 'at_jump', {})
    check(j and all(j.values()), 'before the setting UI: screen restored (%s)' % j)


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
    scenario_clock_backwards(fl)
    scenario_f1(fl)
    scenario_key_s(fl)
    scenario_nohds(fl)
    print('FAILED %d' % fails if fails else 'all passed')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
