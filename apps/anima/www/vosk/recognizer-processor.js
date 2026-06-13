// AudioWorklet that forwards mic samples to the Vosk recognizer worker.
// Mirrors the official vosk-browser example: floats [-1,1] -> int16 range, sent
// over the MessagePort the main thread hands us at init.
class RecognizerAudioProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super(options);
    this.port.onmessage = (event) => {
      if (event.data.action === 'init') {
        this._recognizerId = event.data.recognizerId;
        this._recognizerPort = event.ports[0];
      }
    };
  }
  process(inputs) {
    const data = inputs[0][0];
    if (this._recognizerPort && data) {
      const audioArray = data.map((v) => v * 0x8000);   // Kaldi wants int16-scaled samples
      this._recognizerPort.postMessage(
        { action: 'audioChunk', data: audioArray, recognizerId: this._recognizerId, sampleRate },
        { transfer: [audioArray.buffer] }
      );
    }
    return true;
  }
}
registerProcessor('recognizer-processor', RecognizerAudioProcessor);
