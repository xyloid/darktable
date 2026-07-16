"""Tests for server.py: the four MCP tools, exercised in-process against
the fake darktable peer (no stdio transport, no darktable, no `mcp`
subprocess) by calling `FastMCP.call_tool()` directly -- the same call the
SDK's stdio dispatcher makes once a real MCP client is attached.
"""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path

import pytest
from mcp.server.fastmcp.exceptions import ToolError
from mcp.types import ImageContent, TextContent

from conftest import WireError, load_fixture
from darktable_mcp.server import build_server


def _write_record(config_dir: Path, server) -> Path:
    mcp_dir = config_dir / "mcp"
    mcp_dir.mkdir(parents=True, exist_ok=True)
    record_path = mcp_dir / "session-12345.json"
    record_path.write_text(
        json.dumps(
            {
                "protocol": "org.darktable.remote-edit",
                "protocol_version": 1,
                "darktable_version": "5.x",
                "pid": os.getpid(),  # this test process: guaranteed alive
                "host": "127.0.0.1",
                "port": server.port,
                "token": server.token,
                "started_at": "2026-07-05T15:04:05Z",
            }
        )
    )
    return record_path


async def _built_server(tmp_path, server):
    _write_record(tmp_path, server)
    return build_server(config_dir=str(tmp_path), connect_timeout=1.0, request_timeout=1.0)


async def call_tool_json(app, name: str, arguments: dict) -> object:
    """Calls a tool and returns its structured result.

    Every tool here is annotated `-> dict[str, Any]`, so FastMCP derives
    an output schema and `call_tool()` returns `(content, structured)`:
    a one-item text-content list (JSON, for clients that only render
    text) plus the structured dict itself. Both carry the same data;
    this helper checks that invariant once and returns the structured
    form, which is simpler for tests to assert on.
    """
    content, structured = await app.call_tool(name, arguments)
    assert len(content) == 1
    assert json.loads(content[0].text) == structured
    return structured


async def test_get_current_image_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_state_response_darkroom.json")
    server.handle_from_fixture("get_state", "get_state_response_darkroom.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_current_image", {})

    assert result == fixture["result"]


async def test_list_modules_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("list_modules_response.json")
    server.handle_from_fixture("list_modules", "list_modules_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "list_modules", {})

    assert result == fixture["result"]


async def test_list_modules_not_in_darkroom_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture("list_modules", "list_modules_error_not_in_darkroom_response.json")

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("list_modules", {})

    message = str(excinfo.value)
    assert "not_in_darkroom" in message
    assert "darkroom" in message  # the actionable hint text


async def test_get_module_schema_returns_fields_verbatim(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_module_schema_response.json")
    server.handle_from_fixture("get_module_schema", "get_module_schema_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_schema", {"module": "exposure"})

    assert result == fixture["result"]
    assert result["fields"] == fixture["result"]["fields"]


async def test_get_module_schema_unknown_module_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "get_module_schema", "get_module_schema_error_unknown_module_response.json"
    )

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_module_schema", {"module": "nonexistent_op"})

    message = str(excinfo.value)
    assert "unknown_module" in message
    assert "list_modules" in message  # the actionable hint text


async def test_get_module_params_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_module_params_response.json")
    server.handle_from_fixture("get_module_params", "get_module_params_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_params", {"module": "exposure", "instance": 0})

    assert result == fixture["result"]


async def test_get_module_params_defaults_instance_to_zero(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen_params = {}

    def handler(params):
        seen_params.update(params)
        return {"module": params["module"], "instance": params["instance"], "values": {}}

    server.handle("get_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool("get_module_params", {"module": "exposure"})

    assert seen_params == {"module": "exposure", "instance": 0}


async def test_get_module_schema_rgbcurve_semantic_fields_verbatim(tmp_path, fake_server_factory):
    """Milestone 2: semantic_fields and represented_by pass through unreshaped."""
    server = await fake_server_factory()
    fixture = load_fixture("get_module_schema_rgbcurve_response.json")
    server.handle_from_fixture("get_module_schema", "get_module_schema_rgbcurve_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_schema", {"module": "rgbcurve"})

    assert result == fixture["result"]
    assert result["semantic_fields"] == fixture["result"]["semantic_fields"]
    represented = {field["name"]: field.get("represented_by") for field in result["fields"]}
    for native in ("curve_nodes", "curve_num_nodes", "curve_type"):
        assert represented[native] == [
            "curve.master",
            "curve.red",
            "curve.green",
            "curve.blue",
        ]
    # primitive-only ops keep emitting no semantic members
    assert "semantic_fields" not in load_fixture("get_module_schema_response.json")["result"]


async def test_get_module_params_rgbcurve_semantic_values_verbatim(tmp_path, fake_server_factory):
    """Milestone 2: all four semantic_values entries pass through unreshaped."""
    server = await fake_server_factory()
    fixture = load_fixture("get_module_params_rgbcurve_response.json")
    server.handle_from_fixture("get_module_params", "get_module_params_rgbcurve_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_module_params", {"module": "rgbcurve", "instance": 0})

    assert result == fixture["result"]
    semantic = result["semantic_values"]
    assert set(semantic) == {"curve.master", "curve.red", "curve.green", "curve.blue"}
    assert semantic["curve.master"]["active"] is True
    for inactive in ("curve.red", "curve.green", "curve.blue"):
        assert semantic[inactive]["active"] is False  # present, never omitted
    for value in semantic.values():
        assert value["interpolation"] == "MONOTONE_HERMITE"  # uppercase, unchanged
        assert all(set(point) == {"x", "y"} for point in value["points"])
    # primitive-only ops keep emitting no semantic members
    assert "semantic_values" not in load_fixture("get_module_params_response.json")["result"]


async def test_set_module_enabled_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("set_module_enabled_response.json")
    server.handle_from_fixture("set_module_enabled", "set_module_enabled_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(
        app, "set_module_enabled", {"module": "exposure", "enabled": True}
    )

    assert result == fixture["result"]


async def test_set_module_enabled_threads_params(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {"module": params["module"], "instance": params["instance"], "enabled": False, "revision": 5}

    server.handle("set_module_enabled", handler)

    app = await _built_server(tmp_path, server)
    # expected_revision omitted -> must not appear in the wire params;
    # instance defaults to 0.
    await app.call_tool("set_module_enabled", {"module": "exposure", "enabled": False})

    assert seen == {"module": "exposure", "instance": 0, "enabled": False}


async def test_set_module_enabled_includes_expected_revision_when_given(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {"module": params["module"], "instance": params["instance"], "enabled": True, "revision": 9}

    server.handle("set_module_enabled", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_enabled",
        {"module": "exposure", "enabled": True, "instance": 2, "expected_revision": 8},
    )

    assert seen == {"module": "exposure", "instance": 2, "enabled": True, "expected_revision": 8}


async def test_set_module_enabled_revision_conflict_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()

    def handler(_params):
        raise WireError(
            "revision_conflict", "expected revision 30 does not match current state", retryable=True
        )

    server.handle("set_module_enabled", handler)

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("set_module_enabled", {"module": "exposure", "enabled": True})

    message = str(excinfo.value)
    assert "revision_conflict" in message
    assert "re-read state and retry" in message  # the actionable hint text


async def test_set_module_params_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("set_module_params_response.json")
    server.handle_from_fixture("set_module_params", "set_module_params_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(
        app,
        "set_module_params",
        {"module": "exposure", "values": {"exposure": 0.7, "black": -0.002}},
    )

    assert result == fixture["result"]


async def test_set_module_params_threads_params(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": params["values"],
            "revision": 5,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    # enable and expected_revision omitted -> must not appear in the wire
    # params; instance defaults to 0.
    await app.call_tool(
        "set_module_params", {"module": "exposure", "values": {"exposure": 1.25}}
    )

    assert seen == {"module": "exposure", "instance": 0, "values": {"exposure": 1.25}}


async def test_set_module_params_includes_optional_members_when_given(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": params["values"],
            "revision": 9,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {
            "module": "exposure",
            "instance": 2,
            "values": {"exposure": 0.3},
            "enable": True,
            "expected_revision": 8,
        },
    )

    assert seen == {
        "module": "exposure",
        "instance": 2,
        "values": {"exposure": 0.3},
        "enable": True,
        "expected_revision": 8,
    }


async def test_set_module_params_revision_conflict_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()

    def handler(_params):
        raise WireError(
            "revision_conflict", "expected revision 30 does not match current state", retryable=True
        )

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params", {"module": "exposure", "values": {"exposure": 1.0}}
        )

    message = str(excinfo.value)
    assert "revision_conflict" in message
    assert "re-read state and retry" in message  # the actionable hint text


async def test_set_module_params_invalid_value_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "set_module_params", "set_module_params_error_invalid_value_response.json"
    )

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params", {"module": "exposure", "values": {"exposure": 99.0}}
        )

    assert "invalid_value" in str(excinfo.value)


async def test_set_module_params_denylisted_field_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "set_module_params", "set_module_params_error_denylisted_field_response.json"
    )
    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params", {"module": "filmicrgb", "values": {"version": 3}}
        )
    message = str(excinfo.value)
    assert "unsupported_field" in message
    assert "writable flag" in message  # errors.py hint for unsupported_field


async def test_set_module_params_curves_translates_to_semantic_values(
    tmp_path, fake_server_factory
):
    """`curves` is tool-side sugar: point pairs become `{x, y}` objects,
    interpolation is upper-cased, and every entry gains `class: "curve"`;
    the wire request carries `semantic_values` and never a `curves`
    member. (The no-`curves` case is pinned by
    `test_set_module_params_threads_params`, whose exact-equality assert
    proves no `semantic_values` member appears uninvited.)"""
    server = await fake_server_factory()
    hello_response = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": {},
            "semantic_values": params["semantic_values"],
            "revision": 3,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {
            "module": "rgbcurve",
            "values": {},
            "curves": {
                "curve.master": {
                    "points": [[0.0, 0.0], [0.4, 0.5], [1.0, 1.0]],
                    "interpolation": "cubic_spline",
                },
                "curve.red": {"points": [[0.0, 0.0], [1.0, 1.0]]},
            },
        },
    )

    assert "curves" not in seen
    assert seen["semantic_values"] == {
        "curve.master": {
            "class": "curve",
            "points": [{"x": 0.0, "y": 0.0}, {"x": 0.4, "y": 0.5}, {"x": 1.0, "y": 1.0}],
            "interpolation": "CUBIC_SPLINE",
        },
        "curve.red": {
            "class": "curve",
            "points": [{"x": 0.0, "y": 0.0}, {"x": 1.0, "y": 1.0}],
        },
    }


async def test_set_module_params_curves_gated_on_curve_params_capability(
    tmp_path, fake_server_factory
):
    """A darktable whose hello does not advertise `curve_params` (the
    fake's default hello has `capabilities: []`) must be refused
    client-side: clear upgrade message, and no `set_module_params` wire
    call that the peer would reject less legibly."""
    server = await fake_server_factory()
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {
                "module": "rgbcurve",
                "values": {},
                "curves": {"curve.master": {"points": [[0.0, 0.0], [1.0, 1.0]]}},
            },
        )

    message = str(excinfo.value)
    assert "curve_params" in message
    assert "upgrade darktable" in message
    assert calls == []


async def test_set_module_params_curves_invalid_interpolation_fails_before_wire(
    tmp_path, fake_server_factory
):
    server = await fake_server_factory()
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {
                "module": "rgbcurve",
                "values": {},
                "curves": {
                    "curve.master": {
                        "points": [[0.0, 0.0], [1.0, 1.0]],
                        "interpolation": "bezier",
                    }
                },
            },
        )

    message = str(excinfo.value)
    assert "unknown interpolation" in message
    assert "CUBIC_SPLINE" in message  # the valid names are enumerated
    assert calls == []
    # Translation is pure client-side and runs before connect: the fake
    # server never even saw a connection.
    assert server.connections_seen == 0


async def test_set_module_params_vectors_translates_to_semantic_values(
    tmp_path, fake_server_factory
):
    """`vectors` is tool-side sugar mirroring `curves`: each entry becomes
    `{"class": "vector", "values": [float...]}` under `semantic_values`."""
    server = await fake_server_factory()
    hello_response = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": {},
            "semantic_values": params["semantic_values"],
            "revision": 3,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {
            "module": "colorbalancergb",
            "values": {},
            "vectors": {"lift": [1.0, 1.1, 1.0, 0.95]},
        },
    )

    assert "vectors" not in seen
    assert seen["semantic_values"] == {
        "lift": {"class": "vector", "values": [1.0, 1.1, 1.0, 0.95]},
    }


async def test_set_module_params_curves_and_vectors_merge_into_semantic_values(
    tmp_path, fake_server_factory
):
    """`curves` and `vectors` given in the same call merge into one
    `semantic_values` dict on the wire."""
    server = await fake_server_factory()
    hello_response = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": {},
            "semantic_values": params["semantic_values"],
            "revision": 4,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {
            "module": "colorbalancergb",
            "values": {},
            "curves": {"curve.master": {"points": [[0.0, 0.0], [1.0, 1.0]]}},
            "vectors": {"lift": [1.0, 1.1, 1.0, 0.95]},
        },
    )

    assert seen["semantic_values"] == {
        "curve.master": {
            "class": "curve",
            "points": [{"x": 0.0, "y": 0.0}, {"x": 1.0, "y": 1.0}],
        },
        "lift": {"class": "vector", "values": [1.0, 1.1, 1.0, 0.95]},
    }


async def test_set_module_params_curves_and_vectors_overlap_fails_before_wire(
    tmp_path, fake_server_factory
):
    """A semantic id given in both `curves` and `vectors` is rejected
    client-side, before any wire traffic."""
    server = await fake_server_factory()
    hello_response = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "values": {},
                "curves": {"lift": {"points": [[0.0, 0.0], [1.0, 1.0]]}},
                "vectors": {"lift": [1.0, 1.1, 1.0, 0.95]},
            },
        )

    message = str(excinfo.value)
    assert "lift" in message
    assert calls == []
    assert server.connections_seen == 0


async def test_set_module_params_vectors_gated_on_vector_params_capability(
    tmp_path, fake_server_factory
):
    """A darktable whose hello does not advertise `vector_params` (the
    fake's default hello has `capabilities: []`) must be refused
    client-side: clear upgrade message, and no `set_module_params` wire
    call that the peer would reject less legibly."""
    server = await fake_server_factory()
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {
                "module": "colorbalancergb",
                "values": {},
                "vectors": {"lift": [1.0, 1.1, 1.0, 0.95]},
            },
        )

    message = str(excinfo.value)
    assert "vector_params" in message
    assert "upgrade darktable" in message
    assert calls == []


async def test_set_module_params_curves_alone_not_gated_on_vector_params(
    tmp_path, fake_server_factory
):
    """A darktable advertising `curve_params` but not `vector_params` must
    still accept a curves-only call: the two gates are independent."""
    server = await fake_server_factory()

    def hello_override(params, req_id):
        return {
            "id": req_id,
            "ok": True,
            "result": {
                "protocol_version": 1,
                "darktable_version": "5.x",
                "pid": 12345,
                "capabilities": ["curve_params"],
            },
        }

    server.hello_override = hello_override
    seen = {}

    def handler(params):
        seen.update(params)
        return {
            "module": params["module"],
            "instance": params["instance"],
            "enabled": True,
            "values": {},
            "semantic_values": params["semantic_values"],
            "revision": 5,
        }

    server.handle("set_module_params", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "set_module_params",
        {
            "module": "rgbcurve",
            "values": {},
            "curves": {"curve.master": {"points": [[0.0, 0.0], [1.0, 1.0]]}},
        },
    )

    assert seen["semantic_values"] == {
        "curve.master": {
            "class": "curve",
            "points": [{"x": 0.0, "y": 0.0}, {"x": 1.0, "y": 1.0}],
        },
    }


@pytest.mark.parametrize(
    "bad_vectors,expected_name",
    [
        ({"lift": []}, "lift"),
        ({"lift": "not-a-list"}, "lift"),
        ({"lift": [1.0, True, 1.0, 0.95]}, "lift"),
        ({"lift": [1.0, float("nan"), 1.0, 0.95]}, "lift"),
        ({"lift": [1.0, float("inf"), 1.0, 0.95]}, "lift"),
        ({"lift": [1.0, "not-a-number", 1.0, 0.95]}, "lift"),
    ],
    ids=[
        "empty-list",
        "non-list",
        "bool-element",
        "nan-element",
        "inf-element",
        "non-numeric-element",
    ],
)
async def test_set_module_params_vectors_rejects_invalid_entries(
    tmp_path, fake_server_factory, bad_vectors, expected_name
):
    server = await fake_server_factory()
    hello_response = load_fixture("hello_response.json")
    server.hello_override = lambda params, req_id: {**hello_response, "id": req_id}
    calls = []
    server.handle("set_module_params", lambda params: calls.append(params) or {})

    app = await _built_server(tmp_path, server)
    with pytest.raises(ToolError) as excinfo:
        await app.call_tool(
            "set_module_params",
            {"module": "colorbalancergb", "values": {}, "vectors": bad_vectors},
        )

    message = str(excinfo.value)
    assert expected_name in message
    assert calls == []
    # Validation is pure client-side and runs before connect: the fake
    # server never even saw a connection.
    assert server.connections_seen == 0


async def test_reset_module_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("reset_module_response.json")
    server.handle_from_fixture("reset_module", "reset_module_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "reset_module", {"module": "exposure"})

    assert result == fixture["result"]
    # reset returns post-reset values (not instance_name).
    assert "values" in result


async def test_create_module_instance_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("create_module_instance_response.json")
    server.handle_from_fixture("create_module_instance", "create_module_instance_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "create_module_instance", {"module": "exposure"})

    assert result == fixture["result"]


async def test_create_module_instance_threads_copy_params(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {"module": params["module"], "instance": 1, "instance_name": "1", "enabled": True, "revision": 7}

    server.handle("create_module_instance", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool(
        "create_module_instance", {"module": "exposure", "copy_params": True, "source_instance": 3}
    )

    assert seen == {"module": "exposure", "source_instance": 3, "copy_params": True}


async def test_create_module_instance_not_supported_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture(
        "create_module_instance",
        "create_module_instance_error_instance_not_supported_response.json",
    )

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("create_module_instance", {"module": "demosaic"})

    message = str(excinfo.value)
    assert "instance_not_supported" in message
    assert "multiple instances" in message  # the actionable hint text


async def test_get_history_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("get_history_response.json")
    server.handle_from_fixture("get_history", "get_history_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "get_history", {"limit": 20})

    assert result == fixture["result"]
    # model-oriented metadata only: no parameter blobs in the items.
    for item in result["items"]:
        assert "params" not in item
        assert "values" not in item


async def test_get_history_defaults_limit_to_twenty(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen = {}

    def handler(params):
        seen.update(params)
        return {"revision": 1, "items": []}

    server.handle("get_history", handler)

    app = await _built_server(tmp_path, server)
    await app.call_tool("get_history", {})

    assert seen == {"limit": 20}


async def test_undo_returns_wire_result(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    fixture = load_fixture("undo_response.json")
    server.handle_from_fixture("undo", "undo_response.json")

    app = await _built_server(tmp_path, server)
    result = await call_tool_json(app, "undo", {"expected_revision": 33})

    assert result == fixture["result"]


async def test_undo_revision_conflict_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture("undo", "undo_error_revision_conflict_response.json")

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("undo", {"expected_revision": 30})

    message = str(excinfo.value)
    assert "revision_conflict" in message
    assert "re-read state and retry" in message


async def test_render_preview_returns_native_image_content(tmp_path, fake_server_factory):
    """The plan's binding requirement: MCP returns NATIVE image content,
    never a base64 text blob for the model to choke on."""
    server = await fake_server_factory()
    fixture = load_fixture("render_preview_response.json")
    server.handle_from_fixture("render_preview", "render_preview_response.json")

    app = await _built_server(tmp_path, server)
    result = await app.call_tool("render_preview", {})

    # unstructured-only tool: a one-item content list, no structured tuple
    assert isinstance(result, list)
    assert len(result) == 1
    block = result[0]
    assert isinstance(block, ImageContent)
    assert block.type == "image"
    assert block.mimeType == "image/jpeg"
    # the image bytes round-trip the wire base64 exactly
    assert base64.b64decode(block.data) == base64.b64decode(fixture["result"]["data"])


async def test_render_preview_threads_and_clamps_params(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen: list[dict] = []
    fixture = load_fixture("render_preview_response.json")

    def handler(params):
        seen.append(dict(params))
        return fixture["result"]

    server.handle("render_preview", handler)

    app = await _built_server(tmp_path, server)

    # defaults are made explicit on the wire
    await app.call_tool("render_preview", {})
    assert seen[-1] == {"max_px": 1024, "quality": 85}

    # out-of-range values are clamped client-side too (the server clamps
    # anyway; doing it here keeps requests honest and self-describing)
    await app.call_tool("render_preview", {"max_px": 10, "quality": 200})
    assert seen[-1] == {"max_px": 64, "quality": 95}

    await app.call_tool("render_preview", {"max_px": 5000, "quality": 10})
    assert seen[-1] == {"max_px": 2048, "quality": 50}


async def test_render_preview_failure_surfaces_hint(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    server.handle_from_fixture("render_preview", "render_preview_error_preview_failed_response.json")

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("render_preview", {})

    assert "preview_failed" in str(excinfo.value)


async def test_get_scopes_returns_mixed_content(tmp_path, fake_server_factory):
    """get_scopes returns MIXED content (design spec tool table: 'summaries
    and MCP image content'): one JSON text block carrying the numeric data
    -- revision, profile, histogram statistics, per-image metadata (the
    docstring-promised data render_preview's M39 note discards) -- plus a
    native ImageContent block per rendered scope, never base64 text."""
    server = await fake_server_factory()
    fixture = load_fixture("compute_scopes_response.json")
    server.handle_from_fixture("compute_scopes", "compute_scopes_response.json")

    app = await _built_server(tmp_path, server)
    result = await app.call_tool("get_scopes", {})

    # unstructured-only tool: a plain content list
    assert isinstance(result, list)
    # first block: the JSON numeric summary
    text_block = result[0]
    assert isinstance(text_block, TextContent)
    summary = json.loads(text_block.text)
    assert summary["revision"] == 34
    assert summary["source"] == "final_preview"
    assert summary["color_profile"] == "linear Rec2020 RGB"
    assert summary["roi"] == "full_image"
    assert summary["histogram"]["luminance_percentiles"]["p50"] == 0.41
    assert summary["histogram"]["channel_means"]["red"] == 0.46
    # image metadata stays in the text block, base64 payload does not
    assert summary["waveform"]["image"]["width"] == 360
    assert summary["waveform"]["image"]["height"] == 256
    assert "data" not in summary["waveform"]["image"]
    assert summary["parade"]["image"]["width"] == 1080
    assert summary["vectorscope"]["image"]["width"] == 512

    # then one native image block per rendered scope, in wire order
    image_blocks = result[1:]
    assert len(image_blocks) == 3  # the fixture carries waveform + parade + vectorscope
    for block in image_blocks:
        assert isinstance(block, ImageContent)
        assert block.type == "image"
        assert block.mimeType == "image/png"
    # the image bytes round-trip the wire base64 exactly, in wire order
    for block, name in zip(image_blocks, ("waveform", "parade", "vectorscope")):
        assert base64.b64decode(block.data) == base64.b64decode(
            fixture["result"][name]["image"]["data"]
        )


async def test_get_scopes_keyed_imageless_scope_does_not_crash(tmp_path, fake_server_factory):
    """An image scope requested with include_images=false comes back keyed
    but without an "image" member (a minimal {"image_size": N} metadata
    object). The tool must not crash: the metadata object stays in the JSON
    text block and produces no ImageContent."""
    server = await fake_server_factory()

    def handler(_params):
        return {
            "revision": 5,
            "source": "final_preview",
            "color_profile": "linear Rec2020 RGB",
            "roi": "full_image",
            "histogram": {"bins": 256},
            "waveform": {"image_size": 512},
            "parade": {"image_size": 512},
            "vectorscope": {"image_size": 512},
        }

    server.handle("compute_scopes", handler)
    app = await _built_server(tmp_path, server)

    result = await app.call_tool("get_scopes", {"include_images": False})

    assert isinstance(result, list)
    # no image blocks -- every scope was imageless
    assert len(result) == 1
    summary = json.loads(result[0].text)
    for name in ("waveform", "parade", "vectorscope"):
        assert summary[name] == {"image_size": 512}


async def test_get_scopes_threads_and_clamps_params(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    seen: list[dict] = []
    fixture = load_fixture("compute_scopes_response.json")

    def handler(params):
        seen.append(dict(params))
        return fixture["result"]

    server.handle("compute_scopes", handler)

    app = await _built_server(tmp_path, server)

    # defaults are made explicit on the wire: all four scopes, summary and
    # images on, bins off, image_size 512
    await app.call_tool("get_scopes", {})
    assert seen[-1] == {
        "scopes": ["histogram", "waveform", "parade", "vectorscope"],
        "include_summary": True,
        "include_bins": False,
        "include_images": True,
        "image_size": 512,
    }

    # a subset threads through verbatim; image_size clamps client-side too
    await app.call_tool(
        "get_scopes",
        {"scopes": ["histogram"], "include_bins": True, "image_size": 10},
    )
    assert seen[-1]["scopes"] == ["histogram"]
    assert seen[-1]["include_bins"] is True
    assert seen[-1]["image_size"] == 128

    await app.call_tool("get_scopes", {"image_size": 5000})
    assert seen[-1]["image_size"] == 1024


async def test_get_scopes_scope_failed_surfaces_hint(tmp_path, fake_server_factory):
    """The empty-capture-slot failure (fresh darkroom, no preview yet)
    surfaces the retryable scope_failed with its recovery hint."""
    server = await fake_server_factory()
    server.handle_from_fixture("compute_scopes", "compute_scopes_error_scope_failed_response.json")

    app = await _built_server(tmp_path, server)

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_scopes", {})

    message = str(excinfo.value)
    assert "scope_failed" in message
    assert "retry" in message


async def test_list_tools_exposes_exactly_the_plan_tool_names(tmp_path, fake_server_factory):
    server = await fake_server_factory()
    app = await _built_server(tmp_path, server)

    tools = await app.list_tools()
    names = {tool.name for tool in tools}

    assert names == {
        "get_current_image",
        "list_modules",
        "get_module_schema",
        "get_module_params",
        "set_module_params",
        "set_module_enabled",
        "reset_module",
        "create_module_instance",
        "get_history",
        "undo",
        "render_preview",
        "get_scopes",
    }


async def test_no_discovery_record_surfaces_discovery_error(tmp_path):
    app = build_server(config_dir=str(tmp_path))

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_current_image", {})

    assert "no live darktable discovery record" in str(excinfo.value)
