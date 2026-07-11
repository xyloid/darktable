"""Visual-feedback gates: render_preview and the M47 scopes acceptance gate.

render_preview must return a decodable JPEG of the requested size from the
real export path. The scopes gate (RATIFIED, load-bearing) proves that
``compute_scopes`` returns real data after the preview pipe runs, that an
exposure edit shifts the remote histogram in the expected direction, and
that the rendered scope images decode.

On "agree with the GUI": the harness runs headless, so it cannot read the
GUI scope widget's pixels to diff them. What is *testable* here -- and what
this gate asserts -- is coherence at the source: both the GUI panel and
``compute_scopes`` derive from the same captured preview buffer stamped with
the same process-local revision (design/internals SS9), and the remote
histogram statistics move coherently with a known exposure edit. A
pixel-level GUI-vs-remote diff would need a scripted darkroom with the
scopes panel visible and a screen grab; it is called out as manual-only in
the report's verification checklist. This is a flagged interpretation of the
gate's "agree" clause for controller ratification.
"""

from __future__ import annotations

import base64
import io
import struct

import pytest

from . import harness

pytestmark = pytest.mark.integration

JPEG_SOI = b"\xff\xd8\xff"
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def _jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Parse width/height from a JPEG's first SOF marker -- no image library
    dependency (Pillow is not a test requirement)."""
    assert data[:3] == JPEG_SOI, "not a JPEG (bad SOI marker)"
    i = 2
    n = len(data)
    while i + 9 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        # SOF0..SOF3, SOF5..SOF7, SOF9..SOF11, SOF13..SOF15 carry dimensions.
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            height = struct.unpack(">H", data[i + 5 : i + 7])[0]
            width = struct.unpack(">H", data[i + 7 : i + 9])[0]
            return width, height
        seg_len = struct.unpack(">H", data[i + 2 : i + 4])[0]
        i += 2 + seg_len
    raise AssertionError("no SOF marker found in JPEG")


async def test_render_preview_returns_decodable_jpeg(darktable_session):
    """render_preview live: a JPEG whose longest edge honours ``max_px``."""
    async with harness.connected_client(darktable_session) as client:
        result = await client.call("render_preview", {"max_px": 256, "quality": 85})
        assert result["mime_type"] == "image/jpeg"
        data = base64.b64decode(result["data"])
        assert data[:3] == JPEG_SOI
        width, height = _jpeg_dimensions(data)
        assert max(width, height) <= 256
        assert (width, height) == (result["width"], result["height"])
        assert isinstance(result["revision"], int)


async def test_exposure_edit_shifts_remote_histogram(darktable_session):
    """M47 acceptance gate: a downward then upward exposure edit moves the
    remote histogram brighter, coherently with the edit, and the scope
    images decode."""
    async with harness.connected_client(darktable_session) as client:
        # Point 1: pull exposure down. wait_for_scopes forces the preview
        # pipe to push a buffer stamped with this mutation's revision.
        dark = await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": -2.0}, "enable": True},
        )
        scopes_dark = await harness.wait_for_scopes(client, dark["revision"])
        mean_dark = scopes_dark["histogram"]["channel_means"]
        p50_dark = scopes_dark["histogram"]["luminance_percentiles"]["p50"]

        # Point 2: push exposure up by 4 stops relative to point 1.
        bright = await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 2.0}},
        )
        scopes_bright = await harness.wait_for_scopes(
            client, bright["revision"], scopes=("histogram", "waveform", "vectorscope"),
            include_images=True,
        )
        mean_bright = scopes_bright["histogram"]["channel_means"]
        p50_bright = scopes_bright["histogram"]["luminance_percentiles"]["p50"]

        # The histogram must move brighter in the expected direction.
        for channel in ("red", "green", "blue"):
            assert mean_bright[channel] > mean_dark[channel], (
                f"{channel} mean did not increase with +4 stops of exposure: "
                f"{mean_dark[channel]} -> {mean_bright[channel]}"
            )
        assert p50_bright > p50_dark

        # The buffer is at least as new as our mutation. >= not ==: trailing
        # DEVELOP_HISTORY_CHANGE signals can advance the revision after the
        # mutation echo returns (see harness.undo_latest), stamping the
        # buffer with a newer revision.
        assert scopes_bright["revision"] >= bright["revision"]

        # The rendered scope images decode as PNG at bounded size.
        for name in ("waveform", "vectorscope"):
            entry = scopes_bright.get(name)
            assert isinstance(entry, dict), f"missing {name} scope"
            img = entry["image"]
            png = base64.b64decode(img["data"])
            assert png[:8] == PNG_MAGIC, f"{name} image is not a PNG"
            assert 0 < img["width"] <= 256 and 0 < img["height"] <= 256


async def test_compute_scopes_shares_one_revision(darktable_session):
    """Every scope in one response derives from one buffer and carries one
    revision (design-spec coherence requirement)."""
    async with harness.connected_client(darktable_session) as client:
        state = await client.call("get_state")
        # Nudge state so a fresh coherent buffer is guaranteed to exist.
        edit = await client.call(
            "set_module_params",
            {"module": "exposure", "instance": 0, "values": {"exposure": 0.5}, "enable": True},
        )
        result = await harness.wait_for_scopes(
            client, edit["revision"], scopes=("histogram", "waveform", "parade", "vectorscope"),
            include_images=True,
        )
        # >= not ==: post-echo revision drift (see harness.undo_latest) can
        # stamp the buffer newer than the mutation echo. The one-revision
        # coherence this test pins is structural: compute_scopes returns a
        # single top-level revision for ALL scopes in the response.
        assert result["revision"] >= edit["revision"]
        assert result["source"] == "final_preview"
