"""LocalLLM の Python (Tk) 版のチャットクライアント

    python3 python/main.py
"""

import logging

from chat_ui import main

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    main()
