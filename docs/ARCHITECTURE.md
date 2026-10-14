# How Burnos fits together

Burnos is a native Windows desktop application. Its conversation model coordinates the user interaction; separate task models perform longer work while conversation continues.

| Responsibility | Source |
| --- | --- |
| Application startup and developer probes | [`main.cpp`](../main.cpp) |
| Orb, listening state, shortcuts, and spoken controls | [`mainwindow.cpp`](../mainwindow.cpp), [`orbwidget.cpp`](../orbwidget.cpp) |
| Conversation, background tasks, memory operations | [`agent.cpp`](../agent.cpp) |
| Model requests | [`openrouter.cpp`](../openrouter.cpp) |
| Speech recognition and voice activity detection | [`sttengine.cpp`](../sttengine.cpp) |
| Speech synthesis | [`ttsengine.cpp`](../ttsengine.cpp) |
| Wake word and optional speaker matching | [`keywordspotter.cpp`](../keywordspotter.cpp), [`voicecheck.cpp`](../voicecheck.cpp) |
| Model downloads | [`installer.cpp`](../installer.cpp) |
| MCP client and Mnemosyne lifecycle | [`mcp.cpp`](../mcp.cpp) |
| Connected services | [`composio.cpp`](../composio.cpp) |
| Windows automation | [`computeruse.cpp`](../computeruse.cpp) |
| Research browser and PDF text | [`webbrowser.cpp`](../webbrowser.cpp), [`web.qrc`](../web.qrc) |
| Configuration and encrypted API-key storage | [`settings.cpp`](../settings.cpp), [`settingsdialog.cpp`](../settingsdialog.cpp) |

## Memory

The native app directly integrates Mnemosyne through its MCP client. It can start a local server or connect to a configured endpoint. Conversation records, retrieved context, and user profile information are handled through that backend. If the backend is missing or unavailable, do not assume that closing the app preserves the conversation.

The default local server listens on `127.0.0.1:8765/mcp` and uses a SQLite store under `%LOCALAPPDATA%\BurnOS\memory`. A local session secret supports authenticated memory operations. Memory retrieval can also use embeddings requested from OpenRouter. See [data handling](PRIVACY.md).

## Browser

Burnos starts an installed Chromium-based browser as a separate headless process with its own profile. It drives that process through the DevTools protocol, reads page content, and embeds PDF.js for PDF text extraction. It does not inherit your normal browser's signed-in sessions. Sites can block automation, require authentication, or expose content that cannot be extracted.

## Protocol boundary

Burnos is an MCP **client**, not the self-hosted memory server. In the reviewed v0.22.0 source, the native client requests protocol `2024-11-05`. The separate memory demonstration initialized Mnemosyne with `2025-11-25`. These are different executions; the latter does not prove that Burnos itself meets the hackathon's server-route protocol requirement. The [submission guide](hackathon/README.md) keeps that distinction explicit.
