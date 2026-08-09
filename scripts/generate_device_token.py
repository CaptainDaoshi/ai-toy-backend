import os
import sys

from dotenv import load_dotenv

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if PROJECT_DIR not in sys.path:
    sys.path.insert(0, PROJECT_DIR)

from app.security import generate_device_token


def main() -> None:
    if len(sys.argv) != 2 or not sys.argv[1].strip():
        raise SystemExit("Usage: python scripts/generate_device_token.py <device_id>")
    load_dotenv(os.path.join(PROJECT_DIR, ".env"))
    server_secret = os.getenv("DEVICE_AUTH_SECRET", "")
    if len(server_secret) < 32:
        raise SystemExit("DEVICE_AUTH_SECRET must contain at least 32 characters")
    print(generate_device_token(sys.argv[1].strip(), server_secret))


if __name__ == "__main__":
    main()
