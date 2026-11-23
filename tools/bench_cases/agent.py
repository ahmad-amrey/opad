"""gui_benches cases of the AI integration's settings (app/AgentSettings.cpp, app/AgentClients.cpp)."""

# Claude Desktop's and Claude Code's settings in folders of the case's own (APPDATA on Windows, HOME on macOS,
# XDG_CONFIG_HOME on Linux; CLAUDE_CONFIG_DIR): the bench writes and rewrites them, never a real configuration.
ISOLATED = {"APPDATA": "{root}/agent-connect/appdata", "XDG_CONFIG_HOME": "{root}/agent-connect/xdg", "HOME": "{root}/agent-connect/home",
            "CLAUDE_CONFIG_DIR": "{root}/agent-connect/claude"}

CASES = [
    # Connect an AI client: opened while no client is connected; Claude Code, Claude Desktop and Codex each with its state
    # (registered for an old data folder, its program missing); Point it at this OPAD rewrites only Claude Desktop's
    # mcpServers.opad (a .bak copy), Remove takes it out; step 3 turns access on; step 4's prompt names the document.
    ("agent-connect", "empty", {"OPAD_BENCH_AGENT_CONNECT": "{prefix}", **ISOLATED}),
    # AI activity (app/AgentActivityBench.cpp): an assistant through the bridge's socket and what the user sees of it, step by
    # step: no panel on connect, a toast; the chip per state; live_state adds no row; lookups fold; a look's picture; a task
    # as one group row, Discard; retried errors make no row; look only's needs-you row and Allow changes; a change in words
    # with Undo on the newest step only, "Claude: ..." as the undo step, ai_client on the op; the viewport's preview chip and
    # its Discard; Properties' "Claude (AI) · hh:mm"; the timeline marker's Show in AI activity; Show / Back with the camera;
    # "AI" in the Annotations author filter; Stop's divider and toast with no row left running; undo / redo reply labels as
    # before; failure rows naming their own call; a failed commit's row.
    ("agent-activity", "empty", {"OPAD_BENCH_AGENT_ACTIVITY": "{prefix}", "OPAD_BENCH_AGENT": "{root}/activity-agent"}),
    ("agent-activity-ar", "empty", {"OPAD_BENCH_AGENT_ACTIVITY": "{prefix}", "OPAD_BENCH_AGENT": "{root}/activity-agent-ar", "OPAD_LANG": "ar"}),
]
