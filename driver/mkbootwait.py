#!/usr/bin/env python3
"""bootwait.S (待ち IPL の後半) が使う文字列と紋章のビットマップを bootwait_gen.S に書き出す。
出力は git に入れる (ビルドに PIL を要らないように)。直したら: python3 mkbootwait.py [-o bootwait_gen.S] [--png art.png]

ロゴは "Pico Z Loader" (オリジナル): 電源の印の輪と Z の紋章、"PICO Z" と字間を空けた "LOADER" (Orbitron)。
テキスト VRAM に直接描く。紋章の行には IOCS の字を出さないので、プレーン 0・1 も使える:
  プレーン 0+1 = 色 3 (白: "PICO ")、プレーン 2 = 色 4 (LIGHT: Z・軸・"Z")、プレーン 2+3 = 色 12 (MID: 輪)、
  プレーン 3 = 色 8 (DARK: "LOADER")
Orbitron (SIL OFL 1.1、https://github.com/theleagueof/orbitron) は無ければ fonts/ に取ってくる (git には入れない)。
文字列は UTF-8 で書き、Shift_JIS (cp932) にする。全角は 2 バイト = 2 桁なので、バイト数で桁をそろえられる。
'\\' (0x5C) は X68k では円記号になるので使わない。
"""
import argparse, os, urllib.request
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- 画面の配置 (1 始まりの行・桁。テキスト画面は 96 桁 x 32 行、1 文字 8x16 ドット) ----
FRAME_TOP, FRAME_BOTTOM, FRAME_LEFT, FRAME_WIDTH = 2, 28, 3, 92   # 3-94 桁
ART_ROW = 4                     # ロゴの上端の行 (ドットでは (ART_ROW-1)*16)。ロゴは 8 行 (128 ドット): 4-11 行に IOCS の字は出さない
ASPECT = 1.125                  # X68k の 768x512 を 4:3 に映すと 1 ドットは横の 1.125 倍の高さ: 絵は縦を 1/ASPECT にしておく
ROW_SUB, ROW_TITLE = 13, 15
ROW_WIFI, ROW_SMB, ROW_HDS, ROW_TIME = 17, 18, 19, 20
ROW_TRACK, ROW_MSG, ROW_BAR = 23, 25, 30      # 枠の中 (3-27 行) で上 1 行・下 2 行の余白
# 状態欄は「見出し 6 + 2、値 16、付け足し 22」の塊 (幅 46) を画面の中央に置く。付け足しの最長は
# "(次の再接続まで 99 秒)" / "IP 255.255.255.255"。枠の内側 (5-92 桁) をはみ出さない
VALUE_W, EXTRA_W = 16, 22
BLOCK_W = 8 + VALUE_W + EXTRA_W
COL_LABEL = (96 - BLOCK_W) // 2 + 1
COL_VALUE, COL_EXTRA = COL_LABEL + 8, COL_LABEL + 8 + VALUE_W
MSG_W = 66                      # 知らせは状態欄の左にそろえ、枠の手前まで
# ◆ の行: "[Pico W]" 8 + 空き 2 + 線 (全角 20 = 40 桁) + 空き 2 + "[X68000 Z]" 10 = 62 桁を中央に
TRACK_LEN = 20
TRACK_ROW_W = 8 + 2 + TRACK_LEN * 2 + 2 + 10
TRACK_COL = (96 - TRACK_ROW_W) // 2 + 1 + 10    # 線の始まり

# 文字の色 (IOCS _B_COLOR: 0-3 が色、+8 で反転)。テキストパレット 1-3 を入れ替える (青系)
#   C_DIM 深い青 (枠・線)、C_OK 水色 (つながった・副題・◆)、C_TEXT 白 (見出し・待ち)、C_WARN 白の反転 (知らせ)、C_BAR 水色の反転
C_DIM, C_OK, C_TEXT = 1, 2, 3
C_WARN, C_BAR = C_TEXT + 8, C_OK + 8
# テキストパレット (GRB 各 5 ビット + I)。待ち IPL が始めに控えて書き、本物の IPL に飛ぶ前に戻す
PALETTE = {                     # (R, G, B) 各 0-31
    1: (2, 7, 22),              # 深い青 (枠・線)
    2: (12, 24, 31),            # 水色 (つながった ●・副題・◆)
    3: (31, 31, 31),            # 白 (IOCS の字と "PICO ")
    4: (14, 26, 31),            # LIGHT: Z・電源の軸・"Z"
    8: (13, 16, 21),            # DARK: "LOADER" (#6f86a8 ぐらい。黒に対して 4.5:1 以上)
    12: (5, 13, 30),            # MID: 電源の輪。待っている間は PULSE と交互に光る
}
PULSE = (16, 24, 31)            # 輪が明るくなるとき
FONT_URL = 'https://github.com/google/fonts/raw/main/ofl/orbitron/Orbitron%5Bwght%5D.ttf'
FONT = os.path.join(HERE, 'fonts', 'Orbitron.ttf')


def grb(r, g, b):
    return (g << 11) | (r << 6) | (b << 1)


def sj(s):
    assert '\\' not in s, s                  # 2 バイト文字の 2 バイト目の 0x5C は構わない
    return s.encode('cp932')


def esc(row, col):
    return b'\x1b[%d;%dH' % (row, col)


def pad(s, width):
    b = sj(s)
    assert len(b) <= width, (s, len(b), width)
    return b + b' ' * (width - len(b))


def strings():
    t = {}
    l, w = FRAME_LEFT, FRAME_WIDTH
    inner = (w - 4) // 2                                    # ━ の数
    fr = esc(FRAME_TOP, l) + sj('┏' + '━' * inner + '┓')
    for r in range(FRAME_TOP + 1, FRAME_BOTTOM):
        fr += esc(r, l) + sj('┃') + esc(r, l + w - 2) + sj('┃')
    fr += esc(FRAME_BOTTOM, l) + sj('┗' + '━' * inner + '┛')
    t['s_cls'] = b'\x1b[2J'
    t['s_frame'] = fr

    def center(row, text):
        b = sj(text)
        return esc(row, (96 - len(b)) // 2 + 1) + b

    t['s_sub'] = center(ROW_SUB, 'for X68000 Z   ・   Raspberry Pi Pico W / x68kzremotedrv')
    t['s_title'] = center(ROW_TITLE, 'SMB 接続待ち…   (Wi-Fi / SMB を確認してください)')
    t['s_labels'] = (esc(ROW_WIFI, COL_LABEL) + sj('Wi-Fi') + esc(ROW_SMB, COL_LABEL) + sj('SMB') +
                     esc(ROW_HDS, COL_LABEL) + sj('HDS') + esc(ROW_TIME, COL_LABEL) + sj('経過'))
    t['s_trk_l'] = esc(ROW_TRACK, TRACK_COL - 10) + sj('[Pico W]')
    t['s_trk_r'] = esc(ROW_TRACK, TRACK_COL + TRACK_LEN * 2 + 2) + sj('[X68000 Z]')
    bar = ' S: 設定画面 (Wi-Fi / SMB / HDS)      ※ HDS を変えたら Z の電源を入れ直してください '
    # 枠の縦線 (┃ は 2 桁の字の真ん中に線がある: 4 桁目の左端と 93 桁目の右端) にそろえる。文言は真ん中
    b = sj(bar)
    bw = FRAME_WIDTH - 2
    lpad = (bw - len(b)) // 2
    t['s_bar'] = esc(ROW_BAR, FRAME_LEFT + 1) + b' ' * lpad + b + b' ' * (bw - len(b) - lpad)
    # 書き直す欄の位置
    t['e_wifi'] = esc(ROW_WIFI, COL_VALUE)
    t['e_smb'] = esc(ROW_SMB, COL_VALUE)
    t['e_hds'] = esc(ROW_HDS, COL_VALUE)
    t['e_time'] = esc(ROW_TIME, COL_VALUE)
    t['e_ip'] = esc(ROW_WIFI, COL_EXTRA)
    t['e_retry'] = esc(ROW_SMB, COL_EXTRA)
    t['e_msg'] = esc(ROW_MSG, COL_LABEL)
    t['e_track'] = esc(ROW_TRACK, TRACK_COL)
    # 欄の中身 (幅をそろえる)
    for k, s in {'v_wifi0': '○ 未接続', 'v_wifi1': '○ 接続中', 'v_wifi2': '● 接続済み',
                 'v_smb0': '― ', 'v_smb1': '○ 接続待ち', 'v_smb2': '○ 接続中', 'v_smb3': '● 接続済み',
                 'v_hds0': '○ 待っています', 'v_hds1': '○ 開いています', 'v_hds2': '× 開けません',
                 'v_hds3': '● 開きました'}.items():
        t[k] = pad(s, VALUE_W)
    t['m_ip'] = sj('IP ')
    t['m_sec'] = sj(' 秒')
    t['m_trying'] = sj('(接続を試しています)')
    t['m_next'] = sj('(次の再接続まで ')
    t['m_secp'] = sj(' 秒)')
    t['m_noresp'] = sj('Pico の応答がありません (SCSI ID 6)')
    t['m_nohds'] = sj('HDS を開けません。設定画面 (S) で config の HDS を確かめてください')
    t['m_changed'] = sj('HDS が変わりました。Z の電源を入れ直してください')
    t['m_rail'] = sj('━')
    t['m_packet'] = sj('◆')
    t['m_half'] = sj('bootwait: 2nd half missing')
    return t


def orbitron(px, weight):
    if not os.path.exists(FONT):
        os.makedirs(os.path.dirname(FONT), exist_ok=True)
        urllib.request.urlretrieve(FONT_URL, FONT)
        urllib.request.urlretrieve(FONT_URL.replace('Orbitron%5Bwght%5D.ttf', 'OFL.txt'),
                                   os.path.join(os.path.dirname(FONT), 'OFL.txt'))
    f = ImageFont.truetype(FONT, px)
    f.set_variation_by_axes([weight])
    return f


def emblem(height):
    """電源の輪と Z (SVG の viewBox 0 0 100 100 のとおり)。4 倍で描いて縮める。戻り値: (LIGHT, MID) の 2 値画像"""
    S = 4
    w, h = round(height * ASPECT), height                  # 縦を 1/ASPECT に: 映すと輪が丸い
    sx, sy = w * S / 100, h * S / 100
    P = lambda x, y: (x * sx, y * sy)
    light = Image.new('L', (w * S, h * S), 0)
    mid = Image.new('L', (w * S, h * S), 0)
    dm = ImageDraw.Draw(mid)
    dm.ellipse([P(50 - 47, 50 - 47), P(50 + 47, 50 + 47)], fill=255)      # r=44、太さ 6
    dm.ellipse([P(50 - 41, 50 - 41), P(50 + 41, 50 + 41)], fill=0)
    dm.rectangle([P(44, 0), P(56, 14)], fill=0)                          # 上の切れ目
    dl = ImageDraw.Draw(light)
    dl.rectangle([P(47, 0), P(53, 22)], fill=255)                        # 電源の軸
    dl.polygon([P(*q) for q in ((28, 30), (72, 30), (72, 38), (40, 64), (72, 64), (72, 72),
                                (28, 72), (28, 64), (60, 38), (28, 38))], fill=255)
    out = []
    for im in (light, mid):
        im = im.resize((w, h), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0)
        out.append(im)
    return out


def spaced(text, font, spacing):
    """字間 spacing ドットで並べた 2 値画像 (縦は 1/ASPECT)"""
    asc, desc = font.getmetrics()
    widths = [font.getlength(c) for c in text]
    W = int(sum(widths) + spacing * (len(text) - 1)) + 4
    img = Image.new('L', (W, asc + desc), 0)
    d = ImageDraw.Draw(img)
    x = 0
    for c, cw in zip(text, widths):
        d.text((x, 0), c, fill=255, font=font)
        x += cw + spacing
    img = img.crop(img.getbbox())
    img = img.resize((img.size[0], round(img.size[1] / ASPECT)), Image.LANCZOS)
    return img.point(lambda v: 255 if v >= 128 else 0)


def art():
    """ロゴ (縦積み): 上に電源の輪と Z、その下に 1 行で "PICO Z LOADER"。どちらも横の中央。
    戻り値: (W, H, [p0, p1, p2, p3])"""
    EH = 88
    light, mid = emblem(EH)
    f9, f7 = orbitron(40, 900), orbitron(40, 700)
    pico = spaced('PICO', f9, 3)                                         # 白
    z = spaced('Z', f9, 3)                                               # LIGHT
    loader = spaced('LOADER', f7, 6)                                     # DARK (字間を広く)
    sp = 22                                                              # 語の間
    ww = pico.size[0] + sp + z.size[0] + sp + loader.size[0]
    wh = max(pico.size[1], z.size[1], loader.size[1])
    gap = 12
    W = max(light.size[0], ww)
    H = EH + gap + wh
    planes = [Image.new('1', (W, H), 0) for _ in range(4)]

    def paste(img, x, y, ps):
        px = img.load()
        for yy in range(img.size[1]):
            for xx in range(img.size[0]):
                if px[xx, yy] and 0 <= x + xx < W and 0 <= y + yy < H:
                    for p in range(4):
                        planes[p].putpixel((x + xx, y + yy), 1 if p in ps else 0)

    ex = (W - light.size[0]) // 2
    paste(mid, ex, 0, (2, 3))                                            # 輪 = 色 12
    paste(light, ex, 0, (2,))                                            # Z と軸 = 色 4
    x = (W - ww) // 2
    y = EH + gap
    paste(pico, x, y + wh - pico.size[1], (0, 1))                        # 下端 (字の並び) をそろえる
    x += pico.size[0] + sp
    paste(z, x, y + wh - z.size[1], (2,))
    x += z.size[0] + sp
    paste(loader, x, y + wh - loader.size[1], (3,))
    return W, H, planes


def placed():
    """ロゴを画面の横の中央に置く (紋章も字も中央に描いてあるので外形の中央)。バイト単位でしか置けないので、
    左に空き列を足してドット単位にする。戻り値: (幅 (8 の倍数), 高さ, プレーン, 置くバイト位置, 情報)"""
    W, H, planes = art()
    left = round(384 - W / 2)
    pad = left % 8
    W2 = (pad + W + 7) // 8 * 8
    out = []
    for p in planes:
        q = Image.new('1', (W2, H), 0)
        q.paste(p, (pad, 0))
        out.append(q)
    assert H <= 8 * 16, H
    return W2, H, out, left // 8, 'stacked lockup %dx%d, left edge x=%d' % (W, H, left)


def pieces(W, H, planes, left):
    """ロゴを「塗った点のある横長の帯」ごとに切り、プレーンごとの絵にする (空のプレーンは持たない、同じ絵は 1 つにまとめる)。
    戻り値: [(テキスト VRAM の番地, バイト幅, 高さ, データ番号)], [データ]"""
    rows = [y for y in range(H) if any(p.getpixel((x, y)) for p in planes for x in range(W))]
    bands, start = [], rows[0]
    for a, b in zip(rows, rows[1:] + [None]):
        if b is None or b != a + 1:
            bands.append((start, a + 1))
            start = b
    out, datas = [], []
    for y0, y1 in bands:
        xs = [x for x in range(W) for y in range(y0, y1) if any(p.getpixel((x, y)) for p in planes)]
        x0, x1 = min(xs), max(xs) + 1
        gx = left + x0                                                   # 画面のドット位置
        pad = gx % 8
        wb = (pad + x1 - x0 + 7) // 8
        for i, p in enumerate(planes):
            q = Image.new('1', (wb * 8, y1 - y0), 0)
            q.paste(p.crop((x0, y0, x1, y1)), (pad, 0))
            d = q.tobytes()
            if not any(d):
                continue
            if d not in datas:
                datas.append(d)
            addr = 0xe00000 + i * 0x20000 + ((ART_ROW - 1) * 16 + y0) * 128 + gx // 8
            out.append((addr, wb, y1 - y0, datas.index(d)))
    return out, datas


def asm_bytes(label, data):
    out = ['%s:' % label]
    for i in range(0, len(data), 16):
        out.append('    .byte   ' + ','.join('0x%02x' % c for c in data[i:i + 16]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', default=os.path.join(HERE, 'bootwait_gen.S'))
    ap.add_argument('--png')
    a = ap.parse_args()
    W, H, pl, xb, info = placed()
    lines = ['/* mkbootwait.py が作った。直接直さない */', '',
             'TRACK_LEN   = %d' % TRACK_LEN,
             'VALUE_W     = %d' % VALUE_W, 'EXTRA_W     = %d' % EXTRA_W, 'MSG_W       = %d' % MSG_W,
             'C_DIM       = %d' % C_DIM, 'C_OK        = %d' % C_OK, 'C_TEXT      = %d' % C_TEXT,
             'C_WARN      = %d' % C_WARN, 'C_BAR       = %d' % C_BAR,
             'PAL12       = 0x%04x' % grb(*PALETTE[12]), 'PAL12_PULSE = 0x%04x' % grb(*PULSE),
             '']
    lines.append('pal_set:                                /* (パレット番号 * 2, 色) の組、0xffff で終わり */')
    for i, (r, g, b) in sorted(PALETTE.items()):
        lines.append('    .word   %d, 0x%04x' % (i * 2, grb(r, g, b)))
    lines.append('    .word   0xffff')
    for k, v in strings().items():
        lines += asm_bytes(k, v + b'\0')
    lines.append('    .even')
    ps, datas = pieces(W, H, pl, xb * 8)
    lines.append('art_blits:                              /* (VRAM の番地, バイト幅 - 1, ラスタ数 - 1, art_blits からのデータの位置)、0 で終わり */')
    for addr, wb, h, di in ps:
        lines.append('    .long   0x%06x' % addr)
        lines.append('    .word   %d, %d, art_d%d-art_blits' % (wb - 1, h - 1, di))
    lines.append('    .long   0')
    for i, d in enumerate(datas):
        lines += asm_bytes('art_d%d' % i, d)
    lines.append('    .even')
    open(a.o, 'w').write('\n'.join(lines) + '\n')
    if a.png:
        img = Image.new('RGB', (W, H))
        pal = {0: (0, 0, 0)}
        for i, (r, g, b) in PALETTE.items():
            pal[i] = (r * 8, g * 8, b * 8)
        for y in range(H):
            for x in range(W):
                v = sum(pl[i].getpixel((x, y)) << i for i in range(4))
                img.putpixel((x, y), pal.get(v, (255, 0, 255)))
        img.resize((W * 2, H * 2), Image.NEAREST).save(a.png)
    print('art %dx%d, %d blits, %d bytes of bitmap; %s' % (W, H, len(ps), sum(map(len, datas)), info))


if __name__ == '__main__':
    main()
