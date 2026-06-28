#!/usr/bin/env node
// Generate Tank Duel SFX WAV files from synth recipes
// Output: /sd/data/tankduel/*.wav

import fs from 'fs';
import path from 'path';

const OUTPUT_DIR = '/sd/data/tankduel';
const SAMPLE_RATE = 22050;
const BITS_PER_SAMPLE = 16;

// Create output directory
if (!fs.existsSync(OUTPUT_DIR)) {
  fs.mkdirSync(OUTPUT_DIR, { recursive: true });
}

// Simple sine wave synth
function synth(freqHz, durationMs, sampleRate = SAMPLE_RATE) {
  const numSamples = Math.round(sampleRate * durationMs / 1000);
  const samples = [];
  for (let i = 0; i < numSamples; i++) {
    const t = i / sampleRate;
    const phase = 2 * Math.PI * freqHz * t;
    const sample = Math.sin(phase) * 0.8;
    samples.push(Math.max(-1, Math.min(1, sample)));
  }
  return samples;
}

// Decay envelope
function applyDecay(samples, decayMs, sampleRate = SAMPLE_RATE) {
  const decaySamples = Math.round(sampleRate * decayMs / 1000);
  return samples.map((s, i) => {
    if (i > decaySamples) return 0;
    const envelope = 1 - (i / decaySamples);
    return s * envelope;
  });
}

// Mix multiple signals
function mix(...signals) {
  const maxLen = Math.max(...signals.map(s => s.length));
  const result = [];
  for (let i = 0; i < maxLen; i++) {
    let sum = 0;
    for (const sig of signals) {
      sum += sig[i] || 0;
    }
    result.push(Math.max(-1, Math.min(1, sum / signals.length)));
  }
  return result;
}

// Convert samples to WAV buffer
function samplesToWav(samples) {
  const numChannels = 1;
  const blockAlign = (BITS_PER_SAMPLE / 8) * numChannels;
  const byteRate = SAMPLE_RATE * blockAlign;
  const dataSize = samples.length * blockAlign;

  const buffer = Buffer.alloc(44 + dataSize);
  let offset = 0;

  // RIFF header
  buffer.write('RIFF', offset); offset += 4;
  buffer.writeUInt32LE(36 + dataSize, offset); offset += 4;
  buffer.write('WAVE', offset); offset += 4;

  // fmt subchunk
  buffer.write('fmt ', offset); offset += 4;
  buffer.writeUInt32LE(16, offset); offset += 4; // subchunk1Size
  buffer.writeUInt16LE(1, offset); offset += 2; // audioFormat (PCM)
  buffer.writeUInt16LE(numChannels, offset); offset += 2;
  buffer.writeUInt32LE(SAMPLE_RATE, offset); offset += 4;
  buffer.writeUInt32LE(byteRate, offset); offset += 4;
  buffer.writeUInt16LE(blockAlign, offset); offset += 2;
  buffer.writeUInt16LE(BITS_PER_SAMPLE, offset); offset += 2;

  // data subchunk
  buffer.write('data', offset); offset += 4;
  buffer.writeUInt32LE(dataSize, offset); offset += 4;

  // Write PCM samples (16-bit signed)
  for (let i = 0; i < samples.length; i++) {
    const s = Math.round(samples[i] * 32767);
    buffer.writeInt16LE(s, offset);
    offset += 2;
  }

  return buffer;
}

// SFX recipes (duration ms, frequencies)
const recipes = {
  fire: () => {
    const s1 = synth(320, 25);
    const s2 = synth(160, 35);
    return mix(applyDecay(s1, 25), applyDecay(s2, 35));
  },

  hit: () => {
    const s1 = synth(220, 40);
    const s2 = synth(110, 50);
    return mix(applyDecay(s1, 40), applyDecay(s2, 50));
  },

  kill: () => {
    const s1 = applyDecay(synth(300, 50), 50);
    const s2 = applyDecay(synth(180, 80), 80);
    const s3 = applyDecay(synth(90, 140), 140);
    return mix(s1, s2, s3);
  },

  shop: () => {
    const s1 = applyDecay(synth(880, 50), 50);
    const s2 = applyDecay(synth(1174.7, 60), 60);
    return mix(s1, s2);
  },

  pu: () => {
    const s1 = applyDecay(synth(660, 40), 40);
    const s2 = applyDecay(synth(988, 50), 50);
    const s3 = applyDecay(synth(1320, 70), 70);
    return mix(s1, s2, s3);
  },

  buy: () => {
    const s1 = applyDecay(synth(784, 40), 40);
    const s2 = applyDecay(synth(1046.5, 60), 60);
    return mix(s1, s2);
  },

  sell: () => {
    const s1 = applyDecay(synth(1046.5, 40), 40);
    const s2 = applyDecay(synth(659.25, 60), 60);
    return mix(s1, s2);
  },

  win: () => {
    const s1 = applyDecay(synth(523.25, 80), 80);
    const s2 = applyDecay(synth(659.25, 90), 90);
    const s3 = applyDecay(synth(783.99, 100), 100);
    const s4 = applyDecay(synth(1046.5, 180), 180);
    return mix(s1, s2, s3, s4);
  },

  lose: () => {
    const s1 = applyDecay(synth(392, 100), 100);
    const s2 = applyDecay(synth(294, 120), 120);
    const s3 = applyDecay(synth(196, 200), 200);
    return mix(s1, s2, s3);
  },

  nav: () => {
    return applyDecay(synth(520, 20), 20);
  },

  sel: () => {
    const s1 = applyDecay(synth(660, 30), 30);
    const s2 = applyDecay(synth(880, 40), 40);
    return mix(s1, s2);
  }
};

// Generate and write WAV files
for (const [name, recipeFn] of Object.entries(recipes)) {
  try {
    const samples = recipeFn();
    const wav = samplesToWav(samples);
    const filePath = path.join(OUTPUT_DIR, `${name}.wav`);
    fs.writeFileSync(filePath, wav);
    console.log(`✓ ${name}.wav (${samples.length} samples, ${(wav.length/1024).toFixed(1)}KB)`);
  } catch (e) {
    console.error(`✗ ${name}.wav: ${e.message}`);
  }
}

console.log(`\nAll SFX generated to ${OUTPUT_DIR}`);
