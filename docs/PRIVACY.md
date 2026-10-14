# Your data and your controls

Burnos mixes local processing with connected services. This describes the reviewed v0.22.1 implementation, not a guarantee about every external tool you may connect.

| Data or action | Where it goes |
| --- | --- |
| Speech recognition, speech synthesis, wake detection, speaker matching | Local speech runtimes and models |
| Conversation and reasoning context | OpenRouter and the selected model provider |
| Desktop screenshots and tool results | May be included in model requests |
| Persistent memories | Configured Mnemosyne backend; the default arrangement uses local SQLite |
| Semantic retrieval | May send text to OpenRouter's embedding endpoint |
| Research | Websites opened by a separate browser profile |
| Connected services | Composio and/or the MCP endpoints you configure |

New model downloads use `%LOCALAPPDATA%\BurnOS\models`; a complete existing `%ProgramData%\BurnOS` cache is reused. MCP configuration is `%APPDATA%\BurnOS\mcp.json`; a custom `BURNOS_MCP_CONFIG` overrides it. The local memory store and session-secret file are under `%LOCALAPPDATA%\BurnOS`. API keys are stored through Windows DPAPI for the current user. This does not protect secrets from every process already running as that user. Uninstalling the app preserves these user data and settings.

Clicking mute changes the conversation state. Local wake-word or spoken-command detection can still use the microphone. Exit Burnos or use Windows microphone controls when you need a full microphone stop. Speaker matching is not a security boundary.

Use fictional information for recordings. Review generated files and consequential actions. Avoid sharing live logs, registry exports, memory databases, or MCP configurations that contain credentials. Asking to forget a memory is not a promise to erase provider logs, backups, or every derived copy.
