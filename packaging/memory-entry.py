"""Standalone entry point for the bundled Mnemosyne server."""
from multiprocessing import freeze_support

if __name__ == "__main__":
    freeze_support()
    from mnemosyne.mcp_server import main
    main()
