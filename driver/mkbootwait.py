#!/usr/bin/env python3
"""bootwait.S (待ち IPL の後半) が使う文字列と紋章のビットマップを bootwait_gen.S に書き出す。
出力は git に入れる (ビルドに PIL を要らないように)。直したら: python3 mkbootwait.py [-o bootwait_gen.S] [--png art.png]

紋章は自前の幾何学的な絵 (Sharp のロゴの写しではない): 中心で向き合う 4 つの三角形でできた X と、
斜体の太字の "X68000 Z"。テキスト VRAM のプレーン 2・3 に描く (プレーン 0・1 は IOCS の文字)。
  プレーン 2 だけ = 色 4 (赤)、プレーン 3 だけ = 色 8 (影)、両方 = 色 12 (明るい橙)
文字列は UTF-8 で書き、Shift_JIS (cp932) にする。全角は 2 バイト = 2 桁なので、バイト数で桁をそろえられる。
'\\' (0x5C) は X68k では円記号になるので使わない。
"""
import argparse, os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- 画面の配置 (1 始まりの行・桁。テキスト画面は 96 桁 x 32 行、1 文字 8x16 ドット) ----
FRAME_TOP, FRAME_BOTTOM, FRAME_LEFT, FRAME_WIDTH = 2, 28, 3, 92   # 3-94 桁
ART_ROW = 4                     # 紋章の上端の行 (ドットでは (ART_ROW-1)*16)
ROW_SUB, ROW_TITLE = 10, 12
ROW_WIFI, ROW_SMB, ROW_HDS, ROW_TIME = 14, 15, 16, 17
ROW_TRACK, ROW_MSG, ROW_BAR = 20, 23, 30
COL_LABEL, COL_VALUE, COL_EXTRA = 12, 20, 36
VALUE_W, EXTRA_W, MSG_W = 16, 50, 80
TRACK_COL, TRACK_LEN = 23, 20   # ◆ が走る線 (全角 20 文字 = 40 桁)

# 文字の色 (IOCS _B_COLOR: 0-3 が色、+8 で反転)。テキストパレット 1-3 を入れ替える (青系)
#   C_DIM 深い青 (枠・線)、C_OK 水色 (つながった・副題・◆)、C_TEXT 白 (見出し・待ち)、C_WARN 白の反転 (知らせ)、C_BAR 水色の反転
C_DIM, C_OK, C_TEXT = 1, 2, 3
C_WARN, C_BAR = C_TEXT + 8, C_OK + 8
# テキストパレット (GRB 各 5 ビット + I)。待ち IPL が始めに控えて書き、本物の IPL に飛ぶ前に戻す
PALETTE = {                     # (R, G, B) 各 0-31
    1: (2, 7, 22),              # 深い青 (枠・線)
    2: (12, 24, 31),            # 水色 (つながった ●・副題・◆)
    3: (31, 31, 31),            # 白
    4: (14, 26, 31),            # 紋章: 字と三角形の組 A (明るい水色)
    8: (1, 3, 13),              # 紋章: 字の影 (紺)
    12: (5, 13, 30),            # 紋章: 三角形の組 B (青)。待っている間は PULSE と交互に光る
}
PULSE = (22, 29, 31)            # 組 B が明るくなるとき


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

    t['s_sub'] = center(ROW_SUB, 'Raspberry Pi Pico W  /  x68kzremotedrv   ・   BOOT WAIT')
    t['s_title'] = center(ROW_TITLE, 'Pico: SMB 接続待ち…   (Wi-Fi / SMB を確認してください)')
    t['s_labels'] = (esc(ROW_WIFI, COL_LABEL) + sj('Wi-Fi') + esc(ROW_SMB, COL_LABEL) + sj('SMB') +
                     esc(ROW_HDS, COL_LABEL) + sj('HDS') + esc(ROW_TIME, COL_LABEL) + sj('経過'))
    t['s_trk_l'] = esc(ROW_TRACK, TRACK_COL - 10) + sj('[Pico W]')
    t['s_trk_r'] = esc(ROW_TRACK, TRACK_COL + TRACK_LEN * 2 + 2) + sj('[X68000 Z]')
    bar = ' S: 設定画面 (Wi-Fi / SMB / HDS)      ※ HDS を変えたら Z の電源を入れ直してください '
    t['s_bar'] = center(ROW_BAR, bar)
    # 書き直す欄の位置
    t['e_wifi'] = esc(ROW_WIFI, COL_VALUE)
    t['e_smb'] = esc(ROW_SMB, COL_VALUE)
    t['e_hds'] = esc(ROW_HDS, COL_VALUE)
    t['e_time'] = esc(ROW_TIME, COL_VALUE)
    t['e_ip'] = esc(ROW_WIFI, COL_EXTRA)
    t['e_retry'] = esc(ROW_SMB, COL_EXTRA)
    t['e_msg'] = esc(ROW_MSG, 9)
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


def emblem(size):
    """中心で向き合う 4 つの三角形の X (隙間あり)。各三角形は頂点が中心のそば、底辺が角を斜めに切る線。
    左上・右下は組 B (両プレーン = 色 12)、右上・左下は組 A (プレーン 2 だけ = 色 4、字と同じ)"""
    img = Image.new('L', (size, size), 0)
    d = ImageDraw.Draw(img)
    m = size - 1
    c = m / 2
    g = size * 0.07                                         # 中心の隙間
    b = size * 0.36                                         # 角から底辺の端までの長さ
    for (sx, sy), col in (((-1, -1), 2), ((1, 1), 2), ((1, -1), 1), ((-1, 1), 1)):
        cx = 0 if sx < 0 else m                             # この三角形の角
        cy = 0 if sy < 0 else m
        apex = (c + sx * g, c + sy * g)
        a = (cx, cy - sy * b)                               # 角から縦に b
        e = (cx - sx * b, cy)                               # 角から横に b
        d.polygon([apex, a, e], fill=col)
    return img


def wordmark(height):
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Black.ttf', int(height * 1.32))
    text = 'X68000 Z'
    box = font.getbbox(text)
    img = Image.new('L', (box[2] - box[0] + height, box[3] - box[1] + 4), 0)
    ImageDraw.Draw(img).text((-box[0] + height * 0.3, -box[1] + 2), text, fill=255, font=font)
    sh = 0.22                                               # 斜体
    img = img.transform(img.size, Image.AFFINE, (1, sh, -sh * img.size[1], 0, 1, 0), Image.BILINEAR)
    img = img.point(lambda v: 255 if v > 110 else 0)
    return img.crop(img.getbbox())


def art():
    H = 80
    em = emblem(H)
    wm = wordmark(52)
    gap = 18
    W = (H + gap + wm.size[0] + 4 + 7) // 8 * 8
    p2 = Image.new('1', (W, H), 0)
    p3 = Image.new('1', (W, H), 0)
    ey = em.load()
    for y in range(H):
        for x in range(H):
            v = ey[x, y]
            if v == 2:
                p2.putpixel((x, y), 1)
                p3.putpixel((x, y), 1)
            elif v == 1:
                p2.putpixel((x, y), 1)
    ox, oy = H + gap, (H - wm.size[1]) // 2 + 4
    wl = wm.load()
    for y in range(wm.size[1]):
        for x in range(wm.size[0]):
            if wl[x, y]:
                p2.putpixel((ox + x, oy + y), 1)
    # 影: 右下に 3 ドットずらした字のうち、字に重ならないところ (プレーン 3 だけ = 色 8)
    for y in range(wm.size[1]):
        for x in range(wm.size[0]):
            if wl[x, y]:
                X, Y = ox + x + 3, oy + y + 3
                if X < W and Y < H and not p2.getpixel((X, Y)):
                    p3.putpixel((X, Y), 1)
    return W, H, p2, p3


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
    W, H, p2, p3 = art()
    xb = (768 - W) // 2 // 8
    lines = ['/* mkbootwait.py が作った。直接直さない */', '',
             'ART_WB      = %d' % (W // 8), 'ART_H       = %d' % H,
             'ART_OFF     = %d                  /* テキスト VRAM の中の位置: 行 * 128 + 桁 */' % ((ART_ROW - 1) * 16 * 128 + xb),
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
    lines += asm_bytes('art_p2', p2.tobytes())
    lines += asm_bytes('art_p3', p3.tobytes())
    lines.append('    .even')
    open(a.o, 'w').write('\n'.join(lines) + '\n')
    if a.png:
        img = Image.new('RGB', (W, H))
        pal = {0: (0, 0, 0)}
        for i, (r, g, b) in PALETTE.items():
            pal[i] = (r * 8, g * 8, b * 8)
        for y in range(H):
            for x in range(W):
                img.putpixel((x, y), pal.get(p2.getpixel((x, y)) * 4 + p3.getpixel((x, y)) * 8, (255, 0, 255)))
        img.resize((W * 2, H * 2), Image.NEAREST).save(a.png)
    print('art %dx%d (%d bytes x 2 planes)' % (W, H, W // 8 * H))


if __name__ == '__main__':
    main()
