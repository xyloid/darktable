"""Tests for server.py: the four MCP tools, exercised in-process against
the fake darktable peer (no stdio transport, no darktable, no `mcp`
subprocess) by calling `FastMCP.call_tool()` directly -- the same call the
SDK's stdio dispatcher makes once a real MCP client is attached.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from mcp.server.fastmcp.exceptions import ToolError

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
        "set_module_enabled",
        "reset_module",
        "create_module_instance",
        "get_history",
        "undo",
    }


async def test_no_discovery_record_surfaces_discovery_error(tmp_path):
    app = build_server(config_dir=str(tmp_path))

    with pytest.raises(ToolError) as excinfo:
        await app.call_tool("get_current_image", {})

    assert "no live darktable discovery record" in str(excinfo.value)
