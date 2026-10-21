# Your data and your controls

Burnos mixes local processing with connected services. This describes Burnos v0.27.0 with Mnemosyne 1.7.0, not a guarantee about every external tool you may connect.

| Data or action | Where it goes |
| --- | --- |
| Speech recognition, speech synthesis, wake detection, speaker matching | Local speech runtimes and models |
| Conversation and reasoning context | OpenRouter and the selected model provider; or, for a `claude:` or `codex:` model, Anthropic or OpenAI through the official CLI you are logged in to (Burnos never reads its login); or, for a `chatgpt:` model, OpenAI directly with your Sign in with ChatGPT login, whose tokens Burnos keeps encrypted with Windows DPAPI |
| Desktop screenshots and tool results | May be included in model requests |
| Persistent memories | Configured Mnemosyne backend; the default arrangement uses local SQLite |
| Semantic retrieval | May send text to OpenRouter's embedding endpoint |
| Skills | Original skill and book files are retained by the configured memory server, locally in the default setup, and are not executed on import. Plain Skill import does not require framework generation. When a BurnSkill framework is generated, source text is sent through OpenRouter to the selected model provider or through the authenticated Codex CLI, according to the generation model. Skill instructions can also be included in later model requests |
| Research | Websites opened by a separate browser profile |
| Connected services | Composio and/or the MCP endpoints you configure |

Official Windows packages include voice models in the application's `models` folder. Source builds can download models into `%LOCALAPPDATA%\BurnOS\models` or reuse a complete `%ProgramData%\BurnOS` cache. MCP configuration is `%APPDATA%\BurnOS\mcp.json`; a custom `BURNOS_MCP_CONFIG` overrides it. The local memory store and session-secret file are under `%LOCALAPPDATA%\BurnOS`. API keys are stored through Windows DPAPI for the current user. This does not protect secrets from every process already running as that user. Uninstalling removes bundled models with the app but preserves personal memory, settings, and separately downloaded caches.

Clicking mute changes the conversation state. Local wake-word or spoken-command detection can still use the microphone. Exit Burnos or use Windows microphone controls when you need a full microphone stop. Speaker matching is not a security boundary.

Use fictional information for recordings. Review generated files and consequential actions. Avoid sharing live logs, registry exports, memory databases, or MCP configurations that contain credentials. Asking to forget a memory is not a promise to erase provider logs, backups, or every derived copy.
