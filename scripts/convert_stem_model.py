#!/usr/bin/env python3
"""Convert Meta's HT-Demucs 6-stem model (MIT) to the Core ML package Rounds uses.

Rounds splits songs about 3.5x faster with this than with the ONNX model, on
the CPU. (The Neural Engine's FP16 maths throws some stems far off, and Core ML
on the GPU hangs on this model, so Rounds asks for the CPU only.)

Needs Python 3.11-3.13 (PyTorch has no wheels for newer ones):

    uv venv --python 3.12 env
    uv pip install --python env/bin/python torch==2.4.1 "demucs-onnx[export]==0.3.4" coremltools==9.0
    env/bin/python scripts/convert_stem_model.py ~/Library/Application\\ Support/Rounds/Models

It writes HTDemucs6S_F16.mlpackage: input `mix` [1, 2, 343980] (7.8 s of
44.1 kHz stereo), output `stems` [1, 6, 2, 343980] in the order drums, bass,
other, vocals, guitar, piano. Rounds compiles it on first use.
"""
import builtins
import copy
import sys
import types
from pathlib import Path

import coremltools as ct
import numpy as np
import torch
from coremltools.converters.mil.frontend.torch import ops as ct_ops
from coremltools.converters.mil.frontend.torch.torch_op_registry import register_torch_op
from demucs.pretrained import get_model
from demucs_onnx.export import patch_htdemucs_for_onnx

SEGMENT = 343_980


# coremltools: traced shape arithmetic hands int()/float() one-element
# arrays, which its casts can't take; read the value out first.
class _Int(int):
    def __new__(cls, v):
        return builtins.int(np.asarray(v).reshape(-1)[0])


class _Float(float):
    def __new__(cls, v):
        return builtins.float(np.asarray(v).reshape(-1)[0])


def _int(context, node):
    ct_ops._cast(context, node, _Int, "int32")


def _float(context, node):
    ct_ops._cast(context, node, _Float, "fp32")


_int.__name__, _float.__name__ = "int", "float"
register_torch_op(_int, override=True)
register_torch_op(_float, override=True)


def _mask_rank5(self, z, m):
    """Core ML tensors have at most 5 dimensions: drop the batch (always 1).
    The iSTFT then gives (S, C, L), which the model broadcasts onto (1, S, C, L)."""
    _, sources, channels, freqs, frames = m.shape
    return m.view(sources, channels // 2, 2, freqs, frames)


class Wrapper(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, mix):
        return self.model(mix)


def main(out_dir: Path) -> None:
    bag = get_model("htdemucs_6s")
    original = (bag.models[0] if hasattr(bag, "models") else bag).eval().to("cpu")
    # StemSplit's patches: real-valued STFT/iSTFT (convolutions), no random
    # position shift, plain-op attention, float segment length.
    patched = patch_htdemucs_for_onnx(copy.deepcopy(original))
    patched._mask = types.MethodType(_mask_rank5, patched)
    wrapper = Wrapper(patched).eval()

    torch.manual_seed(0)
    x = (torch.rand(1, 2, SEGMENT) * 2 - 1) * 0.5
    with torch.no_grad():
        diff = (original(x) - wrapper(x)).abs().max().item()
    print(f"patched vs original PyTorch: max difference {diff:.2e}")

    traced = torch.jit.trace(wrapper, (x,), strict=False, check_trace=False)
    model = ct.convert(
        traced,
        inputs=[ct.TensorType(name="mix", shape=(1, 2, SEGMENT))],
        outputs=[ct.TensorType(name="stems")],
        minimum_deployment_target=ct.target.macOS14,
        convert_to="mlprogram",
        compute_precision=ct.precision.FLOAT16,
    )
    model.author = "Meta Research (HT-Demucs); converted for Rounds"
    model.license = "MIT"
    model.short_description = ("HT-Demucs 6-stem: mix [1,2,343980] at 44.1 kHz -> stems [1,6,2,343980] "
                               "(drums, bass, other, vocals, guitar, piano)")
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / "HTDemucs6S_F16.mlpackage"
    model.save(str(path))
    print(f"saved {path}")


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path("."))
