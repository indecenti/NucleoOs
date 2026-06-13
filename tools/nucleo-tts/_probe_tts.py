import importlib.util as u
def has(m): return bool(u.find_spec(m))
print("piper(piper-tts):", has("piper"))
print("TTS(coqui):", has("TTS"))
print("onnxruntime:", has("onnxruntime"))
print("torch:", has("torch"))
try:
    import torch
    print("  torch.cuda.is_available:", torch.cuda.is_available())
    if torch.cuda.is_available():
        print("  device:", torch.cuda.get_device_name(0), "| torch:", torch.__version__)
except Exception as e:
    print("  torch err:", e)
try:
    import onnxruntime as o
    print("onnxruntime providers:", o.get_available_providers())
except Exception as e:
    print("onnxruntime err:", e)
