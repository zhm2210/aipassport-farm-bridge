#!/usr/bin/env python3
"""把固件里那 8 个告警音原样合成成 wav，方便在电脑上直接挨个试听对比。
合成逻辑跟 aipassport_farm.ino 里的 tone() 保持一致（16kHz / 4ms 淡入淡出）。

用法：python tools/preview_tones.py
输出：音效预览/finish1_琶音.wav ... fault4_低沉.wav
"""
import math, os, struct, wave

SR = 16000          # 和固件 TONE_SR 保持一致（别改 32k，见固件注释）
ATK = int(SR * 8 / 1000)
TAIL = int(SR * 10 / 1000)
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), '音效预览')

# wave: 0=纯正弦（唯一在用） 1=方波 2=已废弃（原双音和弦，现在按正弦处理）
def sample(freq, i, wave):
    p = 2 * math.pi * freq * i / SR
    s = math.sin(p)
    if wave == 1:
        return 0.85 if s >= 0 else -0.85
    if wave == 2:
        return s * 0.9
    return s

def tone(seq, freq, ms, amp=1.0, wave=0):
    """钟形包络：8ms 起音 + 指数衰减 + 10ms 收尾（跟固件 tone() 一致）"""
    n = int(SR * ms / 1000)
    end_lvl = 0.45 if ms > 400 else 0.28
    for i in range(n):
        if i < ATK:
            env = i / ATK
        else:
            t = (i - ATK) / max(1, n - ATK)
            env = end_lvl ** t
        if i > n - TAIL:
            env *= (n - i) / TAIL
        seq.append(sample(freq, len(seq), wave) * amp * env)

def gap(seq, ms):
    seq.extend([0.0] * int(SR * ms / 1000))

def finish(i):
    s = []
    if i == 1:
        tone(s, 784, 200); gap(s, 110); tone(s, 523, 430)
    elif i == 2:
        tone(s, 784, 80); gap(s, 45)
        tone(s, 784, 80); gap(s, 45)
        tone(s, 659, 80); gap(s, 45)
        tone(s, 523, 400)
    elif i == 3:
        for _ in range(3):
            tone(s, 659, 75); gap(s, 40)
            tone(s, 784, 75); gap(s, 40)
        tone(s, 784, 420)
    else:
        tone(s, 523, 160); gap(s, 65)
        tone(s, 659, 160); gap(s, 65)
        tone(s, 784, 460)
    return s

def fault(i):
    s = []
    if i == 1:
        tone(s, 587, 650); gap(s, 160); tone(s, 523, 720)
    elif i == 2:
        for _ in range(2):
            for _ in range(4):
                tone(s, 784, 85); gap(s, 50)
            gap(s, 50)
            tone(s, 587, 300); gap(s, 240)
    elif i == 3:
        tone(s, 523, 300); gap(s, 150)
        tone(s, 494, 300); gap(s, 150)
        tone(s, 494, 460)
    else:
        for _ in range(2):
            tone(s, 784, 110); gap(s, 60)
            tone(s, 784, 110); gap(s, 60)
            tone(s, 587, 220); gap(s, 240)
    return s


def speaker_sim(seq, fc=900.0, poles=2):
    """模拟工牌那个小喇叭的频响。
    实测线索：784Hz 很清楚，988Hz 明显变小，1319Hz 几乎听不见
    → 大约 900Hz 以上开始以 -12dB/oct 往下掉。
    不模拟的话，电脑上听着"很尖"的方波音，到喇叭上会被削平，
    跟正弦听起来一模一样（这就是为什么预览和实际对不上）。"""
    a = 1 - math.exp(-2 * math.pi * fc / SR)
    out = list(seq)
    for _ in range(poles):
        y = 0.0
        for i, x in enumerate(out):
            y += a * (x - y)
            out[i] = y
    return out

def write_wav(path, seq):
    os.makedirs(OUT_DIR, exist_ok=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        frames = b''.join(struct.pack('<h', max(-32768, min(32767, int(v * 26000)))) for v in seq)
        w.writeframes(frames)

FINISH = ['琶音', '双音', '清脆', '欢快']
FAULT = ['三连', '长鸣', '四短', '低沉']

NOTES = {
    '完成1': '3 声 慢速上行', '完成2': '2 声 高→低大跳',
    '完成3': '4 声 三短一长下行', '完成4': '7 声 来回抖+长尾',
    '故障1': '6 声 急促两轮', '故障2': '2 声 超长下行',
    '故障3': '10 声 最急四连击', '故障4': '3 声 低音闷响',
}

print('已按小喇叭频响（900Hz 以上 -12dB/oct）过滤，听感≈工牌实机')
print('%-8s %-8s %-8s %-8s %s' % ('文件', '时长', '峰值', 'RMS', '节奏特征'))
rows = []
for i, name in enumerate(FINISH):
    s = speaker_sim(finish(i))
    p = os.path.join(OUT_DIR, '完成%d_%s.wav' % (i + 1, name))
    write_wav(p, s)
    rows.append(('完成%d_%s' % (i + 1, name), s))
for i, name in enumerate(FAULT):
    s = speaker_sim(fault(i))
    p = os.path.join(OUT_DIR, '故障%d_%s.wav' % (i + 1, name))
    write_wav(p, s)
    rows.append(('故障%d_%s' % (i + 1, name), s))
for tag, s in rows:
    rms = math.sqrt(sum(x * x for x in s) / max(1, len(s)))
    print('%-8s %6dms %7.2f %8.3f %s' % (tag.replace('.wav', ''), len(s) * 1000 // SR,
          max(abs(x) for x in s), rms, NOTES.get(tag.split('_')[0], '')))
print('\n输出目录：', OUT_DIR)
