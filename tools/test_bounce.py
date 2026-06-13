import sys
import struct
import wave
import os
import math

def read_npx_header(path):
    if not os.path.exists(path): return None
    with open(path, "rb") as f:
        if f.read(4) != b'NPX1': return None
        version, frate, channels, tonic, scale = struct.unpack("<BBBBB", f.read(5))
        f.read(3)
        bpm, dur, frames, srate, peak, lufs = struct.unpack("<ffIIff", f.read(24))
        return {
            "bpm": bpm, "duration_s": dur, "frames": frames, "srate": srate,
            "peak": peak, "lufs": lufs, "frame_rate": frate
        }

def find_drop_s(npx_path, header):
    if not os.path.exists(npx_path) or not header: return 0.0
    with open(npx_path, "rb") as f:
        f.seek(36)
        for i in range(header["frames"]):
            data = f.read(12)
            if len(data) < 12: break
            rms, bass, mid, high, beat, cue, bright, pad = struct.unpack("<HHHHBBBB", data)
            if cue == 1: return i / header["frame_rate"]
    return 0.0

def find_nearest_beat(npx_path, header, target_s):
    if not os.path.exists(npx_path) or not header: return target_s
    frate = header["frame_rate"]
    start_f = int(max(0, target_s - 1.0) * frate)
    end_f = int(min(header["frames"], (target_s + 2.0) * frate))
    
    with open(npx_path, "rb") as f:
        f.seek(36 + start_f * 12)
        closest, min_dist = target_s, 999.0
        for i in range(start_f, end_f):
            data = f.read(12)
            if len(data) < 12: break
            beat = data[8]
            if beat > 0:
                beat_s = (i + (beat - 1)/254.0) / frate
                dist = abs(beat_s - target_s)
                if dist < min_dist:
                    min_dist = dist
                    closest = beat_s
        return closest

class Biquad:
    def __init__(self):
        self.b0 = self.b1 = self.b2 = self.a1 = self.a2 = 0
        self.z1_L = self.z2_L = self.z1_R = self.z2_R = 0

    def calc_hpf(self, fc, sample_rate, q):
        if fc > sample_rate / 2.5: fc = sample_rate / 2.5
        w0 = 2.0 * math.pi * fc / sample_rate
        alpha = math.sin(w0) / (2.0 * q)
        cosw0 = math.cos(w0)
        a0 = 1.0 + alpha
        self.b0 = ((1.0 + cosw0) / 2.0) / a0
        self.b1 = -(1.0 + cosw0) / a0
        self.b2 = ((1.0 + cosw0) / 2.0) / a0
        self.a1 = (-2.0 * cosw0) / a0
        self.a2 = (1.0 - alpha) / a0

    def calc_lpf(self, fc, sample_rate, q):
        if fc > sample_rate / 2.5: fc = sample_rate / 2.5
        w0 = 2.0 * math.pi * fc / sample_rate
        alpha = math.sin(w0) / (2.0 * q)
        cosw0 = math.cos(w0)
        a0 = 1.0 + alpha
        self.b0 = ((1.0 - cosw0) / 2.0) / a0
        self.b1 = (1.0 - cosw0) / a0
        self.b2 = ((1.0 - cosw0) / 2.0) / a0
        self.a1 = (-2.0 * cosw0) / a0
        self.a2 = (1.0 - alpha) / a0

    def process(self, inL, inR):
        outL = inL * self.b0 + self.z1_L
        self.z1_L = inL * self.b1 - outL * self.a1 + self.z2_L
        self.z2_L = inL * self.b2 - outL * self.a2
        
        outR = inR * self.b0 + self.z1_R
        self.z1_R = inR * self.b1 - outR * self.a1 + self.z2_R
        self.z2_R = inR * self.b2 - outR * self.a2
        return outL, outR

def mix_dsp_wav(pathA, pathB, outPath, plan):
    print(f"DSP Bounce: {pathA} -> {pathB}")
    print(f"Fade: {plan['fade_ms']}ms | A_out: {plan['a_out']:.2f}s | B_in: {plan['b_in']:.2f}s | Pitch: {plan['pitch_ratio']:.3f} | Type: {plan['type']}")
    
    wa = wave.open(pathA, 'rb')
    wb = wave.open(pathB, 'rb')
    
    srA, chA = wa.getframerate(), wa.getnchannels()
    srB, chB = wb.getframerate(), wb.getnchannels()
    
    total_samps = int((plan['fade_ms'] / 1000.0) * srA)
    sampsB_req = int(total_samps * plan['pitch_ratio']) + 2
    
    wa.setpos(int(plan['a_out'] * srA))
    rawA = wa.readframes(total_samps)
    
    wb.setpos(int(plan['b_in'] * srB))
    rawB = wb.readframes(sampsB_req)
    
    out = wave.open(outPath, 'wb')
    out.setnchannels(chA)
    out.setsampwidth(2)
    out.setframerate(srA)
    
    out_data = bytearray(total_samps * chA * 2)
    bq = Biquad()
    
    limit = min(total_samps, len(rawA)//(chA*2))
    
    for i in range(limit):
        progress = i / total_samps
        
        fL_a = float(struct.unpack_from("<h", rawA, i * chA * 2)[0])
        fR_a = float(struct.unpack_from("<h", rawA, (i * chA + 1) * 2)[0]) if chA==2 else fL_a
        
        if plan['type'] == 'BASS_SWAP':
            fc = 20.0 + (progress**3) * 1980.0
            bq.calc_hpf(fc, 44100.0, 0.707)
            fL_a, fR_a = bq.process(fL_a, fR_a)
        elif plan['type'] == 'FILTER_FADE':
            fc = 20000.0 - (progress**2) * 19800.0
            bq.calc_lpf(fc, 44100.0, 0.707)
            fL_a, fR_a = bq.process(fL_a, fR_a)
            
        b_idx_f = i * plan['pitch_ratio']
        b_idx = int(b_idx_f)
        frac = b_idx_f - b_idx
        
        fL_b, fR_b = 0.0, 0.0
        if (b_idx + 1) * chB * 2 < len(rawB):
            vL1 = struct.unpack_from("<h", rawB, b_idx * chB * 2)[0]
            vR1 = struct.unpack_from("<h", rawB, (b_idx * chB + 1) * 2)[0] if chB==2 else vL1
            vL2 = struct.unpack_from("<h", rawB, (b_idx + 1) * chB * 2)[0]
            vR2 = struct.unpack_from("<h", rawB, ((b_idx + 1) * chB + 1) * 2)[0] if chB==2 else vL2
            
            fL_b = vL1 * (1.0 - frac) + vL2 * frac
            fR_b = vR1 * (1.0 - frac) + vR2 * frac
            
        volA, volB = 1.0, 1.0
        if plan['type'] == 'BASS_SWAP':
            volA = math.cos(progress * math.pi / 2)
            volB = math.cos((1.0 - progress) * math.pi / 2)
        elif plan['type'] == 'FILTER_FADE':
            volA = 1.0 - progress
            volB = progress
        elif plan['type'] == 'TENSION_BUILDUP':
            volA = 1.0 - (progress ** 2.0)
            volB = progress ** 0.5
            
        mixedL = fL_a * volA + fL_b * volB
        mixedR = fR_a * volA + fR_b * volB
        
        if plan['type'] == 'TENSION_BUILDUP':
            fc = 20.0 + (progress ** 2.0) * 3980.0
            bq.calc_hpf(fc, 44100.0, 0.707)
            mixedL, mixedR = bq.process(mixedL, mixedR)
            
        outL = int(mixedL)
        outR = int(mixedR)
        
        outL = max(-32768, min(32767, outL))
        outR = max(-32768, min(32767, outR))
        
        struct.pack_into("<h", out_data, i * chA * 2, outL)
        if chA == 2: struct.pack_into("<h", out_data, (i * chA + 1) * 2, outR)
            
    out.writeframes(out_data)
    out.close()
    wa.close(); wb.close()
    print(f"Generated {outPath}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(1)
        
    fileA = sys.argv[1]
    fileB = sys.argv[2]
    
    npxA = fileA.replace(".wav", ".npx")
    npxB = fileB.replace(".wav", ".npx")
    hA = read_npx_header(npxA)
    hB = read_npx_header(npxB)
    
    ratio = hA['bpm'] / hB['bpm']
    
    plan = {
        'type': 'TENSION_BUILDUP',
        'fade_ms': 8000,
        'pitch_ratio': ratio,
        'a_out': find_nearest_beat(npxA, hA, hA['duration_s'] - 12.0),
        'b_in': find_nearest_beat(npxB, hB, find_drop_s(npxB, hB) - 8.0)
    }
    
    mix_dsp_wav(fileA, fileB, "bounce_test_buildup.wav", plan)
