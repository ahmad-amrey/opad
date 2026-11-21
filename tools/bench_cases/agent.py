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
]
